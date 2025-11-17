#ifndef PSCAL_RUNTIME_SHADERS_SHADER_COMMON_H
#define PSCAL_RUNTIME_SHADERS_SHADER_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef SDL
#include "core/sdl_headers.h"
#if defined(PSCAL_TARGET_IOS)
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#else
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include PSCALI_SDL_OPENGL_HEADER
#if defined(__has_include)
#if __has_include(PSCALI_SDL_OPENGL_GLEXT_HEADER)
#include PSCALI_SDL_OPENGL_GLEXT_HEADER
#endif
#endif
#endif

GLuint runtimeCompileShader(GLenum type, const char *source, char *infoLog, size_t infoLogSize);
GLuint runtimeCreateProgram(const char *vertexSource,
                            const char *fragmentSource,
                            char *infoLog,
                            size_t infoLogSize);
bool runtimeLinkProgram(GLuint program, char *infoLog, size_t infoLogSize);
void runtimeDestroyProgram(GLuint program);
#endif

#endif // PSCAL_RUNTIME_SHADERS_SHADER_COMMON_H
