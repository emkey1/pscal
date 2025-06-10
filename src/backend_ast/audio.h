//
//  audio.h
//  Pscal
//
//  Created by Michael Miller on 5/10/25.
//
#ifndef PSCAL_AUDIO_H
#define PSCAL_AUDIO_H

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
Value executeBuiltinInitSoundSystem(AST *node);
Value executeBuiltinLoadSound(AST *node);
Value executeBuiltinPlaySound(AST *node);
Value executeBuiltinQuitSoundSystem(AST *node);
Value executeBuiltinIsSoundPlaying(AST *node);

// VM-native built-ins
Value vm_builtin_initsoundsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_loadsound(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_playsound(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_quitsoundsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_initsoundsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_loadsound(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_playsound(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_quitsoundsystem(struct VM_s* vm, int arg_count, Value* args);

#endif // PSCAL_AUDIO_H
