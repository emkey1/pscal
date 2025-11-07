#ifndef PSCAL_RUNTIME_SHADERS_SHADER_COMMON_H
#define PSCAL_RUNTIME_SHADERS_SHADER_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef SDL
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <SDL2/SDL_opengl.h>
#if defined(__has_include)
#if __has_include(<SDL2/SDL_opengl_glext.h>)
#include <SDL2/SDL_opengl_glext.h>
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
