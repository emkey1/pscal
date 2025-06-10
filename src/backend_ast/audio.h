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

// --- START MODIFICATION ---
// Forward declare the VM struct to break circular dependencies
struct VM_s;
// --- END MODIFICATION ---

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

// --- START MODIFICATION ---
// Prototypes for VM-native built-in handlers
// Use the explicit struct tag 'struct VM_s*' to match the forward declaration.
Value vm_builtin_initsoundsystem(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_loadsound(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_playsound(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_quitsoundsystem(struct VM_s* vm, int arg_count, Value* args);
// --- END MODIFICATION ---

#endif // PSCAL_AUDIO_H
