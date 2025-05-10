//
//  audio.h
//  Pscal
//
//  Created by Michael Miller on 5/10/25.
//
#ifndef PSCAL_AUDIO_H
#define PSCAL_AUDIO_H

#include <SDL2/SDL.h> // Include the SDL_mixer header
#include <SDL2/SDL_mixer.h> // Include the SDL_mixer header
#include "types.h"          // For the Value struct if needed (though helpers won't return Value)
#include "ast.h"            // For AST node if needed

#define MAX_SOUNDS 32 // Define a maximum number of sound effects we can load

// Global array to store pointers to loaded sound chunks (sound effects)
extern Mix_Chunk* gLoadedSounds[MAX_SOUNDS];
// Flag to track if the sound system (SDL Audio + Mix_Init/OpenAudio) is initialized
extern bool gSoundSystemInitialized;

// Helper functions for C-side audio management
void InitializeSoundArray(void); // Internal helper to initialize the sound array pointers to NULL

// Initialize the SDL audio subsystem and SDL_mixer
void Audio_InitSystem(void);

// Load a sound file (like a .wav). Returns an integer ID (1-based index) or -1 on error.
int Audio_LoadSound(const char* filename);

// Play a loaded sound effect once. Takes the 1-based sound ID.
void Audio_PlaySound(int soundID);

// Free a loaded sound effect from memory. Takes the 1-based sound ID.
void Audio_FreeSound(int soundID);

// Shut down SDL_mixer and the SDL audio subsystem
void Audio_QuitSystem(void);

// The builtins
Value executeBuiltinInitSoundSystem(AST *node);
Value executeBuiltinLoadSound(AST *node);
Value executeBuiltinPlaySound(AST *node);
Value executeBuiltinQuitSoundSystem(AST *node);



#endif // PSCAL_AUDIO_H
