// src/backend_ast/audio.c

#include "audio.h"
#include "Pascal/globals.h" // For EXIT_FAILURE_HANDLER
#include "core/utils.h" // For EXIT_FAILURE_HANDLER
#include "pscal_paths.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For strdup
#include "vm/vm.h" // <<< ADDED: For VM struct and runtimeError prototype


#ifdef SDL
// Define and initialize global variables from audio.h
Mix_Chunk* gLoadedSounds[MAX_SOUNDS];
bool gSoundSystemInitialized = false;

static bool audioPathHasDirectorySeparator(const char* path) {
    if (!path) {
        return false;
    }
    return strchr(path, '/') != NULL || strchr(path, '\\') != NULL;
}

static Mix_Chunk* audioTryLoadChunk(const char* candidate, char* resolved, size_t resolved_size) {
    if (!candidate || candidate[0] == '\0') {
        return NULL;
    }
    Mix_Chunk* chunk = Mix_LoadWAV(candidate);
    if (!chunk) {
        return NULL;
    }
    if (resolved && resolved_size > 0) {
        snprintf(resolved, resolved_size, "%s", candidate);
    }
    return chunk;
}

static Mix_Chunk* audioTryLoadFromRoot(const char* root,
                                       const char* filename,
                                       char* resolved,
                                       size_t resolved_size) {
    if (!root || root[0] == '\0' || !filename || filename[0] == '\0') {
        return NULL;
    }
    char candidate[PATH_MAX];
    int written = snprintf(candidate, sizeof(candidate), "%s/%s", root, filename);
    if (written <= 0 || (size_t)written >= sizeof(candidate)) {
        return NULL;
    }
    return audioTryLoadChunk(candidate, resolved, resolved_size);
}

static Mix_Chunk* audioResolveAndLoadChunk(const char* filename,
                                           char* resolved,
                                           size_t resolved_size) {
    Mix_Chunk* chunk = audioTryLoadChunk(filename, resolved, resolved_size);
    if (chunk) {
        return chunk;
    }
    if (!filename || filename[0] == '\0') {
        return NULL;
    }
    if (audioPathHasDirectorySeparator(filename)) {
        return NULL;
    }

    const char* searchEnv = getenv("PSCAL_SOUND_PATH");
    if (searchEnv && searchEnv[0] != '\0') {
        size_t envLen = strlen(searchEnv);
        char* roots = (char*)malloc(envLen + 1);
        if (roots) {
            memcpy(roots, searchEnv, envLen + 1);
            char* cursor = roots;
            while (cursor && cursor[0] != '\0') {
                char* sep = strchr(cursor, ':');
                if (sep) {
                    *sep = '\0';
                }
                if (cursor[0] != '\0') {
                    chunk = audioTryLoadFromRoot(cursor, filename, resolved, resolved_size);
                    if (chunk) {
                        free(roots);
                        return chunk;
                    }
                }
                if (!sep) {
                    break;
                }
                cursor = sep + 1;
            }
            free(roots);
        }
    }

    const char* fallbackRoots[] = {
        "/lib/sounds",
        "lib/sounds",
        "../lib/sounds"
    };
    for (size_t i = 0; i < sizeof(fallbackRoots) / sizeof(fallbackRoots[0]); ++i) {
        chunk = audioTryLoadFromRoot(fallbackRoots[i], filename, resolved, resolved_size);
        if (chunk) {
            return chunk;
        }
    }

    const char* installRootResolved = getenv("PSCAL_INSTALL_ROOT_RESOLVED");
    if (installRootResolved && installRootResolved[0] != '\0') {
        char installSounds[PATH_MAX];
        int written = snprintf(installSounds, sizeof(installSounds), "%s/lib/sounds", installRootResolved);
        if (written > 0 && (size_t)written < sizeof(installSounds)) {
            chunk = audioTryLoadFromRoot(installSounds, filename, resolved, resolved_size);
            if (chunk) {
                return chunk;
            }
        }
    }

    const char* installRoot = getenv("PSCAL_INSTALL_ROOT");
    if (installRoot && installRoot[0] != '\0') {
        char installSounds[PATH_MAX];
        int written = snprintf(installSounds, sizeof(installSounds), "%s/lib/sounds", installRoot);
        if (written > 0 && (size_t)written < sizeof(installSounds)) {
            chunk = audioTryLoadFromRoot(installSounds, filename, resolved, resolved_size);
            if (chunk) {
                return chunk;
            }
        }
    }

    chunk = audioTryLoadFromRoot(PSCAL_SOUNDS_DIR, filename, resolved, resolved_size);
    if (chunk) {
        return chunk;
    }

    return NULL;
}

// VM-native version of LoadSound.
Value vmBuiltinLoadsound(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "LoadSound expects 1 argument (FileName: String).");
        return makeInt(-1);
    }
    Value fileNameVal = args[0];
    if (fileNameVal.type != TYPE_STRING || fileNameVal.s_val == NULL) {
        runtimeError(vm, "LoadSound argument must be a valid String. Got %s.", varTypeToString(fileNameVal.type));
        return makeInt(-1);
    }

    int soundID = audioLoadSound(fileNameVal.s_val);
    return makeInt(soundID);
}

// Internal helper to set all entries in the loaded sounds array to NULL
void initializeSoundArray(void) {
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        gLoadedSounds[i] = NULL;
    }
    DEBUG_PRINT("[DEBUG AUDIO] gLoadedSounds array initialized.\n");
}

// Initialize the SDL audio subsystem and SDL_mixer
void audioInitSystem(void) {
    if (gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system is already initialized.\n");
        return; // Avoid double initialization
    }

    DEBUG_PRINT("[DEBUG AUDIO] Initializing sound system...\n");

    // Initialize SDL audio subsystem (if it hasn't been by SDL_Init(SDL_INIT_VIDEO))
    if ((PSCAL_SDL_WAS_INIT(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
         DEBUG_PRINT("[DEBUG AUDIO] SDL_INIT_AUDIO not yet initialized. Calling SDL_Init(SDL_INIT_AUDIO).\n");
        if (PSCAL_SDL_INIT_SUBSYSTEM(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "Runtime error: SDL_Init(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
            // Decide whether this is a fatal error or if we can continue without sound
            EXIT_FAILURE_HANDLER(); // For now, treat as fatal
        }
        DEBUG_PRINT("[DEBUG AUDIO] SDL_Init(SDL_INIT_AUDIO) successful.\n");
    } else {
         DEBUG_PRINT("[DEBUG AUDIO] SDL_INIT_AUDIO already initialized.\n");
    }

    // Initialize SDL_mixer. Specify desired audio formats (flags).
    // MIX_INIT_OGG and MIX_INIT_MP3 require external libraries (libvorbis, libmad/libmpg123).
    int mix_flags = 0;
    #ifdef INCLUDE_OGG_MP3_SUPPORT // Define this macro in your Makefile if you have the libs
    mix_flags |= MIX_INIT_OGG | MIX_INIT_MP3;
    #endif

    int initialized_flags = Mix_Init(mix_flags);
    // Check if all requested flags were initialized.
    // Note: Mix_Init might succeed but not enable all flags if libraries are missing.
    if ((initialized_flags & mix_flags) != mix_flags) {
        fprintf(stderr, "Runtime warning: Mix_Init failed to fully initialize requested formats. Check if Ogg/MP3 libraries are installed: %s\n", Mix_GetError());
        // Continue execution, but warn. WAV should still work if requested.
    } else {
         DEBUG_PRINT("[DEBUG AUDIO] Mix_Init successful with flags %d.\n", initialized_flags);
    }

    // Open the audio device.
    // Parameters: frequency (e.g. 44100), format (MIX_DEFAULT_FORMAT),
    // channels (1 for mono, 2 for stereo), chunksize (size of the audio buffer)
    if (Mix_OpenAudio(MIX_DEFAULT_FREQUENCY, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        fprintf(stderr, "Runtime error: Mix_OpenAudio failed: %s\n", SDL_GetError());
        Mix_Quit(); // Clean up any Mix_Init attempts before failing
        // Decide whether to exit or continue without audio. Let's exit for now.
        EXIT_FAILURE_HANDLER(); // Treat as fatal
    }
     DEBUG_PRINT("[DEBUG AUDIO] Mix_OpenAudio successful (Freq: %d, Format: %d, Channels: %d, Chunksize: %d).\n",
                 MIX_DEFAULT_FREQUENCY, MIX_DEFAULT_FORMAT, 2, 2048);


    // Optionally reserve some channels for specific sound types
    // Mix_ReserveChannels(num_channels);

    initializeSoundArray(); // Initialize the global sound array

    gSoundSystemInitialized = true;
    DEBUG_PRINT("[DEBUG AUDIO] Sound system initialization complete.\n");
}

// Load a sound file (like a .wav). Returns an integer ID (1-based index) or -1 on error.
int audioLoadSound(const char* filename) {
    if (!gSoundSystemInitialized) {
        fprintf(stderr, "Runtime error: Sound system not initialized. Call InitSoundSystem before LoadSound.\n");
        return -1; // Indicate error by returning -1
    }
    if (!filename || filename[0] == '\0') {
        fprintf(stderr, "Runtime error: LoadSound requires a valid filename string.\n");
        return -1;
    }

    DEBUG_PRINT("[DEBUG AUDIO] Attempting to load sound: '%s'\n", filename);

    // Find the first available slot in our loaded sounds array
    int soundID = -1; // Use 0-based index internally
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (gLoadedSounds[i] == NULL) {
            soundID = i;
            break;
        }
    }

    if (soundID == -1) {
        fprintf(stderr, "Runtime error: Maximum number of loaded sounds (%d) reached. Cannot load '%s'.\n", MAX_SOUNDS, filename);
        return -1; // Indicate error
    }

    // Load the sound file into a Mix_Chunk.
    // For bare filenames, fall back to common lib/sounds roots.
    char resolved_path[PATH_MAX];
    resolved_path[0] = '\0';
    Mix_Chunk* chunk = audioResolveAndLoadChunk(filename, resolved_path, sizeof(resolved_path));
    if (!chunk) {
        fprintf(stderr, "Runtime error: Mix_LoadWAV failed for '%s': %s\n", filename, Mix_GetError());
        return -1; // Indicate error
    }

    // Store the loaded chunk pointer in the array
    gLoadedSounds[soundID] = chunk;

    // Return a 1-based ID to the Pascal side (as Pascal arrays/indices are often 1-based)
    if (resolved_path[0] != '\0') {
        DEBUG_PRINT("[DEBUG AUDIO] Successfully loaded sound '%s' from '%s'. Assigned ID: %d (internal index %d).\n",
                    filename, resolved_path, soundID + 1, soundID);
    } else {
        DEBUG_PRINT("[DEBUG AUDIO] Successfully loaded sound '%s'. Assigned ID: %d (internal index %d).\n",
                    filename, soundID + 1, soundID);
    }
    return soundID + 1;
}

// Play a loaded sound effect once. Takes the 1-based sound ID.
void audioPlaySound(int soundID) {
    if (!gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system not initialized. Skipping PlaySound(ID: %d).\n", soundID);
        return; // Don't crash, just don't play sound
    }

    // Convert the 1-based Pascal ID to a 0-based C index
    int c_index = soundID - 1;

    // Validate the ID and check if a sound is actually loaded at that index
    if (c_index < 0 || c_index >= MAX_SOUNDS || gLoadedSounds[c_index] == NULL) {
        fprintf(stderr, "Runtime warning: PlaySound called with an invalid or unloaded SoundID %d.\n", soundID);
        return;
    }

    DEBUG_PRINT("[DEBUG AUDIO] Playing SoundID %d (internal index %d)...\n", soundID, c_index);

    // Play the loaded sound chunk.
    // Parameters: channel (-1 means find the first available), chunk (the Mix_Chunk* to play), loops (0 means play once).
    // Returns the channel the sound is playing on, or -1 on error.
    int played_channel = Mix_PlayChannel(-1, gLoadedSounds[c_index], 0);
    if (played_channel < 0) {
        fprintf(stderr, "Runtime warning: Mix_PlayChannel failed for SoundID %d: %s\n", soundID, Mix_GetError());
        // Continue, but warn
    } else {
        DEBUG_PRINT("[DEBUG AUDIO] Played SoundID %d on channel %d.\n", soundID, played_channel);
    }
}

// Free a loaded sound effect from memory. Takes the 1-based sound ID.
void audioFreeSound(int soundID) {
    if (!gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system not initialized. Skipping FreeSound(ID: %d).\n", soundID);
        return;
    }

    // Convert 1-based Pascal ID to 0-based C index
    int c_index = soundID - 1;

    // Validate the ID and check if there's a sound loaded at this index
    if (c_index < 0 || c_index >= MAX_SOUNDS || gLoadedSounds[c_index] == NULL) {
        fprintf(stderr, "Runtime warning: FreeSound called with invalid or unloaded SoundID %d.\n", soundID);
        return;
    }

    DEBUG_PRINT("[DEBUG AUDIO] Freeing sound ID %d (internal index %d)...\n", soundID, c_index);

    // Free the Mix_Chunk data.
    Mix_FreeChunk(gLoadedSounds[c_index]);
    gLoadedSounds[c_index] = NULL; // Set the array entry to NULL to mark the slot as free

    DEBUG_PRINT("[DEBUG AUDIO] Sound ID %d freed successfully.\n", soundID);
}

// Stop all currently playing sounds (including music) without tearing down the
// sound system. Safe to call even if nothing is playing.
void audioStopAllSounds(void) {
    if (!gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system not initialized. Skipping StopAllSounds.\n");
        return;
    }

    Mix_HaltGroup(-1);
    Mix_HaltMusic();
    DEBUG_PRINT("[DEBUG AUDIO] StopAllSounds halted all channels and music.\n");
}

// Shut down SDL_mixer and the SDL audio subsystem
void audioQuitSystem(void) {
    if (!gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system not initialized. Skipping audioQuitSystem.\n");
        return;
    }
    DEBUG_PRINT("[DEBUG AUDIO] Shutting down sound system (called by Pscal's QuitSoundSystem)...\n");

    Mix_HaltGroup(-1);
    Mix_HaltMusic();

    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (gLoadedSounds[i] != NULL) {
            Mix_FreeChunk(gLoadedSounds[i]);
            gLoadedSounds[i] = NULL;
            DEBUG_PRINT("[DEBUG AUDIO] Freed sound chunk at index %d during audioQuitSystem.\n", i);
        }
    }
    DEBUG_PRINT("[DEBUG AUDIO] All user-loaded sound chunks freed by audioQuitSystem.\n");

    // Close the audio device. This makes sense here as the sound system is being "quit" from Pscal's perspective.
    // sdlCleanupAtExit will also call it, but Mix_CloseAudio can be called multiple times, though only first has effect.
    // For safety, let's only close if it's known to be open.
    // SDL_WasInit(SDL_INIT_AUDIO) can check if the subsystem was ever inited.
    // Mix_QuerySpec can check if audio is open.
    int open_freq, open_channels;
    Uint16 open_format;
    if (Mix_QuerySpec(&open_freq, &open_format, &open_channels) != 0) { // Returns 1 if audio is open
        Mix_CloseAudio();
        DEBUG_PRINT("[DEBUG AUDIO] Mix_CloseAudio called from audioQuitSystem.\n");
    } else {
        DEBUG_PRINT("[DEBUG AUDIO] Mix_CloseAudio skipped in audioQuitSystem (audio not open or already closed).\n");
    }


    // DO NOT CALL Mix_Quit() here. Let sdlCleanupAtExit handle the final Mix_Quit().
    // Mix_Quit(); // <<< REMOVE OR COMMENT OUT

    gSoundSystemInitialized = false; // Mark as no longer initialized by Pscal logic
    DEBUG_PRINT("[DEBUG AUDIO] Pscal sound system shutdown procedures complete (Mix_Quit deferred to global exit).\n");
}

// The builtins (assuming these are correctly placed after Audio_... function definitions)

Value vmBuiltinInitsoundsystem(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "InitSoundSystem expects 0 arguments.");
    else audioInitSystem();
    return makeVoid();
}

Value vmBuiltinPlaysound(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "PlaySound expects 1 integer argument.");
    } else {
        audioPlaySound((int)AS_INTEGER(args[0]));
    }
    return makeVoid();
}

Value vmBuiltinFreesound(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "FreeSound expects 1 integer argument.");
    } else {
        audioFreeSound((int)AS_INTEGER(args[0]));
    }
    return makeVoid();
}

Value vmBuiltinStopallsounds(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "StopAllSounds expects 0 arguments.");
    } else {
        audioStopAllSounds();
    }
    return makeVoid();
}

Value vmBuiltinQuitsoundsystem(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "QuitSoundSystem expects 0 arguments.");
    else audioQuitSystem();
    return makeVoid();
}

Value vmBuiltinIssoundplaying(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "IsSoundPlaying expects 0 arguments.");
        return makeBoolean(false);
    }
    if (!gSoundSystemInitialized) {
        return makeBoolean(false);
    }
    int playing = Mix_Playing(-1);
    return makeBoolean(playing != 0);
}
#endif
