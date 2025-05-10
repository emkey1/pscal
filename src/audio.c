// audio.c
//
// audio.c
// Pscal
//
// Created by Michael Miller on 5/10/25.
//

#include "audio.h"
#include "globals.h" // For EXIT_FAILURE_HANDLER
#include "interpreter.h" // For EXIT_FAILURE_HANDLER
#include "utils.h" // For EXIT_FAILURE_HANDLER
#include <stdio.h>
#include <string.h> // For strdup
#include <SDL2/SDL.h> // Need basic SDL for SDL_InitSubSystem, SDL_WasInit

// Define and initialize global variables from audio.h
Mix_Chunk* gLoadedSounds[MAX_SOUNDS];
bool gSoundSystemInitialized = false;

// Internal helper to set all entries in the loaded sounds array to NULL
void InitializeSoundArray(void) {
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        gLoadedSounds[i] = NULL;
    }
    DEBUG_PRINT("[DEBUG AUDIO] gLoadedSounds array initialized.\n");
}

// Initialize the SDL audio subsystem and SDL_mixer
void Audio_InitSystem(void) {
    if (gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system is already initialized.\n");
        return; // Avoid double initialization
    }

    DEBUG_PRINT("[DEBUG AUDIO] Initializing sound system...\n");

    // Initialize SDL audio subsystem (if it hasn't been by SDL_Init(SDL_INIT_VIDEO))
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
         DEBUG_PRINT("[DEBUG AUDIO] SDL_INIT_AUDIO not yet initialized. Calling SDL_InitSubSystem(SDL_INIT_AUDIO).\n");
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "Runtime error: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
            // Decide whether this is a fatal error or if we can continue without sound
            EXIT_FAILURE_HANDLER(); // For now, treat as fatal
        }
        DEBUG_PRINT("[DEBUG AUDIO] SDL_InitSubSystem(SDL_INIT_AUDIO) successful.\n");
    } else {
         DEBUG_PRINT("[DEBUG AUDIO] SDL_INIT_AUDIO already initialized.\n");
    }

    // Initialize SDL_mixer. Specify desired audio formats (flags).
    // MIX_INIT_OGG and MIX_INIT_MP3 require external libraries (libvorbis, libmad/libmpg123).
    // MIX_INIT_WAV is typically built-in.
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

    InitializeSoundArray(); // Initialize the global sound array

    gSoundSystemInitialized = true;
    DEBUG_PRINT("[DEBUG AUDIO] Sound system initialization complete.\n");
}

// Load a sound file (like a .wav). Returns an integer ID (1-based index) or -1 on error.
int Audio_LoadSound(const char* filename) {
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

    // Load the sound file into a Mix_Chunk
    // Mix_LoadWAV is a common loader, can often handle formats initialized by Mix_Init
    Mix_Chunk* chunk = Mix_LoadWAV(filename);
    if (!chunk) {
        fprintf(stderr, "Runtime error: Mix_LoadWAV failed for '%s': %s\n", filename, Mix_GetError());
        return -1; // Indicate error
    }

    // Store the loaded chunk pointer in the array
    gLoadedSounds[soundID] = chunk;

    // Return a 1-based ID to the Pascal side (as Pascal arrays/indices are often 1-based)
    DEBUG_PRINT("[DEBUG AUDIO] Successfully loaded sound '%s'. Assigned ID: %d (internal index %d).\n", filename, soundID + 1, soundID);
    return soundID + 1;
}

// Play a loaded sound effect once. Takes the 1-based sound ID.
void Audio_PlaySound(int soundID) {
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
void Audio_FreeSound(int soundID) {
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

// Shut down SDL_mixer and the SDL audio subsystem
void Audio_QuitSystem(void) {
    if (!gSoundSystemInitialized) {
        DEBUG_PRINT("[DEBUG AUDIO] Sound system not initialized. Skipping Audio_QuitSystem.\n");
        return; // Avoid errors if not initialized
    }

    DEBUG_PRINT("[DEBUG AUDIO] Shutting down sound system...\n");

    // Halt any currently playing sound effects and music (optional but good practice)
    Mix_HaltGroup(-1); // Stop all sound effects
    Mix_HaltMusic();   // Stop music

    // Free all loaded sound chunks that haven't been freed yet
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (gLoadedSounds[i] != NULL) {
            Mix_FreeChunk(gLoadedSounds[i]);
            gLoadedSounds[i] = NULL;
            DEBUG_PRINT("[DEBUG AUDIO] Auto-freed sound chunk at index %d during quit.\n", i);
        }
    }
     DEBUG_PRINT("[DEBUG AUDIO] All loaded sound chunks auto-freed.\n");

    // Close the audio device
    Mix_CloseAudio();
    DEBUG_PRINT("[DEBUG AUDIO] Mix_CloseAudio successful.\n");

    // Quit SDL_mixer subsystems
    Mix_Quit(); // This cleans up all initialized formats (OGG, MP3, etc.)
    DEBUG_PRINT("[DEBUG AUDIO] Mix_Quit successful.\n");


    // Quit the SDL audio subsystem. This is often handled by the main SDL_Quit()
    // at the end of the program, but if you initialized it separately, quitting
    // it explicitly here is correct.
    // SDL_QuitSubSystem(SDL_INIT_AUDIO); // Call this if you used SDL_InitSubSystem

    gSoundSystemInitialized = false;
    DEBUG_PRINT("[DEBUG AUDIO] Sound system shutdown complete.\n");
}

// The builtins (assuming these are correctly placed after Audio_... function definitions)

Value executeBuiltinInitSoundSystem(AST *node) {
    // Check that no arguments were provided
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: InitSoundSystem expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER(); // Treat as fatal error for incorrect usage
    }
    Audio_InitSystem(); // Call the C-side initialization helper
    return makeVoid(); // Procedures return a void value
}

// Pascal: function LoadSound(FileName: String): Integer;
Value executeBuiltinLoadSound(AST *node) {
    // Check that exactly one argument was provided
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: LoadSound expects 1 argument (FileName: String).\n");
        EXIT_FAILURE_HANDLER();
    }
    // Evaluate the argument (which should be a string expression)
    Value fileNameVal = eval(node->children[0]);
    // Check if the evaluated argument is a valid string
    if (fileNameVal.type != TYPE_STRING || fileNameVal.s_val == NULL) {
        fprintf(stderr, "Runtime error: LoadSound argument must be a valid String. Got %s.\n", varTypeToString(fileNameVal.type));
        freeValue(&fileNameVal); // Free the evaluated value
        EXIT_FAILURE_HANDLER();
    }

    // Call the C-side sound loading helper
    int soundID = Audio_LoadSound(fileNameVal.s_val);

    freeValue(&fileNameVal); // Free the evaluated string value after use

    // Return the sound ID as a Pascal Integer Value
    return makeInt(soundID); // Returns 1-based ID or -1 on error
}

// Pascal: procedure PlaySound(SoundID: Integer);
Value executeBuiltinPlaySound(AST *node) {
    // Check that exactly one argument was provided
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: PlaySound expects 1 argument (SoundID: Integer).\n");
        EXIT_FAILURE_HANDLER();
    }
    // Evaluate the argument (which should be an integer expression for the sound ID)
    Value soundIDVal = eval(node->children[0]);
    // Check if the evaluated argument is an integer
    if (soundIDVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: PlaySound argument must be an Integer SoundID. Got %s.\n", varTypeToString(soundIDVal.type));
        freeValue(&soundIDVal);
        EXIT_FAILURE_HANDLER();
    }

    // Call the C-side sound playing helper with the Pascal 1-based ID
    Audio_PlaySound((int)soundIDVal.i_val);

    freeValue(&soundIDVal); // Free the evaluated integer value

    return makeVoid(); // Procedures return a void value
}

// Pascal: procedure QuitSoundSystem;
Value executeBuiltinQuitSoundSystem(AST *node) {
    // Check that no arguments were provided
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: QuitSoundSystem expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    Audio_QuitSystem(); // Call the C-side cleanup helper
    return makeVoid(); // Procedures return a void value
}
