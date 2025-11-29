#ifndef PSCAL_GRAPHICS_3D_BACKEND_H
#define PSCAL_GRAPHICS_3D_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gfx3dClearColor(float r, float g, float b, float a);
void gfx3dClear(unsigned int mask);
void gfx3dClearDepth(double depth);
void gfx3dViewport(int x, int y, int width, int height);
void gfx3dMatrixMode(int mode);
void gfx3dLoadIdentity(void);
void gfx3dTranslatef(float x, float y, float z);
void gfx3dRotatef(float angle, float x, float y, float z);
void gfx3dScalef(float x, float y, float z);
void gfx3dFrustum(double left, double right, double bottom, double top,
                  double zNear, double zFar);
void gfx3dPushMatrix(void);
void gfx3dPopMatrix(void);
void gfx3dBegin(unsigned int primitive);
void gfx3dEnd(void);
void gfx3dColor3f(float r, float g, float b);
void gfx3dColor4f(float r, float g, float b, float a);
void gfx3dVertex3f(float x, float y, float z);
void gfx3dNormal3f(float x, float y, float z);
void gfx3dEnable(unsigned int capability);
void gfx3dDisable(unsigned int capability);
void gfx3dShadeModel(unsigned int mode);
void gfx3dLightfv(unsigned int light, unsigned int pname, const float* params);
void gfx3dMaterialfv(unsigned int face, unsigned int pname, const float* params);
void gfx3dMaterialf(unsigned int face, unsigned int pname, float value);
void gfx3dColorMaterial(unsigned int face, unsigned int mode);
void gfx3dBlendFunc(unsigned int src, unsigned int dst);
void gfx3dCullFace(unsigned int mode);
void gfx3dDepthMask(bool enable);
void gfx3dDepthFunc(unsigned int func);
void gfx3dLineWidth(float width);
unsigned int gfx3dGenLists(int range);
void gfx3dDeleteLists(unsigned int list, int range);
void gfx3dNewList(unsigned int list, unsigned int mode);
void gfx3dEndList(void);
void gfx3dCallList(unsigned int list);
void gfx3dPixelStorei(unsigned int pname, int param);
void gfx3dReadBuffer(unsigned int mode);
void gfx3dReadPixels(int x, int y, int width, int height,
                     unsigned int format, unsigned int type, void* pixels);
unsigned int gfx3dGetError(void);

#ifdef __cplusplus
}
#endif

#endif // PSCAL_GRAPHICS_3D_BACKEND_H
