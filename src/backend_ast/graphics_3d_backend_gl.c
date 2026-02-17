#include "backend_ast/graphics_3d_backend.h"

#if !defined(PSCAL_TARGET_IOS) && !defined(__APPLE__)

#include "core/sdl_headers.h"
#include PSCALI_SDL_OPENGL_HEADER

void gfx3dClearColor(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
}

void gfx3dClear(unsigned int mask) {
    glClear(mask);
}

void gfx3dClearDepth(double depth) {
#if defined(GL_ES_VERSION_2_0)
    glClearDepthf((GLfloat)depth);
#else
    glClearDepth(depth);
#endif
}

void gfx3dViewport(int x, int y, int width, int height) {
    glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
}

void gfx3dMatrixMode(int mode) {
    glMatrixMode((GLenum)mode);
}

void gfx3dLoadIdentity(void) {
    glLoadIdentity();
}

void gfx3dTranslatef(float x, float y, float z) {
    glTranslatef(x, y, z);
}

void gfx3dRotatef(float angle, float x, float y, float z) {
    glRotatef(angle, x, y, z);
}

void gfx3dScalef(float x, float y, float z) {
    glScalef(x, y, z);
}

void gfx3dFrustum(double left, double right, double bottom, double top,
                  double zNear, double zFar) {
#if defined(GL_ES_VERSION_2_0)
    glFrustumf((GLfloat)left, (GLfloat)right, (GLfloat)bottom, (GLfloat)top,
               (GLfloat)zNear, (GLfloat)zFar);
#else
    glFrustum(left, right, bottom, top, zNear, zFar);
#endif
}

void gfx3dPushMatrix(void) {
    glPushMatrix();
}

void gfx3dPopMatrix(void) {
    glPopMatrix();
}

void gfx3dBegin(unsigned int primitive) {
    glBegin((GLenum)primitive);
}

void gfx3dEnd(void) {
    glEnd();
}

void gfx3dColor3f(float r, float g, float b) {
    glColor3f(r, g, b);
}

void gfx3dColor4f(float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
}

void gfx3dVertex3f(float x, float y, float z) {
    glVertex3f(x, y, z);
}

void gfx3dNormal3f(float x, float y, float z) {
    glNormal3f(x, y, z);
}

void gfx3dEnable(unsigned int capability) {
    glEnable((GLenum)capability);
}

void gfx3dDisable(unsigned int capability) {
    glDisable((GLenum)capability);
}

void gfx3dShadeModel(unsigned int mode) {
    glShadeModel((GLenum)mode);
}

void gfx3dLightfv(unsigned int light, unsigned int pname, const float* params) {
    glLightfv((GLenum)light, (GLenum)pname, params);
}

void gfx3dMaterialfv(unsigned int face, unsigned int pname, const float* params) {
    glMaterialfv((GLenum)face, (GLenum)pname, params);
}

void gfx3dMaterialf(unsigned int face, unsigned int pname, float value) {
    glMaterialf((GLenum)face, (GLenum)pname, value);
}

void gfx3dColorMaterial(unsigned int face, unsigned int mode) {
    glColorMaterial((GLenum)face, (GLenum)mode);
}

void gfx3dBlendFunc(unsigned int src, unsigned int dst) {
    glBlendFunc((GLenum)src, (GLenum)dst);
}

void gfx3dCullFace(unsigned int mode) {
    glCullFace((GLenum)mode);
}

void gfx3dDepthMask(bool enable) {
    glDepthMask(enable ? GL_TRUE : GL_FALSE);
}

void gfx3dDepthFunc(unsigned int func) {
    glDepthFunc((GLenum)func);
}

void gfx3dLineWidth(float width) {
    glLineWidth(width);
}

unsigned int gfx3dGenLists(int range) {
    return (unsigned int)glGenLists(range);
}

void gfx3dDeleteLists(unsigned int list, int range) {
    glDeleteLists((GLuint)list, range);
}

void gfx3dNewList(unsigned int list, unsigned int mode) {
    glNewList((GLuint)list, (GLenum)mode);
}

void gfx3dEndList(void) {
    glEndList();
}

void gfx3dCallList(unsigned int list) {
    glCallList((GLuint)list);
}

void gfx3dPixelStorei(unsigned int pname, int param) {
    glPixelStorei((GLenum)pname, param);
}

void gfx3dReadBuffer(unsigned int mode) {
    glReadBuffer((GLenum)mode);
}

void gfx3dReadPixels(int x, int y, int width, int height,
                     unsigned int format, unsigned int type, void* pixels) {
    glReadPixels(x, y, width, height, (GLenum)format, (GLenum)type, pixels);
}

unsigned int gfx3dGetError(void) {
    return (unsigned int)glGetError();
}

void gfx3dPresent(void) {
}

void gfx3dReleaseResources(void) {
}

#endif // !defined(PSCAL_TARGET_IOS) && !defined(__APPLE__)
