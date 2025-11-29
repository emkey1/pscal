#if defined(__APPLE__)
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif
#endif

#include "runtime/shaders/shader_common.h"

#ifdef SDL

#include <stdio.h>
#include <string.h>

static void writeInfoLog(GLuint object, bool isProgram, char *infoLog, size_t infoLogSize) {
    if (!infoLog || infoLogSize == 0) {
        return;
    }
    GLint logLength = 0;
    if (isProgram) {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0) {
            GLsizei written = 0;
            glGetProgramInfoLog(object, (GLsizei)infoLogSize, &written, infoLog);
            if (written < (GLsizei)infoLogSize) {
                infoLog[written] = '\0';
            } else {
                infoLog[infoLogSize - 1] = '\0';
            }
        } else {
            infoLog[0] = '\0';
        }
    } else {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0) {
            GLsizei written = 0;
            glGetShaderInfoLog(object, (GLsizei)infoLogSize, &written, infoLog);
            if (written < (GLsizei)infoLogSize) {
                infoLog[written] = '\0';
            } else {
                infoLog[infoLogSize - 1] = '\0';
            }
        } else {
            infoLog[0] = '\0';
        }
    }
}

GLuint runtimeCompileShader(GLenum type, const char *source, char *infoLog, size_t infoLogSize) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        if (infoLog && infoLogSize > 0) {
            snprintf(infoLog, infoLogSize, "Failed to create shader object.");
        }
        return 0;
    }

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        writeInfoLog(shader, false, infoLog, infoLogSize);
        glDeleteShader(shader);
        return 0;
    }

    if (infoLog && infoLogSize > 0) {
        infoLog[0] = '\0';
    }
    return shader;
}

GLuint runtimeCreateProgram(const char *vertexSource,
                            const char *fragmentSource,
                            char *infoLog,
                            size_t infoLogSize) {
    GLuint vertexShader = runtimeCompileShader(GL_VERTEX_SHADER, vertexSource, infoLog, infoLogSize);
    if (!vertexShader) {
        return 0;
    }

    GLuint fragmentShader = runtimeCompileShader(GL_FRAGMENT_SHADER, fragmentSource, infoLog, infoLogSize);
    if (!fragmentShader) {
        glDeleteShader(vertexShader);
        return 0;
    }

    GLuint program = glCreateProgram();
    if (!program) {
        if (infoLog && infoLogSize > 0) {
            snprintf(infoLog, infoLogSize, "Failed to create shader program.");
        }
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return 0;
    }

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (infoLog && infoLogSize > 0) {
        infoLog[0] = '\0';
    }

    return program;
}

bool runtimeLinkProgram(GLuint program, char *infoLog, size_t infoLogSize) {
    if (!program) {
        if (infoLog && infoLogSize > 0) {
            snprintf(infoLog, infoLogSize, "Invalid program object.");
        }
        return false;
    }

    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        writeInfoLog(program, true, infoLog, infoLogSize);
        return false;
    }

    if (infoLog && infoLogSize > 0) {
        infoLog[0] = '\0';
    }
    return true;
}

void runtimeDestroyProgram(GLuint program) {
    if (program) {
        glDeleteProgram(program);
    }
}

#endif // SDL
