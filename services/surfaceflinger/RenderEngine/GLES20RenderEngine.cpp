/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define Warp_Mesh_Resolution_X 64
#define Warp_Mesh_Resolution_Y 64
#define VR_Buffer_Stride 12
#define Screen_X 1000.0f
#define Screen_Y 1000.0f

//_test_Similarity
#define Check_Width 1024/2
#define Check_Height 768
#define Check_Len 8

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <ui/Rect.h>

#include <utils/String8.h>
#include <utils/Trace.h>

#include <cutils/compiler.h>
#include <gui/ISurfaceComposer.h>
#include <math.h>
#include <cutils/properties.h>

#include "GLES20RenderEngine.h"
#include "Program.h"
#include "ProgramCache.h"
#include "Description.h"
#include "Mesh.h"
#include "Texture.h"
#include "../DisplayDevice.h"

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------
GLES20RenderEngine::GLES20RenderEngine() :
        mVpWidth(0),
        mVpHeight(0),
        tname(0),
        name(0),
        useRightFBO(false),
        leftCheck(NULL),
        rightCheck(NULL),
        context(NULL)
{
#ifdef ENABLE_VR
    initVRInfoTable();
    //test similarity
    leftCheck  = new uint32_t[Check_Width*Check_Height];
    rightCheck = new uint32_t[Check_Width*Check_Height];
#endif

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, mMaxViewportDims);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    struct pack565 {
        inline uint16_t operator() (int r, int g, int b) const {
            return (r<<11)|(g<<5)|b;
        }
    } pack565;

    const uint16_t protTexData[] = { 0 };
    glGenTextures(1, &mProtectedTexName);
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0,
            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, protTexData);

    //mColorBlindnessCorrection = M;
}

GLES20RenderEngine::~GLES20RenderEngine() {
}


size_t GLES20RenderEngine::getMaxTextureSize() const {
    return mMaxTextureSize;
}

size_t GLES20RenderEngine::getMaxViewportDims() const {
    return
        mMaxViewportDims[0] < mMaxViewportDims[1] ?
            mMaxViewportDims[0] : mMaxViewportDims[1];
}

void GLES20RenderEngine::setViewportAndProjection(
        size_t vpw, size_t vph, Rect sourceCrop, size_t hwh, bool yswap,
        Transform::orientation_flags rotation) {

    size_t l = sourceCrop.left;
    size_t r = sourceCrop.right;

    // In GL, (0, 0) is the bottom-left corner, so flip y coordinates
    size_t t = hwh - sourceCrop.top;
    size_t b = hwh - sourceCrop.bottom;

    mat4 m;
    if (yswap) {
        m = mat4::ortho(l, r, t, b, 0, 1);
    } else {
        m = mat4::ortho(l, r, b, t, 0, 1);
    }

    // Apply custom rotation to the projection.
    float rot90InRadians = 2.0f * static_cast<float>(M_PI) / 4.0f;
    switch (rotation) {
        case Transform::ROT_0:
            break;
        case Transform::ROT_90:
            m = mat4::rotate(rot90InRadians, vec3(0,0,1)) * m;
            break;
        case Transform::ROT_180:
            m = mat4::rotate(rot90InRadians * 2.0f, vec3(0,0,1)) * m;
            break;
        case Transform::ROT_270:
            m = mat4::rotate(rot90InRadians * 3.0f, vec3(0,0,1)) * m;
            break;
        default:
            break;
    }

    glViewport(0, 0, vpw, vph);
    mState.setProjectionMatrix(m);
    mVpWidth = vpw;
    mVpHeight = vph;
}

void GLES20RenderEngine::setupLayerBlending(
    bool premultipliedAlpha, bool opaque, int alpha) {

    mState.setPremultipliedAlpha(premultipliedAlpha);
    mState.setOpaque(opaque);
    mState.setPlaneAlpha(alpha / 255.0f);

    if (alpha < 0xFF || !opaque) {
        glEnable(GL_BLEND);
       // glBlendFunc(premultipliedAlpha ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
       glBlendFuncSeparate(premultipliedAlpha ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    } else {
        glDisable(GL_BLEND);
    }
}

void GLES20RenderEngine::setupDimLayerBlending(int alpha) {
    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setColor(0, 0, 0, alpha/255.0f);
    mState.disableTexture();

    if (alpha == 0xFF) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
}

void GLES20RenderEngine::setupLayerTexturing(const Texture& texture) {
    GLuint target = texture.getTextureTarget();
    glBindTexture(target, texture.getTextureName());
    GLenum filter = GL_NEAREST;
    if (texture.getFiltering()) {
        filter = GL_LINEAR;
    }
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);

    mState.setTexture(texture);
}

void GLES20RenderEngine::setupLayerBlackedOut() {
    glBindTexture(GL_TEXTURE_2D, mProtectedTexName);
    Texture texture(Texture::TEXTURE_2D, mProtectedTexName);
    texture.setDimensions(1, 1); // FIXME: we should get that from somewhere
    mState.setTexture(texture);
}

void GLES20RenderEngine::disableTexturing() {
    mState.disableTexture();
}

void GLES20RenderEngine::disableBlending() {
    glDisable(GL_BLEND);
}


void GLES20RenderEngine::bindImageAsFramebuffer(EGLImageKHR image,
        uint32_t* texName, uint32_t* fbName, uint32_t* status) {
    GLuint tname, name;
    // turn our EGLImage into a texture
    glGenTextures(1, &tname);
    glBindTexture(GL_TEXTURE_2D, tname);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);

    // create a Framebuffer Object to render into
    glGenFramebuffers(1, &name);
    glBindFramebuffer(GL_FRAMEBUFFER, name);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tname, 0);

    *status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    *texName = tname;
    *fbName = name;

#ifdef ENABLE_VR
    ALOGD("ljt bindImageAsFramebuffer");
    mVR.fboCaptureScreen = name;
    mVR.captureTriggle = true;
#endif
}

void GLES20RenderEngine::unbindFramebuffer(uint32_t texName, uint32_t fbName) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbName);
    glDeleteTextures(1, &texName);
}

void GLES20RenderEngine::setupFillWithColor(float r, float g, float b, float a) {
    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setColor(r, g, b, a);
    mState.disableTexture();
    glDisable(GL_BLEND);
}

void GLES20RenderEngine::drawMesh(const Mesh& mesh) {
//    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }

}

#ifdef ENABLE_VR
void GLES20RenderEngine::initVRInfoTable(){
    mVR.dpyId=0;
    mVR.startFlag=true;
    for(int dpy=0;dpy<DISPLAY_NUM;dpy++){
        mVR.MeshData[dpy].bufferHandle=0;
        mVR.MeshData[dpy].last_x=0;
        mVR.MeshData[dpy].last_y=0;
        mVR.MeshData[dpy].reCompute=false;

        mVR.leftFbo[dpy]=0;
        mVR.leftTex[dpy]=0;

        mVR.rightFbo[dpy]=0;
        mVR.rightTex[dpy]=0;

        mVR.fboWidth[dpy]=0;
        mVR.fboHeight[dpy]=0;

        mVR.fboCaptureScreen = -1;
        mVR.captureTriggle = false;

        testCount=0;

    }
}

void GLES20RenderEngine::drawFBLeft() {
    ProgramCache::getInstance().useProgram(mState);

    loadTexCoordsFB();
    loadVerCoordsFB(LEFT);
    glDrawArrays(Mesh::TRIANGLES, 0, Warp_Mesh_Resolution_X * Warp_Mesh_Resolution_Y * 6);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLES20RenderEngine::drawFBRight() {
    ProgramCache::getInstance().useProgram(mState);

    loadTexCoordsFB();
    loadVerCoordsFB(RIGHT);
    glDrawArrays(Mesh::TRIANGLES, 0,Warp_Mesh_Resolution_X * Warp_Mesh_Resolution_Y * 6);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLES20RenderEngine::drawLeftFBO(const Mesh& mesh) {
    glBindFramebuffer(GL_FRAMEBUFFER, mVR.leftFbo[mVR.dpyId]);

    mState.setDeform(false);
    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }
}

void GLES20RenderEngine::drawRightFBO(const Mesh& mesh) {
    glBindFramebuffer(GL_FRAMEBUFFER, mVR.rightFbo[mVR.dpyId]);

    mState.setDeform(false);
    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }
}

void GLES20RenderEngine::loadTexCoordsFB(){
    glBindBuffer(GL_ARRAY_BUFFER, mVR.MeshData[mVR.dpyId].bufferHandle);

    //rgb texCoords
    glEnableVertexAttribArray(Program::texCoords_r);
    glVertexAttribPointer(Program::texCoords_r,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(4 * sizeof(GLfloat)));

    glEnableVertexAttribArray(Program::texCoords_g);
    glVertexAttribPointer(Program::texCoords_g,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(6 * sizeof(GLfloat)));

    glEnableVertexAttribArray(Program::texCoords_b);
    glVertexAttribPointer(Program::texCoords_b,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(8 * sizeof(GLfloat)));

    glEnableVertexAttribArray(Program::texCoords_offdeform);
    glVertexAttribPointer(Program::texCoords_offdeform,
        2,
        GL_FLOAT, GL_FALSE,
        VR_Buffer_Stride*sizeof(float),
        (void*)(10 * sizeof(GLfloat)));
}

void GLES20RenderEngine::loadVerCoordsFB(int mode){
    glBindBuffer(GL_ARRAY_BUFFER, mVR.MeshData[mVR.dpyId].bufferHandle);

    if(LEFT == mode){
        glEnableVertexAttribArray(Program::position);
        glVertexAttribPointer(Program::position,
            2,
            GL_FLOAT, GL_FALSE,
            VR_Buffer_Stride*sizeof(float),
            (void*)(0 * sizeof(GLfloat)));
    }

    if(RIGHT == mode){
        glEnableVertexAttribArray(Program::position);
        glVertexAttribPointer(Program::position,
            2,
            GL_FLOAT, GL_FALSE,
            VR_Buffer_Stride*sizeof(float),
            (void*)(2 * sizeof(GLfloat)));
    }
}

vec2 GLES20RenderEngine::genDeformTex(vec2 tex,float k1,float k2){
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.height", value, "0.5");
    float Scale = atof(value);
    property_get("sys.3d.ipd_scale", value, "0");
    float ipdByScale = atof(value);
    if(ipdByScale < 0)
        ipdByScale  = (-1.0) * ipdByScale;

    float xyRatio = (mVR.MeshData[mVR.dpyId].last_x * Scale)/ ((mVR.MeshData[mVR.dpyId].last_y/2) * (1.0 - 0.5 * ipdByScale));

    tex = tex - vec2(0.5);
    tex.x = tex.x * xyRatio;

    float len = length(tex);
    float r2 = pow(len,2.0);
    float r4 = pow(len,4.0);
    tex = tex * (1 + k1 * r2 + k2 * r4);

    tex.x = tex.x / xyRatio;
    tex = tex + vec2(0.5);

    return tex;
}

GLuint GLES20RenderEngine::genVRMeshBuffer(float width,float height){
    struct Vertex
    {
        vec2 left_position;
        vec2 right_position;
        vec2 uv_red;
        vec2 uv_green;
        vec2 uv_blue;
        vec2 uv_offdeform;
    };
    static Vertex v[(Warp_Mesh_Resolution_X + 1) * (Warp_Mesh_Resolution_Y + 1)];

    char value[PROPERTY_VALUE_MAX];
    //orientation
    property_get("sys.hwc.force3d.primary", value, "2");
    int orient = atoi(value);

    //r g b deform property set
    property_get("sys.3d.deform_red1", value, "0");
    float rk1 = atof(value);
    property_get("sys.3d.deform_red2", value, "0");
    float rk2 = atof(value);
    property_get("sys.3d.deform_green1", value, "0");
    float gk1 = atof(value);
    property_get("sys.3d.deform_green2", value, "0");
    float gk2 = atof(value);
    property_get("sys.3d.deform_blue1", value, "0");
    float bk1 = atof(value);
    property_get("sys.3d.deform_blue2", value, "0");
    float bk2 = atof(value);

    //height property set
    property_get("sys.3d.height", value, "0.5");
    float heightScale = atof(value);

    /*IPD property set
      IPD Offset priority is higher than Scale
      if Offset != 0, we set the Scale = 0*/
    property_get("sys.3d.ipd_offset", value, "0");
    float ipdByOffset = atof(value);
    property_get("sys.3d.ipd_scale", value, "0");
    float ipdByScale = atof(value);
    if(ipdByOffset!=0 && ipdByScale!=0)
    ipdByScale = 0.0f;

    float finalHeight = 0;
    float finalWidth = 0;
    float ipdMaxSize = 0;

    //size of fbo
    if(2==orient){
        ipdMaxSize = (height/2)/10.0f;
        finalHeight = height * 0.5;
        finalWidth  = width  * heightScale;
    }
    if(1==orient){
        ipdMaxSize = (width/2)/10.0f;
        finalHeight = height * heightScale;
        finalWidth  = width  * 0.5;
    }

    // Compute vertices
    int vi = 0;
    for (int yi = 0; yi <= Warp_Mesh_Resolution_Y; yi++)
    for (int xi = 0; xi <= Warp_Mesh_Resolution_X; xi++)
        {
            float x = float(xi) / float(Warp_Mesh_Resolution_X);
            float y = float(yi) / float(Warp_Mesh_Resolution_Y);

            vec2 tex = vec2(x,y);

            //cellphone orient mode
            if(2==orient){
                //vec position's range is frome width & height,not 0~1
                v[vi].left_position  = vec2(finalWidth*x + width * ((1 - heightScale) * 0.5),finalHeight*y);
                v[vi].right_position = vec2(finalWidth*x + width * ((1 - heightScale) * 0.5),finalHeight*y+finalHeight);

                v[vi].uv_red    = genDeformTex(tex,rk1,rk2);
                v[vi].uv_green  = genDeformTex(tex,gk1,gk2);
                v[vi].uv_blue   = genDeformTex(tex,bk1,bk2);
                //when enabled HDMI, we need disable deform
                v[vi].uv_offdeform = genDeformTex(tex,0.0f,0.0f);

                //if enable IPD_Offset
                v[vi].left_position.y  = v[vi].left_position.y  + ipdMaxSize * ipdByOffset;
                v[vi].right_position.y = v[vi].right_position.y - ipdMaxSize * ipdByOffset;
                v[vi].left_position.y  = ( v[vi].left_position.y < finalHeight) ?  v[vi].left_position.y : finalHeight;
                v[vi].right_position.y = (v[vi].right_position.y > finalHeight) ? v[vi].right_position.y : finalHeight;

                //if enable IPD_Scale
                if(ipdByScale > 0){
                    float screenScale = 1.0 - 0.5 * ipdByScale;
                    v[vi].left_position.y  = v[vi].left_position.y * screenScale;
                    v[vi].right_position.y = v[vi].right_position.y * screenScale + (height/4.0f) * ipdByScale * 2.0f;
                }
                if(ipdByScale < 0){
                    float ipdByScale_abs = (-1.0f) * ipdByScale;
                    float screenScale = 1.0 - 0.5 * ipdByScale_abs;
                    v[vi].left_position.y  = v[vi].left_position.y * screenScale + (height/4.0f) * ipdByScale_abs;
                    v[vi].right_position.y = v[vi].right_position.y * screenScale + (height/4.0f) * ipdByScale_abs;
                }
            }

            //tablet orient mode
            if(1==orient){
                //vec position's range is frome width & height,not 0~1
                v[vi].left_position  = vec2(finalWidth*x ,finalHeight*y + height * ((1 - heightScale) * 0.5));
                v[vi].right_position = vec2(finalWidth*x + finalWidth,finalHeight*y + height * ((1 - heightScale) * 0.5));

                v[vi].uv_red    = genDeformTex(tex,rk1,rk2);
                v[vi].uv_green  = genDeformTex(tex,gk1,gk2);
                v[vi].uv_blue   = genDeformTex(tex,bk1,bk2);
                //when enabled HDMI, we need disable deform
                v[vi].uv_offdeform = genDeformTex(tex,0.0f,0.0f);

                //if enable IPD_Offset
                v[vi].left_position.x  = v[vi].left_position.x  + ipdMaxSize * ipdByOffset;
                v[vi].right_position.x = v[vi].right_position.x - ipdMaxSize * ipdByOffset;
                v[vi].left_position.x  = ( v[vi].left_position.x < finalWidth) ?  v[vi].left_position.x : finalWidth;
                v[vi].right_position.x = (v[vi].right_position.x > finalWidth) ? v[vi].right_position.x : finalWidth;

                //if enable IPD_Scale
                if(ipdByScale > 0){
                    float screenScale = 1.0 - 0.5 * ipdByScale;
                    v[vi].left_position.x  = v[vi].left_position.x * screenScale;
                    v[vi].right_position.x = v[vi].right_position.x * screenScale + (width/4.0f) * ipdByScale * 2.0f;
                }
                if(ipdByScale < 0){
                    float ipdByScale_abs = (-1.0f) * ipdByScale;
                    float screenScale = 1.0 - 0.5 * ipdByScale_abs;
                    v[vi].left_position.x  = v[vi].left_position.x * screenScale + (width/4.0f) * ipdByScale_abs;
                    v[vi].right_position.x = v[vi].right_position.x * screenScale + (width/4.0f) * ipdByScale_abs;
                }
            }

            vi++;
        }

    // Generate faces from vertices
    static Vertex f[Warp_Mesh_Resolution_X * Warp_Mesh_Resolution_Y * 6];
    int fi = 0;
    for (int yi = 0; yi < Warp_Mesh_Resolution_Y; yi++)
    for (int xi = 0; xi < Warp_Mesh_Resolution_X; xi++)
    {
        Vertex v0 = v[(yi    ) * (Warp_Mesh_Resolution_X + 1) + xi    ];
        Vertex v1 = v[(yi    ) * (Warp_Mesh_Resolution_X + 1) + xi + 1];
        Vertex v2 = v[(yi + 1) * (Warp_Mesh_Resolution_X + 1) + xi + 1];
        Vertex v3 = v[(yi + 1) * (Warp_Mesh_Resolution_X + 1) + xi    ];
        f[fi++] = v0;
        f[fi++] = v1;
        f[fi++] = v2;
        f[fi++] = v2;
        f[fi++] = v3;
        f[fi++] = v0;
    }

    GLuint result = 0;
    glGenBuffers(1, &result);
    glBindBuffer(GL_ARRAY_BUFFER, result);
    glBufferData(GL_ARRAY_BUFFER, sizeof(f), f, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return result;
}

void GLES20RenderEngine::enableRightFBO(bool key){
    if(key)
        useRightFBO = true;
    else
        useRightFBO = false;
}

//judge whether we need to generate MeshPrimary.bufferHandle once again or not
bool GLES20RenderEngine::checkVRPropertyChanged(){
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.property_update", value, "1");
    int result = atoi(value);

    if(result){
        property_set("sys.3d.property_update","0");
        return true;//re-compute
    }else
        return false;//don't re-compute
}

void GLES20RenderEngine::clearFbo(){
    if(mVR.fboCaptureScreen != -1){
        glBindFramebuffer(GL_FRAMEBUFFER, mVR.fboCaptureScreen);
        glClearColor(0,0,0,0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mVR.leftFbo[mVR.dpyId]);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, mVR.rightFbo[mVR.dpyId]);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);

    //After we clear fbo,we must bind the original fbo to current
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

//setTargetDpyConfig must be called before beingGroup
void GLES20RenderEngine::setTargetDpyXY(int x, int y, int dpyId){
    //record the dpyId from SurfaceFlinger::doDisplayComposition
    mVR.dpyId = dpyId;

    if((mVR.MeshData[dpyId].last_x != x) || (mVR.MeshData[dpyId].last_y != y)){
        mVR.MeshData[dpyId].reCompute = true;
        mVR.MeshData[dpyId].last_x = x;
        mVR.MeshData[dpyId].last_y = y;
    }else{
        mVR.MeshData[dpyId].reCompute = false;
    }
}

void GLES20RenderEngine::createFBO(int dpyId){
    int width = mVR.MeshData[dpyId].last_x * 0.5;
    int height = mVR.MeshData[dpyId].last_y * 0.5;

    /*create Left FBO
      0x812D:   GL_CLAMP_TO_BORDER
      0x1004:   GL_TEXTURE_BORDER_COLOR*/

    //left FBO
    glGenTextures(1, &mVR.leftTex[dpyId]);
    glBindTexture(GL_TEXTURE_2D, mVR.leftTex[dpyId]);
    //    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
    //    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812D);
    //    glTexParameterfv( GL_TEXTURE_2D, 0x1004, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
    glGenFramebuffers(1, &mVR.leftFbo[dpyId]);
    glBindFramebuffer(GL_FRAMEBUFFER, mVR.leftFbo[dpyId]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVR.leftTex[dpyId], 0);

    //right FBO
    glGenTextures(1, &mVR.rightTex[dpyId]);
    glBindTexture(GL_TEXTURE_2D, mVR.rightTex[dpyId]);
    //    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812D);
    //    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812D);
    //    glTexParameterfv( GL_TEXTURE_2D, 0x1004, color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
    glGenFramebuffers(1, &mVR.rightFbo[dpyId]);
    glBindFramebuffer(GL_FRAMEBUFFER, mVR.rightFbo[dpyId]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVR.rightTex[dpyId], 0);

    //Stack FBO:nothing to be used
    if(GL_FALSE == glIsFramebuffer(name)){
        glGenTextures(1, &tname);
        glBindTexture(GL_TEXTURE_2D, tname);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

        //original fbo, delete it then we get error
        glGenFramebuffers(1, &name);
    }
}

void GLES20RenderEngine::updateFBOSize(int dpyId){
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.height", value, "0.5");
    float heightScale = atof(value);

    property_get("sys.hwc.force3d.primary", value, "0");
    int orient = atoi(value);

    if(1==orient){
        mVR.fboWidth[dpyId] = mVR.MeshData[dpyId].last_x * 0.5;
        mVR.fboHeight[dpyId] = mVR.MeshData[dpyId].last_y * heightScale;
    }
    if(2==orient){
        mVR.fboWidth[dpyId] = mVR.MeshData[dpyId].last_x * heightScale;
        mVR.fboHeight[dpyId] = mVR.MeshData[dpyId].last_y * 0.5;
    }

    glBindTexture(GL_TEXTURE_2D, mVR.leftTex[dpyId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVR.fboWidth[dpyId], mVR.fboHeight[dpyId], 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
    glBindFramebuffer(GL_FRAMEBUFFER, mVR.leftFbo[dpyId]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVR.leftTex[dpyId], 0);

    glBindTexture(GL_TEXTURE_2D, mVR.rightTex[dpyId]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVR.fboWidth[dpyId], mVR.fboHeight[dpyId], 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
    glBindFramebuffer(GL_FRAMEBUFFER, mVR.rightFbo[dpyId]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mVR.rightTex[dpyId], 0);
}

bool GLES20RenderEngine::queryCaptureScreen() {
    return mVR.captureTriggle;

}

void GLES20RenderEngine::beginGroup(const mat4& colorTransform,int mode) {
    mVR.captureTriggle = false;
    if(mode == 3){
        GLuint tname, name;
        // create the texture
        glGenTextures(1, &tname);
        glBindTexture(GL_TEXTURE_2D, tname);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVpWidth, mVpHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

        // create a Framebuffer Object to render into
        glGenFramebuffers(1, &name);
        glBindFramebuffer(GL_FRAMEBUFFER, name);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tname, 0);

        Group group;
        group.texture = tname;
        group.fbo = name;
        group.width = mVpWidth;
        group.height = mVpHeight;

        group.colorTransform = colorTransform;

        mGroupStack.push(group);

        return;
    }

    if(mVR.startFlag){
        mVR.startFlag=false;
        for(int dpyId=0;dpyId<DISPLAY_NUM;dpyId++){
            createFBO(dpyId);
        }
        _test_CreateCheckFBO();
    }

    if(checkVRPropertyChanged()){
        for(int dpyId=0;dpyId<DISPLAY_NUM;dpyId++){
            mVR.MeshData[dpyId].bufferHandle = genVRMeshBuffer(mVR.MeshData[dpyId].last_x,mVR.MeshData[dpyId].last_y);
        }
    }

    if(mVR.MeshData[mVR.dpyId].reCompute){
        mVR.MeshData[mVR.dpyId].reCompute = false;
        mVR.MeshData[mVR.dpyId].bufferHandle = genVRMeshBuffer(mVR.MeshData[mVR.dpyId].last_x,mVR.MeshData[mVR.dpyId].last_y);
    }

    updateFBOSize(mVR.dpyId);

//    glBindFramebuffer(GL_FRAMEBUFFER, mVR.leftFbo[0]);

    Group group;

    group.texture = tname;
    group.fbo = name;
    group.width = mVpWidth;
    group.height = mVpHeight;

    mGroupStack.push(group);

    if(mode > 1)
    {
        group.colorTransform = colorTransform;
    }

}

void GLES20RenderEngine::endGroup(int mode) {
    if(mode == 3){
        const Group group(mGroupStack.top());
        mGroupStack.pop();

        // activate the previous render target
        GLuint fbo = 0;
        if (!mGroupStack.isEmpty()) {
            fbo = mGroupStack.top().fbo;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        // set our state
        Texture texture(Texture::TEXTURE_2D, group.texture);
        texture.setDimensions(group.width, group.height);
        glBindTexture(GL_TEXTURE_2D, group.texture);

        mState.setPlaneAlpha(1.0f);
        mState.setPremultipliedAlpha(true);
        mState.setOpaque(false);
        mState.setTexture(texture);
        mState.setColorMatrix(group.colorTransform);

        glDisable(GL_BLEND);

        Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2, 2);
        Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
        Mesh::VertexArray<vec2> texCoord(mesh.getTexCoordArray<vec2>());
        position[0] = vec2(0, 0);
        position[1] = vec2(group.width, 0);
        position[2] = vec2(group.width, group.height);
        position[3] = vec2(0, group.height);
        texCoord[0] = vec2(0, 0);
        texCoord[1] = vec2(1, 0);
        texCoord[2] = vec2(1, 1);
        texCoord[3] = vec2(0, 1);
        drawMesh(mesh);

        // reset color matrix
        mState.setColorMatrix(mat4());

        // free our fbo and texture
        glDeleteFramebuffers(1, &group.fbo);
        glDeleteTextures(1, &group.texture);

        return;
    }

    if(mode == 4 && mVR.captureTriggle){
        int id = 3;
        glBindFramebuffer(GL_FRAMEBUFFER, mVR.fboCaptureScreen);
        mVR.captureTriggle = false;

        mState.setPlaneAlpha(1.0f);
        mState.setPremultipliedAlpha(true);
        mState.setOpaque(false);
//        mState.setColorMatrix(group.colorTransform);

        mState.setVREnable(true);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glDisable(GL_BLEND);

        //set Texture and State for the Shader
        Texture LeftTexture(Texture::TEXTURE_2D, mVR.leftTex[mVR.dpyId]);
        //LeftTexture.setDimensions(group.width, group.height);
        glBindTexture(GL_TEXTURE_2D, mVR.leftTex[mVR.dpyId]);

        Texture RightTexture(Texture::TEXTURE_2D, mVR.rightTex[mVR.dpyId]);
        //RightTexture.setDimensions(group.width, group.height);
        glBindTexture(GL_TEXTURE_2D, mVR.rightTex[mVR.dpyId]);

        if(mVR.dpyId == DisplayDevice::DISPLAY_PRIMARY){
            mState.setDeform(true);
            mState.setFogborder(true);
            //whether enable dispersion
            char value[PROPERTY_VALUE_MAX];
            property_get("debug.sf.dispersion", value, "0");
            int dispersionEnabled = atoi(value);
            if(dispersionEnabled)
                mState.setDisper(true);
            else
                mState.setDisper(false);
        }

        if(mVR.dpyId == DisplayDevice::DISPLAY_EXTERNAL){
            //we don't need Dispersion & Deform & Fogborder enabled for HDMI
            mState.setFogborder(false);
            mState.setDeform(false);
            mState.setDisper(false);
        }

        //draw fb for left eye
        glBindTexture(GL_TEXTURE_2D, mVR.leftTex[mVR.dpyId]);
        mState.setTexture(LeftTexture);
        drawFBLeft();

        //draw fb for right eye
        if(useRightFBO){
            glBindTexture(GL_TEXTURE_2D, mVR.rightTex[mVR.dpyId]);
            mState.setTexture(RightTexture);
            drawFBRight();
            //disable right FBO
            enableRightFBO(false);
        }else{
            mState.setTexture(LeftTexture);
            drawFBRight();
        }

        // reset color matrix
        mState.setColorMatrix(mat4());
        mState.setVREnable(false);

        return;
    }

    const Group group(mGroupStack.top());
    mGroupStack.pop();

    // activate the previous render target
    GLuint fbo = 0;
    if (!mGroupStack.isEmpty()) {
        fbo = mGroupStack.top().fbo;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setColorMatrix(group.colorTransform);

    mState.setVREnable(true);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glDisable(GL_BLEND);

    //set Texture and State for the Shader
    Texture LeftTexture(Texture::TEXTURE_2D, mVR.leftTex[mVR.dpyId]);
    //LeftTexture.setDimensions(group.width, group.height);
    glBindTexture(GL_TEXTURE_2D, mVR.leftTex[mVR.dpyId]);

    Texture RightTexture(Texture::TEXTURE_2D, mVR.rightTex[mVR.dpyId]);
    //RightTexture.setDimensions(group.width, group.height);
    glBindTexture(GL_TEXTURE_2D, mVR.rightTex[mVR.dpyId]);

    if(mVR.dpyId == DisplayDevice::DISPLAY_PRIMARY){
        mState.setDeform(true);
        mState.setFogborder(true);
        //whether enable dispersion
        char value[PROPERTY_VALUE_MAX];
        property_get("debug.sf.dispersion", value, "0");
        int dispersionEnabled = atoi(value);
        if(dispersionEnabled)
            mState.setDisper(true);
        else
            mState.setDisper(false);
    }

    if(mVR.dpyId == DisplayDevice::DISPLAY_EXTERNAL){
        //we don't need Dispersion & Deform & Fogborder enabled for HDMI
        mState.setFogborder(false);
        mState.setDeform(false);
        mState.setDisper(false);
    }

    //draw fb for left eye
    glBindTexture(GL_TEXTURE_2D, mVR.leftTex[mVR.dpyId]);
    mState.setTexture(LeftTexture);
    drawFBLeft();

    //draw fb for right eye
    if(useRightFBO){
        glBindTexture(GL_TEXTURE_2D, mVR.rightTex[mVR.dpyId]);
        mState.setTexture(RightTexture);
        drawFBRight();
        //disable right FBO
        enableRightFBO(false);
    }else{
        mState.setTexture(LeftTexture);
        drawFBRight();
    }

    // reset color matrix
    mState.setColorMatrix(mat4());
    mState.setVREnable(false);
}

void GLES20RenderEngine::isVideo3dFormat(int mode){
    bool skip = false;
    float total = 0.0;

    if(mVR.dpyId==1){
        return;
    }

    if(mode==-1){
        testCount=0;
        for(int i=0;i<10;i++){
            score_list[i]=0.0;
        }
        return;
    }

    if(testCount%DETECT_3D_RATE==0){
        score_list[testCount/DETECT_3D_RATE] = _test_Similarity();
        if(score_list[testCount/DETECT_3D_RATE]==-1){
            ALOGD("auto-detect-3d-video: skip!");
            testCount=0;
            for(int i=0;i<10;i++){
                score_list[i]=0.0;
            }
            return;
        }
    }

    testCount++;

    if(testCount>=10*DETECT_3D_RATE){
        testCount=0;
        for(int i=0;i<10;i++){
            total += score_list[i];
        }
        score=total/10.0;
        ALOGD("auto-detect-3d-video:_similarity = %f",score);
        if(score>0.6){
            property_set("sys.3d.3dvideo","1");
        }else{
            property_set("sys.3d.3dvideo","0");
        }
    }
}

void GLES20RenderEngine::_test_CreateCheckFBO(){
    int w = mVR.MeshData[0].last_x * 0.25;
    int h = mVR.MeshData[0].last_y * 0.5;

    for(int i=0;i<=ZOOM_OUT_LEVEL;i++){
        if(i==ZOOM_OUT_LEVEL){
            w = 8;
            h = 8;
        }

        _test_simi.fboWidth[i] = w;
        _test_simi.fboHeight[i] = h;

        glGenTextures(1, &_test_simi.checkLeftTex[i]);
        glBindTexture(GL_TEXTURE_2D, _test_simi.checkLeftTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glGenFramebuffers(1, &_test_simi.checkLeftFBO[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.checkLeftFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _test_simi.checkLeftTex[i], 0);

        glGenTextures(1, &_test_simi.checkRightTex[i]);
        glBindTexture(GL_TEXTURE_2D, _test_simi.checkRightTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
        glGenFramebuffers(1, &_test_simi.checkRightFBO[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.checkRightFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _test_simi.checkRightTex[i], 0);

        w = w/2;
        h = h/2;
    }
}

float GLES20RenderEngine::_test_Similarity(){
    Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2, 2);
    Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    Mesh::VertexArray<vec2> texCoord(mesh.getTexCoordArray<vec2>());

    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.3dvideo", value, "0");
    int temp = atoi(value);

    if(temp){
        position[0] = vec2(0, 0);
        position[1] = vec2(Check_Width, 0);
        position[2] = vec2(Check_Width, Check_Height);
        position[3] = vec2(0, Check_Height);
        texCoord[0] = vec2(0, 0);
        texCoord[1] = vec2(1, 0);
        texCoord[2] = vec2(1, 1);
        texCoord[3] = vec2(0, 1);
        _test_drawMeshLeftCheckFBO(mesh,0,1);
        _test_drawMeshRightCheckFBO(mesh,0,1);
    }else{
        position[0] = vec2(0, 0);
        position[1] = vec2(Check_Width, 0);
        position[2] = vec2(Check_Width, Check_Height);
        position[3] = vec2(0, Check_Height);
        texCoord[0] = vec2(0, 0);
        texCoord[1] = vec2(1, 0);
        texCoord[2] = vec2(1, 0.5);
        texCoord[3] = vec2(0, 0.5);
        _test_drawMeshLeftCheckFBO(mesh,0,0);
        texCoord[0] = vec2(0, 0.5);
        texCoord[1] = vec2(1, 0.5);
        texCoord[2] = vec2(1, 1);
        texCoord[3] = vec2(0, 1);
        _test_drawMeshRightCheckFBO(mesh,0,0);
    }

    for(int i=1;i<=ZOOM_OUT_LEVEL;i++){
        position[0] = vec2(0, 0);
        position[1] = vec2(_test_simi.fboWidth[i], 0);
        position[2] = vec2(_test_simi.fboWidth[i], _test_simi.fboHeight[i]);
        position[3] = vec2(0, _test_simi.fboHeight[i]);
        texCoord[0] = vec2(0, 0);
        texCoord[1] = vec2(1, 0);
        texCoord[2] = vec2(1, 1);
        texCoord[3] = vec2(0, 1);
        _test_drawMeshLeftCheckFBO(mesh,i,0);
        _test_drawMeshRightCheckFBO(mesh,i,0);
    }

    return _test_isSimilaryImages(leftCheck,rightCheck);

}


float GLES20RenderEngine::_test_isSimilaryImages(const uint32_t* frame1, const uint32_t * frame2){
    float temp = 0;
    uint8_t greyValue = 0;
    uint8_t *p1 = (uint8_t *)frame1;
    uint8_t *p2 = (uint8_t *)frame2;
    uint8_t *grey1 = new uint8_t[Check_Len*Check_Len];
    uint8_t *grey2 = new uint8_t[Check_Len*Check_Len];
    bool *fingerPrint1 = new bool[(Check_Len-1)*Check_Len];
    bool *fingerPrint2 = new bool[(Check_Len-1)*Check_Len];
    bool *resultMap = new bool[(Check_Len-1)*Check_Len];
    int resultNum = 0;
    uint8_t greyTotal = 0;

    //transform image to grep scale
    for(int i=0;i<(Check_Len*Check_Len);i++){
        temp = 0.2989*(*p1) + 0.5870*(*(p1+1)) + 0.1140*(*(p1+2));
        greyValue = (int8_t)temp;
        *(grey1+i) = greyValue;

        temp = 0.2989*(*p2) + 0.5870*(*(p2+1)) + 0.1140*(*(p2+2));
        greyValue = (int8_t)temp;
        *(grey2+i) = greyValue;

            ALOGD("simi:greyValue =%d",greyValue);
            ALOGD("simi:%d,%d,%d",*p2,*(p2+1),*(p2+2));

        p1 = p1 + 4;
        p2 = p2 + 4;
        greyTotal += *(grey1+i);
    }

    //skip black or white frame
    float greyAverage = (float)greyTotal/(Check_Len*Check_Len);
    float greyVariance = 0;
    for(int i=0;i<(Check_Len*Check_Len);i++){
        greyVariance += ((float)*(grey1+i)-greyAverage);
    }
    if(greyVariance < 20.0)
        return -1.0;

    //draw fingerPrint map
    for(int i=0;i<Check_Len;i++)
        for(int j=1;j<Check_Len;j++){
            if ((*(grey1+i*Check_Len+j)) > (*(grey1+i*Check_Len+j-1)))
                *(fingerPrint1+i*(Check_Len-1)+j-1) = true;
            else
                *(fingerPrint1+i*(Check_Len-1)+j-1) = false;

            if ((*(grey2+i*Check_Len+j)) > (*(grey2+i*Check_Len+j-1)))
                *(fingerPrint2+i*(Check_Len-1)+j-1) = true;
            else
                *(fingerPrint2+i*(Check_Len-1)+j-1) = false;
        }

    //compare two fingerPrint
    for(int i=0;i<((Check_Len-1)*Check_Len);i++){
        if((*(fingerPrint1+i)) !=  (*(fingerPrint2+i))){
            *(resultMap+i) = true;
        }else{
            *(resultMap+i) = false;
        }
    }

    //add weight value when "true zone" is connected
    for(int i=0;i<((Check_Len-1)*Check_Len);i++){
        int weight = 0;
        if((*(resultMap+i))==true){
            weight=1;
            bool toleft=false,toright=false,toup=false,todown=false;
            if(i%(Check_Len-1) - 1 >= 0)              toleft  = *(resultMap+i-1);
            if(i%(Check_Len-1) + 1 <= (Check_Len-1))      toright = *(resultMap+i+1);
            if((i+1)/Check_Len - 1 >= 0)              toup    = *(resultMap+i-(Check_Len-1));
            if((i+1)/Check_Len + 1 <= Check_Len)              todown  = *(resultMap+i+(Check_Len-1));

            if((toleft==true)||(toright==true)||(toup==true)||(todown==true))
                weight = 3;
            }
        resultNum = resultNum + weight;
    }

    float similarRatio = 1.0-((float)resultNum/(float)(Check_Len*Check_Len));
    if(similarRatio < 0)
        similarRatio = 0;

    return similarRatio;
}

void GLES20RenderEngine::_test_drawMeshLeftCheckFBO(const Mesh& mesh, int fboId, int mode) {

    glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.checkLeftFBO[fboId]);

    GLuint texId;
    if(fboId==0){
        texId = mVR.leftTex[mVR.dpyId];
    }else{
        texId = _test_simi.checkLeftTex[fboId-1];
    }

//    Texture LeftTexture(Texture::TEXTURE_2D, mVR.leftTex[0]);
    Texture LeftTexture(Texture::TEXTURE_2D, texId);
//    LeftTexture.setDimensions(_test_simi.fboWidth, _test_simi.fboHeight);

//    glBindTexture(GL_TEXTURE_2D, mVR.leftTex[0]);
    glBindTexture(GL_TEXTURE_2D, texId);

    mState.setTexture(LeftTexture);

    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }
    glFinish();
    glFlush();

    //glBindFramebuffer(GL_FRAMEBUFFER, 0);
    //glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.leftFbo);
    //glBindTexture(GL_TEXTURE_2D, _test_simi.checkLeftTex);
    //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 384,512, 0, GL_RGBA, GL_UNSIGNED_BYTE,0);
    //glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, _test_simi.fboWidth, _test_simi.fboHeight/2, 0);

#if 1
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.dump", value, "4");
    float dumpId = atoi(value);
    if(fboId==dumpId){
        glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.checkLeftFBO[fboId]);
        glReadPixels(0, 0, _test_simi.fboWidth[fboId], _test_simi.fboHeight[fboId], GL_RGBA, GL_UNSIGNED_BYTE, leftCheck);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

//        int n = _test_simi.fboWidth[fboId] * _test_simi.fboHeight[fboId] * 4;
//        FILE *fp1;
//        if ((fp1 = fopen("data/1.bin", "w+")) == NULL)
//        {
//            ALOGD("haha: can't open 1.bin!!!!!");
//            //return 1;
//        }
//        fwrite(leftCheck, n, 1, fp1);
//        fclose(fp1);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
#endif

}

void GLES20RenderEngine::_test_drawMeshRightCheckFBO(const Mesh& mesh, int fboId, int mode) {
    glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.checkRightFBO[fboId]);

    GLuint texId;
    if(fboId==0 && mode==0){
        texId = mVR.leftTex[mVR.dpyId];
    }else if(fboId==0 && mode==1){
        texId = mVR.rightTex[mVR.dpyId];
    }else{
        texId = _test_simi.checkRightTex[fboId-1];
    }

//    Texture LeftTexture(Texture::TEXTURE_2D, mVR.leftTex[0]);
    Texture LeftTexture(Texture::TEXTURE_2D, texId);
//    LeftTexture.setDimensions(_test_simi.fboWidth, _test_simi.fboHeight);

//    glBindTexture(GL_TEXTURE_2D, mVR.leftTex[0]);
    glBindTexture(GL_TEXTURE_2D, texId);

    mState.setTexture(LeftTexture);

    ProgramCache::getInstance().useProgram(mState);

    if (mesh.getTexCoordsSize()) {
        glEnableVertexAttribArray(Program::texCoords);
        glVertexAttribPointer(Program::texCoords,
                mesh.getTexCoordsSize(),
                GL_FLOAT, GL_FALSE,
                mesh.getByteStride(),
                mesh.getTexCoords());
    }

    glVertexAttribPointer(Program::position,
            mesh.getVertexSize(),
            GL_FLOAT, GL_FALSE,
            mesh.getByteStride(),
            mesh.getPositions());

    glDrawArrays(mesh.getPrimitive(), 0, mesh.getVertexCount());

    if (mesh.getTexCoordsSize()) {
        glDisableVertexAttribArray(Program::texCoords);
    }

    glFinish();
    glFlush();

#if 1
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.3d.dump", value, "4");
    float dumpId = atoi(value);
    if(fboId==dumpId){
        glBindFramebuffer(GL_FRAMEBUFFER, _test_simi.checkRightFBO[fboId]);
        glReadPixels(0, 0, _test_simi.fboWidth[fboId], _test_simi.fboHeight[fboId], GL_RGBA, GL_UNSIGNED_BYTE, rightCheck);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

//        int n = _test_simi.fboWidth[fboId] * _test_simi.fboHeight[fboId] * 4;
//        FILE *fp1;
//        if ((fp1 = fopen("data/2.bin", "w+")) == NULL)
//        {
//            ALOGD("haha: can't open 2.bin!!!!!");
//            //return 1;
//        }
//        fwrite(rightCheck, n, 1, fp1);
//        fclose(fp1);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
#endif

}

#else
void GLES20RenderEngine::beginGroup(const mat4& colorTransform) {
    GLuint tname, name;
    // create the texture
    glGenTextures(1, &tname);
    glBindTexture(GL_TEXTURE_2D, tname);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mVpWidth, mVpHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

    // create a Framebuffer Object to render into
    glGenFramebuffers(1, &name);
    glBindFramebuffer(GL_FRAMEBUFFER, name);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tname, 0);

    Group group;
    group.texture = tname;
    group.fbo = name;
    group.width = mVpWidth;
    group.height = mVpHeight;

    group.colorTransform = colorTransform;

    mGroupStack.push(group);
}

void GLES20RenderEngine::endGroup() {
    const Group group(mGroupStack.top());
    mGroupStack.pop();

    // activate the previous render target
    GLuint fbo = 0;
    if (!mGroupStack.isEmpty()) {
        fbo = mGroupStack.top().fbo;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // set our state
    Texture texture(Texture::TEXTURE_2D, group.texture);
    texture.setDimensions(group.width, group.height);
    glBindTexture(GL_TEXTURE_2D, group.texture);

    mState.setPlaneAlpha(1.0f);
    mState.setPremultipliedAlpha(true);
    mState.setOpaque(false);
    mState.setTexture(texture);
    mState.setColorMatrix(group.colorTransform);

    glDisable(GL_BLEND);

    Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2, 2);
    Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    Mesh::VertexArray<vec2> texCoord(mesh.getTexCoordArray<vec2>());
    position[0] = vec2(0, 0);
    position[1] = vec2(group.width, 0);
    position[2] = vec2(group.width, group.height);
    position[3] = vec2(0, group.height);
    texCoord[0] = vec2(0, 0);
    texCoord[1] = vec2(1, 0);
    texCoord[2] = vec2(1, 1);
    texCoord[3] = vec2(0, 1);
    drawMesh(mesh);

    // reset color matrix
    mState.setColorMatrix(mat4());

    // free our fbo and texture
    glDeleteFramebuffers(1, &group.fbo);
    glDeleteTextures(1, &group.texture);
}
#endif



void GLES20RenderEngine::dump(String8& result) {
    RenderEngine::dump(result);
}

// ---------------------------------------------------------------------------
}; // namespace android
// ---------------------------------------------------------------------------

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif
