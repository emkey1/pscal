//
//  audio.h
//  Pscal
//
//  Created by Michael Miller on 5/10/25.
//
#ifndef PSCAL_AUDIO_H
#define PSCAL_AUDIO_H

#ifdef SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include "types.h"
#include "ast.h"

// Forward declare the VM struct
struct VM_s;

#define MAX_SOUNDS 32

extern Mix_Chunk* gLoadedSounds[MAX_SOUNDS];
extern bool gSoundSystemInitialized;

void initializeSoundArray(void);
void audioInitSystem(void);
int audioLoadSound(const char* filename);
void audioPlaySound(int soundID);
void audioFreeSound(int soundID);
void audioQuitSystem(void);

// AST-based built-ins

// VM-native built-ins
Value vmBuiltinInitsoundsystem(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLoadsound(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPlaysound(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFreesound(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinQuitsoundsystem(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinIssoundplaying(struct VM_s* vm, int arg_count, Value* args);

#endif
#endif // PSCAL_AUDIO_H
