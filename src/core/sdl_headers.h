#pragma once

#if defined(PSCALI_SDL3) && !defined(SDL_ENABLE_OLD_NAMES)
#define SDL_ENABLE_OLD_NAMES 1
#endif

#if defined(PSCALI_SDL3)
#define PSCAL_SDL_INIT_SUBSYSTEM(flags) SDL_Init(flags)
#define PSCAL_SDL_QUIT_SUBSYSTEM(flags) ((void)0)
#define PSCAL_SDL_WAS_INIT(flags) (SDL_WasInit(flags) ? (flags) : 0)
#else
#define PSCAL_SDL_INIT_SUBSYSTEM(flags) SDL_InitSubSystem(flags)
#define PSCAL_SDL_QUIT_SUBSYSTEM(flags) SDL_QuitSubSystem(flags)
#define PSCAL_SDL_WAS_INIT(flags) SDL_WasInit(flags)
#endif

#if defined(__has_include)
#if __has_include(<SDL3/SDL_ttf.h>)
#define PSCALI_SDL_TTF_HEADER <SDL3/SDL_ttf.h>
#elif __has_include(<SDL2/SDL_ttf.h>)
#define PSCALI_SDL_TTF_HEADER <SDL2/SDL_ttf.h>
#endif
#if __has_include(<SDL3/SDL_image.h>)
#define PSCALI_SDL_IMAGE_HEADER <SDL3/SDL_image.h>
#elif __has_include(<SDL2/SDL_image.h>)
#define PSCALI_SDL_IMAGE_HEADER <SDL2/SDL_image.h>
#endif
#if __has_include(<SDL3/SDL_mixer.h>)
#define PSCALI_SDL_MIXER_HEADER <SDL3/SDL_mixer.h>
#elif __has_include(<SDL2/SDL_mixer.h>)
#define PSCALI_SDL_MIXER_HEADER <SDL2/SDL_mixer.h>
#endif
#if __has_include(<SDL3/SDL_syswm.h>)
#define PSCALI_SDL_SYSWM_HEADER <SDL3/SDL_syswm.h>
#elif !defined(PSCALI_SDL3) && __has_include(<SDL2/SDL_syswm.h>)
#define PSCALI_SDL_SYSWM_HEADER <SDL2/SDL_syswm.h>
#endif
#if __has_include(<SDL3/SDL_opengl.h>)
#define PSCALI_SDL_OPENGL_HEADER <SDL3/SDL_opengl.h>
#elif __has_include(<SDL2/SDL_opengl.h>)
#define PSCALI_SDL_OPENGL_HEADER <SDL2/SDL_opengl.h>
#endif
#if __has_include(<SDL3/SDL_opengl_glext.h>)
#define PSCALI_SDL_OPENGL_GLEXT_HEADER <SDL3/SDL_opengl_glext.h>
#elif __has_include(<SDL2/SDL_opengl_glext.h>)
#define PSCALI_SDL_OPENGL_GLEXT_HEADER <SDL2/SDL_opengl_glext.h>
#endif
#endif

#ifndef PSCALI_SDL_TTF_HEADER
#define PSCALI_SDL_TTF_HEADER <SDL2/SDL_ttf.h>
#endif
#ifndef PSCALI_SDL_IMAGE_HEADER
#define PSCALI_SDL_IMAGE_HEADER <SDL2/SDL_image.h>
#endif
#ifndef PSCALI_SDL_MIXER_HEADER
#define PSCALI_SDL_MIXER_HEADER <SDL2/SDL_mixer.h>
#endif
#if !defined(PSCALI_SDL_SYSWM_HEADER) && !defined(PSCALI_SDL3)
#define PSCALI_SDL_SYSWM_HEADER <SDL2/SDL_syswm.h>
#endif
#ifndef PSCALI_SDL_OPENGL_HEADER
#define PSCALI_SDL_OPENGL_HEADER <SDL2/SDL_opengl.h>
#endif
#ifndef PSCALI_SDL_OPENGL_GLEXT_HEADER
#define PSCALI_SDL_OPENGL_GLEXT_HEADER <SDL2/SDL_opengl_glext.h>
#endif

#if defined(PSCALI_SDL_SYSWM_HEADER)
#define PSCALI_HAS_SYSWM 1
#else
#define PSCALI_HAS_SYSWM 0
#endif

#if defined(SDL_VERSION_ATLEAST)
#if defined(PSCALI_SDL3) || SDL_VERSION_ATLEAST(2,0,18)
#define PSCAL_SDL_GET_TICKS() SDL_GetTicks64()
#else
#define PSCAL_SDL_GET_TICKS() ((Uint64)SDL_GetTicks())
#endif
#else
#define PSCAL_SDL_GET_TICKS() ((Uint64)SDL_GetTicks())
#endif

#if defined(PSCALI_SDL3)
#include <SDL3/SDL.h>
#else
#include <SDL2/SDL.h>
#endif
