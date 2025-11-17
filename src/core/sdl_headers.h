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

#if defined(PSCALI_SDL3)
typedef struct SDL_version {
    Uint8 major;
    Uint8 minor;
    Uint8 patch;
} SDL_version;

#if defined(PSCALI_SDL_IMAGE_FALLBACK_TO_SDL2)
#ifdef SDL_RWFromMem
#pragma push_macro("SDL_RWFromMem")
#undef SDL_RWFromMem
#define PSCALI_RESTORE_SDL_RWFromMem 1
#endif
#ifdef SDL_RWFromConstMem
#pragma push_macro("SDL_RWFromConstMem")
#undef SDL_RWFromConstMem
#define PSCALI_RESTORE_SDL_RWFromConstMem 1
#endif
#ifdef SDL_RWFromFile
#pragma push_macro("SDL_RWFromFile")
#undef SDL_RWFromFile
#define PSCALI_RESTORE_SDL_RWFromFile 1
#endif
#ifdef SDL_RWclose
#pragma push_macro("SDL_RWclose")
#undef SDL_RWclose
#define PSCALI_RESTORE_SDL_RWclose 1
#endif
#ifdef SDL_RWops
#pragma push_macro("SDL_RWops")
#undef SDL_RWops
#define PSCALI_RESTORE_SDL_RWops 1
#endif
#ifdef SDL_RWread
#pragma push_macro("SDL_RWread")
#undef SDL_RWread
#define PSCALI_RESTORE_SDL_RWread 1
#endif
#ifdef SDL_RWseek
#pragma push_macro("SDL_RWseek")
#undef SDL_RWseek
#define PSCALI_RESTORE_SDL_RWseek 1
#endif
#ifdef SDL_RWsize
#pragma push_macro("SDL_RWsize")
#undef SDL_RWsize
#define PSCALI_RESTORE_SDL_RWsize 1
#endif
#ifdef SDL_RWtell
#pragma push_macro("SDL_RWtell")
#undef SDL_RWtell
#define PSCALI_RESTORE_SDL_RWtell 1
#endif
#ifdef SDL_RWwrite
#pragma push_macro("SDL_RWwrite")
#undef SDL_RWwrite
#define PSCALI_RESTORE_SDL_RWwrite 1
#endif
#ifdef RW_SEEK_SET
#pragma push_macro("RW_SEEK_SET")
#undef RW_SEEK_SET
#define PSCALI_RESTORE_RW_SEEK_SET 1
#endif
#ifdef RW_SEEK_CUR
#pragma push_macro("RW_SEEK_CUR")
#undef RW_SEEK_CUR
#define PSCALI_RESTORE_RW_SEEK_CUR 1
#endif
#ifdef RW_SEEK_END
#pragma push_macro("RW_SEEK_END")
#undef RW_SEEK_END
#define PSCALI_RESTORE_RW_SEEK_END 1
#endif

#ifndef PSCALI_DEFINED_SDL_RWOPS_TYPE
typedef SDL_IOStream SDL_RWops;
#define PSCALI_DEFINED_SDL_RWOPS_TYPE 1
#endif

#include <SDL2/SDL_image.h>
#ifdef PSCALI_RESTORE_SDL_RWFromMem
#pragma pop_macro("SDL_RWFromMem")
#undef PSCALI_RESTORE_SDL_RWFromMem
#endif
#ifdef PSCALI_RESTORE_SDL_RWFromConstMem
#pragma pop_macro("SDL_RWFromConstMem")
#undef PSCALI_RESTORE_SDL_RWFromConstMem
#endif
#ifdef PSCALI_RESTORE_SDL_RWFromFile
#pragma pop_macro("SDL_RWFromFile")
#undef PSCALI_RESTORE_SDL_RWFromFile
#endif
#ifdef PSCALI_RESTORE_SDL_RWclose
#pragma pop_macro("SDL_RWclose")
#undef PSCALI_RESTORE_SDL_RWclose
#endif
#ifdef PSCALI_RESTORE_SDL_RWops
#pragma pop_macro("SDL_RWops")
#undef PSCALI_RESTORE_SDL_RWops
#endif
#ifdef PSCALI_RESTORE_SDL_RWread
#pragma pop_macro("SDL_RWread")
#undef PSCALI_RESTORE_SDL_RWread
#endif
#ifdef PSCALI_RESTORE_SDL_RWseek
#pragma pop_macro("SDL_RWseek")
#undef PSCALI_RESTORE_SDL_RWseek
#endif
#ifdef PSCALI_RESTORE_SDL_RWsize
#pragma pop_macro("SDL_RWsize")
#undef PSCALI_RESTORE_SDL_RWsize
#endif
#ifdef PSCALI_RESTORE_SDL_RWtell
#pragma pop_macro("SDL_RWtell")
#undef PSCALI_RESTORE_SDL_RWtell
#endif
#ifdef PSCALI_RESTORE_SDL_RWwrite
#pragma pop_macro("SDL_RWwrite")
#undef PSCALI_RESTORE_SDL_RWwrite
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_SET
#pragma pop_macro("RW_SEEK_SET")
#undef PSCALI_RESTORE_RW_SEEK_SET
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_CUR
#pragma pop_macro("RW_SEEK_CUR")
#undef PSCALI_RESTORE_RW_SEEK_CUR
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_END
#pragma pop_macro("RW_SEEK_END")
#undef PSCALI_RESTORE_RW_SEEK_END
#endif
#endif

#if defined(PSCALI_SDL_MIXER_FALLBACK_TO_SDL2)
#ifdef SDL_RWFromMem
#pragma push_macro("SDL_RWFromMem")
#undef SDL_RWFromMem
#define PSCALI_RESTORE_SDL_RWFromMem 1
#endif
#ifdef SDL_RWFromConstMem
#pragma push_macro("SDL_RWFromConstMem")
#undef SDL_RWFromConstMem
#define PSCALI_RESTORE_SDL_RWFromConstMem 1
#endif
#ifdef SDL_RWFromFile
#pragma push_macro("SDL_RWFromFile")
#undef SDL_RWFromFile
#define PSCALI_RESTORE_SDL_RWFromFile 1
#endif
#ifdef SDL_RWclose
#pragma push_macro("SDL_RWclose")
#undef SDL_RWclose
#define PSCALI_RESTORE_SDL_RWclose 1
#endif
#ifdef SDL_RWops
#pragma push_macro("SDL_RWops")
#undef SDL_RWops
#define PSCALI_RESTORE_SDL_RWops 1
#endif
#ifdef SDL_RWread
#pragma push_macro("SDL_RWread")
#undef SDL_RWread
#define PSCALI_RESTORE_SDL_RWread 1
#endif
#ifdef SDL_RWseek
#pragma push_macro("SDL_RWseek")
#undef SDL_RWseek
#define PSCALI_RESTORE_SDL_RWseek 1
#endif
#ifdef SDL_RWsize
#pragma push_macro("SDL_RWsize")
#undef SDL_RWsize
#define PSCALI_RESTORE_SDL_RWsize 1
#endif
#ifdef SDL_RWtell
#pragma push_macro("SDL_RWtell")
#undef SDL_RWtell
#define PSCALI_RESTORE_SDL_RWtell 1
#endif
#ifdef SDL_RWwrite
#pragma push_macro("SDL_RWwrite")
#undef SDL_RWwrite
#define PSCALI_RESTORE_SDL_RWwrite 1
#endif
#ifdef RW_SEEK_SET
#pragma push_macro("RW_SEEK_SET")
#undef RW_SEEK_SET
#define PSCALI_RESTORE_RW_SEEK_SET 1
#endif
#ifdef RW_SEEK_CUR
#pragma push_macro("RW_SEEK_CUR")
#undef RW_SEEK_CUR
#define PSCALI_RESTORE_RW_SEEK_CUR 1
#endif
#ifdef RW_SEEK_END
#pragma push_macro("RW_SEEK_END")
#undef RW_SEEK_END
#define PSCALI_RESTORE_RW_SEEK_END 1
#endif

#ifndef PSCALI_DEFINED_SDL_RWOPS_TYPE
typedef SDL_IOStream SDL_RWops;
#define PSCALI_DEFINED_SDL_RWOPS_TYPE 1
#endif

#include <SDL2/SDL_mixer.h>
#ifdef PSCALI_RESTORE_SDL_RWFromMem
#pragma pop_macro("SDL_RWFromMem")
#undef PSCALI_RESTORE_SDL_RWFromMem
#endif
#ifdef PSCALI_RESTORE_SDL_RWFromConstMem
#pragma pop_macro("SDL_RWFromConstMem")
#undef PSCALI_RESTORE_SDL_RWFromConstMem
#endif
#ifdef PSCALI_RESTORE_SDL_RWFromFile
#pragma pop_macro("SDL_RWFromFile")
#undef PSCALI_RESTORE_SDL_RWFromFile
#endif
#ifdef PSCALI_RESTORE_SDL_RWclose
#pragma pop_macro("SDL_RWclose")
#undef PSCALI_RESTORE_SDL_RWclose
#endif
#ifdef PSCALI_RESTORE_SDL_RWops
#pragma pop_macro("SDL_RWops")
#undef PSCALI_RESTORE_SDL_RWops
#endif
#ifdef PSCALI_RESTORE_SDL_RWread
#pragma pop_macro("SDL_RWread")
#undef PSCALI_RESTORE_SDL_RWread
#endif
#ifdef PSCALI_RESTORE_SDL_RWseek
#pragma pop_macro("SDL_RWseek")
#undef PSCALI_RESTORE_SDL_RWseek
#endif
#ifdef PSCALI_RESTORE_SDL_RWsize
#pragma pop_macro("SDL_RWsize")
#undef PSCALI_RESTORE_SDL_RWsize
#endif
#ifdef PSCALI_RESTORE_SDL_RWtell
#pragma pop_macro("SDL_RWtell")
#undef PSCALI_RESTORE_SDL_RWtell
#endif
#ifdef PSCALI_RESTORE_SDL_RWwrite
#pragma pop_macro("SDL_RWwrite")
#undef PSCALI_RESTORE_SDL_RWwrite
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_SET
#pragma pop_macro("RW_SEEK_SET")
#undef PSCALI_RESTORE_RW_SEEK_SET
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_CUR
#pragma pop_macro("RW_SEEK_CUR")
#undef PSCALI_RESTORE_RW_SEEK_CUR
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_END
#pragma pop_macro("RW_SEEK_END")
#undef PSCALI_RESTORE_RW_SEEK_END
#endif
#endif

#if defined(PSCALI_SDL_TTF_FALLBACK_TO_SDL2)
#ifdef SDL_RWFromMem
#pragma push_macro("SDL_RWFromMem")
#undef SDL_RWFromMem
#define PSCALI_RESTORE_SDL_RWFromMem 1
#endif
#ifdef SDL_RWFromConstMem
#pragma push_macro("SDL_RWFromConstMem")
#undef SDL_RWFromConstMem
#define PSCALI_RESTORE_SDL_RWFromConstMem 1
#endif
#ifdef SDL_RWFromFile
#pragma push_macro("SDL_RWFromFile")
#undef SDL_RWFromFile
#define PSCALI_RESTORE_SDL_RWFromFile 1
#endif
#ifdef SDL_RWclose
#pragma push_macro("SDL_RWclose")
#undef SDL_RWclose
#define PSCALI_RESTORE_SDL_RWclose 1
#endif
#ifdef SDL_RWops
#pragma push_macro("SDL_RWops")
#undef SDL_RWops
#define PSCALI_RESTORE_SDL_RWops 1
#endif
#ifdef SDL_RWread
#pragma push_macro("SDL_RWread")
#undef SDL_RWread
#define PSCALI_RESTORE_SDL_RWread 1
#endif
#ifdef SDL_RWseek
#pragma push_macro("SDL_RWseek")
#undef SDL_RWseek
#define PSCALI_RESTORE_SDL_RWseek 1
#endif
#ifdef SDL_RWsize
#pragma push_macro("SDL_RWsize")
#undef SDL_RWsize
#define PSCALI_RESTORE_SDL_RWsize 1
#endif
#ifdef SDL_RWtell
#pragma push_macro("SDL_RWtell")
#undef SDL_RWtell
#define PSCALI_RESTORE_SDL_RWtell 1
#endif
#ifdef SDL_RWwrite
#pragma push_macro("SDL_RWwrite")
#undef SDL_RWwrite
#define PSCALI_RESTORE_SDL_RWwrite 1
#endif
#ifdef RW_SEEK_SET
#pragma push_macro("RW_SEEK_SET")
#undef RW_SEEK_SET
#define PSCALI_RESTORE_RW_SEEK_SET 1
#endif
#ifdef RW_SEEK_CUR
#pragma push_macro("RW_SEEK_CUR")
#undef RW_SEEK_CUR
#define PSCALI_RESTORE_RW_SEEK_CUR 1
#endif
#ifdef RW_SEEK_END
#pragma push_macro("RW_SEEK_END")
#undef RW_SEEK_END
#define PSCALI_RESTORE_RW_SEEK_END 1
#endif

#ifndef PSCALI_DEFINED_SDL_RWOPS_TYPE
typedef SDL_IOStream SDL_RWops;
#define PSCALI_DEFINED_SDL_RWOPS_TYPE 1
#endif

#include <SDL2/SDL_ttf.h>
#ifdef PSCALI_RESTORE_SDL_RWFromMem
#pragma pop_macro("SDL_RWFromMem")
#undef PSCALI_RESTORE_SDL_RWFromMem
#endif
#ifdef PSCALI_RESTORE_SDL_RWFromConstMem
#pragma pop_macro("SDL_RWFromConstMem")
#undef PSCALI_RESTORE_SDL_RWFromConstMem
#endif
#ifdef PSCALI_RESTORE_SDL_RWFromFile
#pragma pop_macro("SDL_RWFromFile")
#undef PSCALI_RESTORE_SDL_RWFromFile
#endif
#ifdef PSCALI_RESTORE_SDL_RWclose
#pragma pop_macro("SDL_RWclose")
#undef PSCALI_RESTORE_SDL_RWclose
#endif
#ifdef PSCALI_RESTORE_SDL_RWops
#pragma pop_macro("SDL_RWops")
#undef PSCALI_RESTORE_SDL_RWops
#endif
#ifdef PSCALI_RESTORE_SDL_RWread
#pragma pop_macro("SDL_RWread")
#undef PSCALI_RESTORE_SDL_RWread
#endif
#ifdef PSCALI_RESTORE_SDL_RWseek
#pragma pop_macro("SDL_RWseek")
#undef PSCALI_RESTORE_SDL_RWseek
#endif
#ifdef PSCALI_RESTORE_SDL_RWsize
#pragma pop_macro("SDL_RWsize")
#undef PSCALI_RESTORE_SDL_RWsize
#endif
#ifdef PSCALI_RESTORE_SDL_RWtell
#pragma pop_macro("SDL_RWtell")
#undef PSCALI_RESTORE_SDL_RWtell
#endif
#ifdef PSCALI_RESTORE_SDL_RWwrite
#pragma pop_macro("SDL_RWwrite")
#undef PSCALI_RESTORE_SDL_RWwrite
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_SET
#pragma pop_macro("RW_SEEK_SET")
#undef PSCALI_RESTORE_RW_SEEK_SET
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_CUR
#pragma pop_macro("RW_SEEK_CUR")
#undef PSCALI_RESTORE_RW_SEEK_CUR
#endif
#ifdef PSCALI_RESTORE_RW_SEEK_END
#pragma pop_macro("RW_SEEK_END")
#undef PSCALI_RESTORE_RW_SEEK_END
#endif
#endif

#endif /* PSCALI_SDL3 */
