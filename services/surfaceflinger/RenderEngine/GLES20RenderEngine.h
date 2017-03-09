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


#ifndef SF_GLES20RENDERENGINE_H_
#define SF_GLES20RENDERENGINE_H_

typedef unsigned int uint32_t;

#include <stdint.h>
#include <sys/types.h>

#include <GLES2/gl2.h>
#include <Transform.h>

#include "Program.h"
#include "Mesh.h"
#include "RenderEngine.h"
#include "ProgramCache.h"
#include "Description.h"

#include <ctime>

#define LEFT 1
#define RIGHT 2
#define DISPLAY_NUM 2
#define ZOOM_OUT_LEVEL 4
#define DETECT_3D_RATE 10

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------

class String8;
class Mesh;
class Texture;

class GLES20RenderEngine : public RenderEngine {
    GLuint mProtectedTexName;
    GLint mMaxViewportDims[2];
    GLint mMaxTextureSize;
    GLuint mVpWidth;
    GLuint mVpHeight;
    GLuint VRMeshBuffer;
    GLuint tname, name;
    GLuint leftFbo, leftTex;
    GLuint rightFbo, rightTex;
    bool useRightFBO;
    void * context;

	struct MeshBuffer{
		GLuint bufferHandle;
		GLuint last_x;
		GLuint last_y;
		GLboolean reCompute;
	};

    struct VRInfoTable {
		GLboolean startFlag;
		int dpyId;

		struct MeshBuffer MeshData[DISPLAY_NUM];

		GLuint leftFbo[DISPLAY_NUM];
		GLuint leftTex[DISPLAY_NUM];

		GLuint rightFbo[DISPLAY_NUM];
		GLuint rightTex[DISPLAY_NUM];

		GLuint fboWidth[DISPLAY_NUM];
		GLuint fboHeight[DISPLAY_NUM];

		GLboolean captureTriggle;
		GLuint fboCaptureScreen;
    } mVR;

	//add for similarity check
	struct VRSimilarity{
		GLuint checkLeftFBO[ZOOM_OUT_LEVEL+1];
		GLuint checkRightFBO[ZOOM_OUT_LEVEL+1];
		GLuint checkLeftTex[ZOOM_OUT_LEVEL+1];
		GLuint checkRightTex[ZOOM_OUT_LEVEL+1];
		GLuint fboWidth[ZOOM_OUT_LEVEL+1];
		GLuint fboHeight[ZOOM_OUT_LEVEL+1];
	} _test_simi;

	float score_list[10];
	float score;
	uint32_t testCount;
	clock_t start,stop,diff;
	uint32_t * leftCheck;
	uint32_t * rightCheck;
	//add end

    struct Group {
        GLuint texture;
        GLuint fbo;
        GLuint width;
        GLuint height;
        mat4 colorTransform;
    };

    Description mState;
    Vector<Group> mGroupStack;

    virtual void bindImageAsFramebuffer(EGLImageKHR image,
            uint32_t* texName, uint32_t* fbName, uint32_t* status);
    virtual void unbindFramebuffer(uint32_t texName, uint32_t fbName);

public:
    GLES20RenderEngine();

protected:
    virtual ~GLES20RenderEngine();

    virtual void dump(String8& result);
    virtual void setViewportAndProjection(size_t vpw, size_t vph,
            Rect sourceCrop, size_t hwh, bool yswap, Transform::orientation_flags rotation);
    virtual void setupLayerBlending(bool premultipliedAlpha, bool opaque, int alpha);
    virtual void setupDimLayerBlending(int alpha);
    virtual void setupLayerTexturing(const Texture& texture);
    virtual void setupLayerBlackedOut();
    virtual void setupFillWithColor(float r, float g, float b, float a);
    virtual void disableTexturing();
    virtual void disableBlending();

    virtual void drawMesh(const Mesh& mesh);

#ifdef ENABLE_VR
    virtual void initVRInfoTable();
    virtual void drawFBLeft();
    virtual void drawFBRight();
    virtual void drawLeftFBO(const Mesh& mesh);
    virtual void drawRightFBO(const Mesh& mesh);
    virtual void loadTexCoordsFB();
    virtual void loadVerCoordsFB(int mode);
    virtual GLuint genVRMeshBuffer(float halfWidth,float halfHeight);
    virtual vec2 genDeformTex(vec2 tex,float k1,float k2);
    virtual void enableRightFBO(bool key);
    virtual bool checkVRPropertyChanged();
	virtual void clearFbo();
	virtual void setTargetDpyXY(int x, int y, int dpyId);
	virtual void createFBO(int dpyId);
	virtual void updateFBOSize(int dpyId);
	virtual bool queryCaptureScreen();
    virtual void beginGroup(const mat4& colorTransform,int mode);
    virtual void endGroup(int mode);
	virtual void isVideo3dFormat(int mode);
	virtual void _test_CreateCheckFBO();
	virtual float _test_Similarity();
	virtual float _test_isSimilaryImages(const uint32_t* frame1,const uint32_t * frame2);
	virtual void _test_drawMeshLeftCheckFBO(const Mesh& mesh, int fboId, int mode);
	virtual void _test_drawMeshRightCheckFBO(const Mesh& mesh, int fboId, int mode);
#else
    virtual void beginGroup(const mat4& colorTransform);
    virtual void endGroup();
#endif

    virtual size_t getMaxTextureSize() const;
    virtual size_t getMaxViewportDims() const;
};

// ---------------------------------------------------------------------------
}; // namespace android
// ---------------------------------------------------------------------------

#endif /* SF_GLES20RENDERENGINE_H_ */
