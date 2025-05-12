//
//  sdl.c
//  Pscal
//
//  Created by Michael Miller on 5/7/25.
//
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
// <<< MODIFICATION START >>>
// Include SDL_mixer header directly
#include <SDL2/SDL_mixer.h>
// Include audio.h directly (declares MAX_SOUNDS and gLoadedSounds)
#include "audio.h"
// <<< MODIFICATION END >>>

#include "sdl.h" // This header includes SDL/SDL_ttf headers
#include "globals.h" // Includes SDL.h and SDL_ttf.h via its includes, and audio.h
#include "types.h"
#include "ast.h"
#include "interpreter.h"
#include "utils.h"
#include "builtin.h"

void InitializeTextureSystem(void) {
    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        gSdlTextures[i] = NULL;
        gSdlTextureWidths[i] = 0;
        gSdlTextureHeights[i] = 0;
    }
}

Value executeBuiltinInitGraph(AST *node) {
    // --- Initialize SDL if not already done ---
    if (!gSdlInitialized) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG InitGraph] SDL not initialized. Calling SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER).\n");
        #endif
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
            fprintf(stderr, "Runtime error: SDL_Init failed in InitGraph: %s\n", SDL_GetError());
            EXIT_FAILURE_HANDLER();
        }
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG InitGraph] SDL_Init successful.\n");
        #endif
        gSdlInitialized = true;
    }

    // --- Argument checks ---
    if (node->child_count != 3) {
        fprintf(stderr, "Runtime error: InitGraph expects 3 arguments (Width, Height: Integer; Title: String).\n");
        // Clean up previously eval'd args if any were done before check
        EXIT_FAILURE_HANDLER();
    }
    Value widthVal = eval(node->children[0]);
    Value heightVal = eval(node->children[1]);
    Value titleVal = eval(node->children[2]);

    if (widthVal.type != TYPE_INTEGER || heightVal.type != TYPE_INTEGER || titleVal.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: InitGraph argument type mismatch.\n");
        freeValue(&widthVal); freeValue(&heightVal); freeValue(&titleVal);
        EXIT_FAILURE_HANDLER();
    }

    // Check if already initialized (optional: cleanup or error)
    if (gSdlWindow || gSdlRenderer) {
        // This logic might be better in a CloseGraph if re-init is not desired
        // For now, let's assume we can reinitialize or this is the first proper init.
        #ifdef DEBUG
        fprintf(stderr, "Warning [InitGraph]: Graphics system (window/renderer) already seems initialized. Recreating.\n");
        #endif
        if(gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
        if(gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
    }

    int width = (int)widthVal.i_val;
    int height = (int)heightVal.i_val;
    const char* title = titleVal.s_val ? titleVal.s_val : "Pscal Graphics";

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Runtime error: InitGraph width and height must be positive.\n");
        freeValue(&widthVal); freeValue(&heightVal); freeValue(&titleVal);
        EXIT_FAILURE_HANDLER();
    }

    // --- Create Window ---
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitGraph] Creating window (%dx%d, Title: '%s')...\n", width, height, title);
    #endif
    // Window is created SHOWN by default
    gSdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
    if (!gSdlWindow) {
        fprintf(stderr, "Runtime error: SDL_CreateWindow failed: %s\n", SDL_GetError());
        freeValue(&widthVal); freeValue(&heightVal); freeValue(&titleVal);
        EXIT_FAILURE_HANDLER();
    }
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitGraph] SDL_CreateWindow successful (Window: %p).\n", (void*)gSdlWindow);
    #endif

    gSdlWidth = width;
    gSdlHeight = height;

    // --- Create Renderer ---
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitGraph] Creating renderer...\n");
    #endif
    // Consider making VSync configurable or testing without it for macOS issues
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_ACCELERATED /* | SDL_RENDERER_PRESENTVSYNC */);
    if (!gSdlRenderer) {
        fprintf(stderr, "Runtime error: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
        freeValue(&widthVal); freeValue(&heightVal); freeValue(&titleVal);
        EXIT_FAILURE_HANDLER();
    }
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitGraph] SDL_CreateRenderer successful (Renderer: %p).\n", (void*)gSdlRenderer);
    #endif

    InitializeTextureSystem(); // Initialize our global texture array to NULLs

    // --- Initial Clear and Present ---
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitGraph] Performing initial clear (to black) and present...\n");
    #endif
    if (SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255) != 0) { // Clear to black
        fprintf(stderr, "Runtime Warning [InitGraph]: SDL_SetRenderDrawColor (background) failed: %s\n", SDL_GetError());
    }
    if (SDL_RenderClear(gSdlRenderer) != 0) {
        fprintf(stderr, "Runtime Warning [InitGraph]: SDL_RenderClear failed: %s\n", SDL_GetError());
    }
    SDL_RenderPresent(gSdlRenderer); // Present the cleared (black) screen immediately

    // Set default drawing color for subsequent Pscal SetRGBColor/PutPixel calls (e.g., to white)
    gSdlCurrentColor.r = 255; gSdlCurrentColor.g = 255; gSdlCurrentColor.b = 255; gSdlCurrentColor.a = 255;
    // The Pscal program should call SetRGBColor explicitly before drawing if it wants a specific color.
    // No need to call SDL_SetRenderDrawColor here again for the *default state*.

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitGraph] Initial clear and present finished.\n");
    #endif

    // Free evaluated arguments
    freeValue(&widthVal);
    freeValue(&heightVal);
    freeValue(&titleVal);

    return makeVoid();
}

// Pscal: procedure CloseGraph;
Value executeBuiltinCloseGraph(AST *node) {
     if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: CloseGraph expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }

    if (gSdlRenderer) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
    if (gSdlWindow) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }
    
    // Destroy renderer and window if they exist
    if (gSdlRenderer) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
    }
    if (gSdlWindow) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
    }
    // Note: We DO NOT set gSdlInitialized back to false here.
    // SDL_Quit() in main handles full subsystem cleanup.
    // Calling InitGraph again will just create new window/renderer.

    return makeVoid();
}

Value executeBuiltinGraphLoop(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: graphloop expects 1 argument (milliseconds).\n");
        EXIT_FAILURE_HANDLER();
    }

    Value msVal = eval(node->children[0]);
    if (msVal.type != TYPE_INTEGER && msVal.type != TYPE_WORD && msVal.type != TYPE_BYTE) {
         fprintf(stderr, "Runtime error: graphloop argument must be an integer-like type. Got %s\n", varTypeToString(msVal.type));
         freeValue(&msVal);
         EXIT_FAILURE_HANDLER();
    }

    long long ms = msVal.i_val;
    freeValue(&msVal); // Free evaluated value

    if (ms < 0) ms = 0; // Treat negative delay as 0

    // Only poll events if SDL video is actually initialized
    if (gSdlInitialized && gSdlWindow && gSdlRenderer) { // Added check for window/renderer
        Uint32 startTime = SDL_GetTicks();
        Uint32 targetTime = startTime + (Uint32)ms; // Calculate end time
        SDL_Event event; // Structure to hold event data

        #ifdef DEBUG
        fprintf(stderr, "[DEBUG GraphLoop] Starting SDL Delay/Event Loop for %lld ms. Start: %u, Target: %u\n", ms, startTime, targetTime);
        #endif

        // Loop until target time is reached
        while (SDL_GetTicks() < targetTime) {
            while (SDL_PollEvent(&event)) {
                 #ifdef DEBUG
                 if (dumpExec) {
                     fprintf(stderr, "[DEBUG GraphLoop] Polled event type: %d\n", event.type);
                 }
                 #endif
                 // Check specifically for the quit event
                // Handle different event types
                if (event.type == SDL_QUIT) {
                    // User closed the window
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG GraphLoop] SDL_QUIT event detected.\n");
                    #endif
                    break_requested = 1; // Signal Pscal to quit by setting the global flag
                    // Exit the inner event loop immediately
                    break; // Exit the while(SDL_PollEvent) loop
                } else if (event.type == SDL_KEYDOWN) {
                    // A key was pressed
                    #ifdef DEBUG
                    fprintf(stderr, "[DEBUG GraphLoop] SDL_KEYDOWN event detected. Key sym: %d ('%s')\n",
                            event.key.keysym.sym, SDL_GetKeyName(event.key.keysym.sym));
                    #endif
                    // Check if the pressed key is 'q' (SDLK_q)
                    if (event.key.keysym.sym == SDLK_q) {
                        #ifdef DEBUG
                        fprintf(stderr, "[DEBUG GraphLoop] 'q' key pressed.\n");
                        #endif
                        break_requested = 1; // Signal Pscal to quit
                         // Exit the inner event loop immediately
                        break; // Exit the while(SDL_PollEvent) loop
                    }
                    // Add more key checks here if needed (e.g., SDLK_ESCAPE)
                }
                 // Add handling for other events if needed (keyboard, mouse)
            } // End SDL_PollEvent while loop
            
            // If break_requested was set by an event, exit the outer loop too
            if (break_requested != 0) {
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG GraphLoop] break_requested set during event polling. Exiting time loop.\n");
                #endif
                break; // Exit the while (SDL_GetTicks() < targetTime) loop
            }

            // Add a small delay to prevent 100% CPU usage if the event queue is empty
            // Only delay if no quit was requested and we still have time
            if (SDL_GetTicks() < targetTime && break_requested == 0) {
                SDL_Delay(1); // Wait for 1 millisecond
            }

            // Prevent busy-waiting: Give a tiny bit of time back to the OS.
            SDL_Delay(1); // Wait 1 millisecond
        } // End while (SDL_GetTicks() < targetTime) loop
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG GraphLoop] Finished SDL Delay/Event Loop. End: %u\n", SDL_GetTicks());
        #endif

    } else {
        // If SDL not initialized, graphloop maybe does nothing or warns?
        // Let's just do nothing, as it's graphics-specific.
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG GraphLoop] SDL not initialized or window/renderer missing. graphloop(%lld) doing nothing.\n", ms);
        #endif
    }

    return makeVoid();
}

Value executeBuiltinGetMaxX(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: GetMaxX expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlWindow) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before GetMaxX.\n");
         EXIT_FAILURE_HANDLER();
    }
    return makeInt(gSdlWidth - 1); // Return 0-based max coordinate
}

// --- GetMaxY ---
// Pascal: function GetMaxY: Integer;
Value executeBuiltinGetMaxY(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: GetMaxY expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
     if (!gSdlInitialized || !gSdlWindow) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before GetMaxY.\n");
         EXIT_FAILURE_HANDLER();
    }
    return makeInt(gSdlHeight - 1); // Return 0-based max coordinate
}

// --- SetColor ---
// Pascal: procedure SetColor(Color: Integer); // Using integer 0-255 for simplicity
Value executeBuiltinSetColor(AST *node) {
     if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: SetColor expects 1 argument (color index 0-255).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before SetColor.\n");
         EXIT_FAILURE_HANDLER();
    }
    Value colorVal = eval(node->children[0]);
    if (colorVal.type != TYPE_INTEGER && colorVal.type != TYPE_BYTE) {
         fprintf(stderr, "Runtime error: SetColor argument must be an integer or byte.\n");
         freeValue(&colorVal); EXIT_FAILURE_HANDLER();
    }
    long long colorCode = colorVal.i_val;
    freeValue(&colorVal);

    // --- Simple Color Mapping Example (Expand later if needed) ---
    // This maps integer 0-15 to CGA-like colors, others cycle through a basic spectrum.
    // You can implement more sophisticated palettes.
    if (colorCode >= 0 && colorCode <= 15) {
         // Basic 16 colors (approximated)
         unsigned char intensity = (colorCode > 7) ? 255 : 192; // Brighter for codes 8-15
         gSdlCurrentColor.r = (colorCode & 4) ? intensity : 0; // Red component
         gSdlCurrentColor.g = (colorCode & 2) ? intensity : 0; // Green component
         gSdlCurrentColor.b = (colorCode & 1) ? intensity : 0; // Blue component
         if (colorCode == 6) { gSdlCurrentColor.g = intensity / 2; } // Brownish adjustment
         if (colorCode == 7 || colorCode == 15) { gSdlCurrentColor.r=intensity; gSdlCurrentColor.g=intensity; gSdlCurrentColor.b=intensity; } // Greys
         if (colorCode == 8) {gSdlCurrentColor.r = 128; gSdlCurrentColor.g = 128; gSdlCurrentColor.b = 128;} // Dark Grey specific
         if (colorCode == 0) {gSdlCurrentColor.r = 0; gSdlCurrentColor.g = 0; gSdlCurrentColor.b = 0;} // Black specific
    } else {
         // Basic cycle for other colors (simple example)
         int c = (int)(colorCode % 256);
         gSdlCurrentColor.r = (c * 3) % 256;
         gSdlCurrentColor.g = (c * 5) % 256;
         gSdlCurrentColor.b = (c * 7) % 256;
    }
    gSdlCurrentColor.a = 255; // Full alpha
    // --- End Simple Color Mapping ---

    // Set the color for subsequent drawing operations
    if(SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
        fprintf(stderr, "Runtime Warning: SetRenderDrawColor failed in SetColor: %s\n", SDL_GetError());
    }

    return makeVoid();
}

// --- PutPixel ---
// Pscal: procedure PutPixel(X, Y: Integer); // Draws using the current color
Value executeBuiltinPutPixel(AST *node) {
     if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: PutPixel expects 2 arguments (X, Y).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before PutPixel.\n");
         EXIT_FAILURE_HANDLER();
    }
    Value xVal = eval(node->children[0]);
    Value yVal = eval(node->children[1]);

    if (xVal.type != TYPE_INTEGER || yVal.type != TYPE_INTEGER) {
         fprintf(stderr, "Runtime error: PutPixel coordinates must be integers.\n");
         freeValue(&xVal); freeValue(&yVal); EXIT_FAILURE_HANDLER();
    }
    int x = (int)xVal.i_val;
    int y = (int)yVal.i_val;
    freeValue(&xVal); freeValue(&yVal);

    // Set the draw color (redundant if SetColor was just called, but safe)
    if(SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
         fprintf(stderr, "Runtime Warning: SetRenderDrawColor failed in PutPixel: %s\n", SDL_GetError());
         // Continue attempt to draw anyway?
    }

    // Draw the point
    if(SDL_RenderDrawPoint(gSdlRenderer, x, y) != 0) {
          fprintf(stderr, "Runtime Warning: RenderDrawPoint failed in PutPixel: %s\n", SDL_GetError());
    }

    return makeVoid();
}

// --- UpdateScreen ---
// Pscal: procedure UpdateScreen;
Value executeBuiltinUpdateScreen(AST *node) {
     if (node->child_count != 0) {
         fprintf(stderr, "Runtime error: UpdateScreen expects 0 arguments.\n");
         EXIT_FAILURE_HANDLER();
     }
     if (!gSdlInitialized || !gSdlRenderer) {
          fprintf(stderr, "Runtime error: Graphics mode not initialized before UpdateScreen.\n");
          EXIT_FAILURE_HANDLER();
     }

     // Process pending events to keep the window responsive
     SDL_Event event;
     while (SDL_PollEvent(&event)) {
         // Currently, we don't act on events here, just process the queue.
         // A full application might check for SDL_QUIT here.
         #ifdef DEBUG
         if(dumpExec) {
             if (event.type == SDL_QUIT) {
                 fprintf(stderr, "[DEBUG UpdateScreen] SDL_QUIT event polled but not handled here.\n");
             }
             // else { fprintf(stderr, "[DEBUG UpdateScreen] Polled event type: %d\n", event.type); } // Optional: Log all events
         }
         #endif
     }

     #ifdef DEBUG
     fprintf(stderr, "[DEBUG UpdateScreen] Calling SDL_RenderPresent(%p)\n", (void*)gSdlRenderer);
     #endif
     SDL_RenderPresent(gSdlRenderer); // Show buffer on screen

     const char *err = SDL_GetError();
     if (err && err[0] != '\0') { // Check if error string is not empty
         fprintf(stderr, "Runtime Warning: SDL Error state after RenderPresent: %s\n", err);
         SDL_ClearError(); // Clear error state after reporting
     }

     #ifdef DEBUG
     fprintf(stderr, "[DEBUG UpdateScreen] SDL_RenderPresent finished.\n");
     #endif
     return makeVoid();
}

// Pscal: procedure DrawRect(X1, Y1, X2, Y2: Integer);
Value executeBuiltinDrawRect(AST *node) {
    if (node->child_count != 4) {
        fprintf(stderr, "Runtime error: DrawRect expects 4 integer arguments (X1, Y1, X2, Y2).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before DrawRect.\n");
         EXIT_FAILURE_HANDLER();
    }

    // Evaluate arguments
    Value x1Val = eval(node->children[0]);
    Value y1Val = eval(node->children[1]);
    Value x2Val = eval(node->children[2]);
    Value y2Val = eval(node->children[3]);

    // Type checking
    if (x1Val.type != TYPE_INTEGER || y1Val.type != TYPE_INTEGER ||
        x2Val.type != TYPE_INTEGER || y2Val.type != TYPE_INTEGER)
    {
        fprintf(stderr, "Runtime error: DrawRect arguments must be integers.\n");
        freeValue(&x1Val); freeValue(&y1Val); freeValue(&x2Val); freeValue(&y2Val);
        EXIT_FAILURE_HANDLER();
    }

    // Extract coordinates
    int x1 = (int)x1Val.i_val;
    int y1 = (int)y1Val.i_val;
    int x2 = (int)x2Val.i_val;
    int y2 = (int)y2Val.i_val;

    // Free evaluated arguments
    freeValue(&x1Val); freeValue(&y1Val); freeValue(&x2Val); freeValue(&y2Val);

    // Create SDL_Rect (SDL requires x, y, width, height)
    // Handle potential swapped coordinates gracefully
    SDL_Rect rect;
    rect.x = (x1 < x2) ? x1 : x2;
    rect.y = (y1 < y2) ? y1 : y2;
    rect.w = abs(x2 - x1) + 1; // Width includes both endpoints
    rect.h = abs(y2 - y1) + 1; // Height includes both endpoints

    // Set the draw color (using the globally stored current color)
    if (SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
         fprintf(stderr, "Runtime Warning: SetRenderDrawColor failed in DrawRect: %s\n", SDL_GetError());
         // Continue anyway? Or return error?
    }

    // Draw the rectangle outline
    if (SDL_RenderDrawRect(gSdlRenderer, &rect) != 0) {
        fprintf(stderr, "Runtime Warning: RenderDrawRect failed: %s\n", SDL_GetError());
    }

    return makeVoid();
}

// Pscal: procedure WaitKeyEvent; // Blocks until key press or window close
Value executeBuiltinWaitKeyEvent(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: WaitKeyEvent expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }

    // Check if graphics subsystem is initialized
    if (!gSdlInitialized || !gSdlWindow || !gSdlRenderer) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before WaitKeyEvent.\n");
         // Don't wait if graphics aren't running
         return makeVoid();
    }

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG WaitKeyEvent] Entering SDL_WaitEvent loop...\n");
    #endif

    SDL_Event event;
    int waiting = 1;
    while (waiting) {
        // Wait indefinitely for the next event
        if (SDL_WaitEvent(&event)) {
            #ifdef DEBUG
            if(dumpExec) fprintf(stderr, "[DEBUG WaitKeyEvent] Event received: type=%d\n", event.type);
            #endif
            // Check if the event is a quit request or a keydown event
            if (event.type == SDL_QUIT) {
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG WaitKeyEvent] SDL_QUIT event detected. Exiting wait loop.\n");
                 #endif
                 waiting = 0; // Exit the loop
            } else if (event.type == SDL_KEYDOWN) {
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG WaitKeyEvent] SDL_KEYDOWN event detected. Exiting wait loop.\n");
                 #endif
                 waiting = 0; // Exit the loop
            }
            // Add checks for other events if needed (e.g., mouse clicks)
        } else {
            // SDL_WaitEvent returning 0 usually indicates an error
            fprintf(stderr, "Runtime error: SDL_WaitEvent failed: %s\n", SDL_GetError());
            waiting = 0; // Exit loop on error
        }
    } // End while(waiting)

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG WaitKeyEvent] Exited SDL_WaitEvent loop.\n");
    #endif

    return makeVoid(); // WaitKeyEvent is a procedure
}

// Pscal: procedure ClearDevice;
Value executeBuiltinClearDevice(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: ClearDevice expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before ClearDevice.\n");
        // Maybe just return void instead of exiting?
        return makeVoid();
    }

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG ClearDevice] Clearing screen.\n");
    #endif

    // Set draw color to the current background color (or default black)
    // For simplicity, let's use black (0,0,0) for now.
    // A more advanced version might use a global background color variable.
    if (SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255) != 0) {
        fprintf(stderr, "Runtime Warning [ClearDevice]: SDL_SetRenderDrawColor failed: %s\n", SDL_GetError());
    }

    // Clear the entire rendering target
    if (SDL_RenderClear(gSdlRenderer) != 0) {
        fprintf(stderr, "Runtime Warning [ClearDevice]: SDL_RenderClear failed: %s\n", SDL_GetError());
    }

    // Note: ClearDevice does NOT call RenderPresent.
    // The changes will be visible only after the next UpdateScreen.

    return makeVoid();
}

Value executeBuiltinSetRGBColor(AST *node) {
    if (node->child_count != 3) {
        fprintf(stderr, "Runtime error: SetRGBColor expects 3 arguments (R, G, B: Byte).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
         fprintf(stderr, "Runtime error: Graphics mode not initialized before SetRGBColor.\n");
         EXIT_FAILURE_HANDLER(); // Or return makeVoid() if you want to allow it to fail silently
    }

    Value r_val = eval(node->children[0]);
    Value g_val = eval(node->children[1]);
    Value b_val = eval(node->children[2]);

    // Type checking (allow Byte or Integer, then cast to Uint8 for SDL)
    if ( (r_val.type != TYPE_INTEGER && r_val.type != TYPE_BYTE) ||
         (g_val.type != TYPE_INTEGER && g_val.type != TYPE_BYTE) ||
         (b_val.type != TYPE_INTEGER && b_val.type != TYPE_BYTE) ) {
        fprintf(stderr, "Runtime error: SetRGBColor arguments must be Integer or Byte. Got R:%s G:%s B:%s\n",
                varTypeToString(r_val.type), varTypeToString(g_val.type), varTypeToString(b_val.type));
        freeValue(&r_val); freeValue(&g_val); freeValue(&b_val);
        EXIT_FAILURE_HANDLER();
    }

    long long r_ll = r_val.i_val;
    long long g_ll = g_val.i_val;
    long long b_ll = b_val.i_val;

    freeValue(&r_val);
    freeValue(&g_val);
    freeValue(&b_val);

    // Clamp values to 0-255 and store in global SDL_Color
    gSdlCurrentColor.r = (r_ll < 0) ? 0 : (r_ll > 255) ? 255 : (Uint8)r_ll;
    gSdlCurrentColor.g = (g_ll < 0) ? 0 : (g_ll > 255) ? 255 : (Uint8)g_ll;
    gSdlCurrentColor.b = (b_ll < 0) ? 0 : (b_ll > 255) ? 255 : (Uint8)b_ll;
    gSdlCurrentColor.a = 255; // Full opacity

    // Set the color for subsequent drawing operations in SDL
    // This is crucial if PutPixel doesn't take color directly
    if(SDL_SetRenderDrawColor(gSdlRenderer,
                             gSdlCurrentColor.r,
                             gSdlCurrentColor.g,
                             gSdlCurrentColor.b,
                             gSdlCurrentColor.a) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_SetRenderDrawColor failed in SetRGBColor: %s\n", SDL_GetError());
    }
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SetRGBColor] Set color to R:%d G:%d B:%d\n", gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b);
    #endif

    return makeVoid();
}

Value executeBuiltinInitTextSystem(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: InitTextSystem expects 2 arguments (FontFileName: String; FontSize: Integer).\n");
        EXIT_FAILURE_HANDLER(); // Or return an error indicator
    }
    // Graphics system must be up before we can think about text on it.
    if (!gSdlInitialized || !gSdlRenderer) { // gSdlInitialized refers to core SDL_Init(VIDEO)
        fprintf(stderr, "Runtime error: Core SDL Graphics not initialized before InitTextSystem.\n");
        EXIT_FAILURE_HANDLER();
    }

    // >>> Lazy Initialize SDL_ttf if not already done <<<
    if (!gSdlTtfInitialized) {
        if (TTF_Init() == -1) {
            fprintf(stderr, "Runtime error: SDL_ttf system initialization failed: %s\n", TTF_GetError());
            // No font loaded yet, so just exit or return error
            EXIT_FAILURE_HANDLER();
        }
        gSdlTtfInitialized = true;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG InitTextSystem] SDL_ttf system initialized (lazily).\n");
        #endif
    }

    Value fontNameVal = eval(node->children[0]);
    Value fontSizeVal = eval(node->children[1]);

    if (fontNameVal.type != TYPE_STRING || fontSizeVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: InitTextSystem argument type mismatch.\n");
        freeValue(&fontNameVal); freeValue(&fontSizeVal);
        EXIT_FAILURE_HANDLER(); // Or return error
    }

    const char* font_path = fontNameVal.s_val;
    int font_size = (int)fontSizeVal.i_val;

    if (gSdlFont) { // Close previous font if one was already loaded
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
    }

    gSdlFont = TTF_OpenFont(font_path, font_size);
    if (!gSdlFont) {
        fprintf(stderr, "Runtime error: Failed to load font '%s': %s\n", font_path, TTF_GetError());
        // Don't TTF_Quit() here if other fonts might be attempted later,
        // but for a fatal error like this, EXIT_FAILURE_HANDLER might be appropriate.
        freeValue(&fontNameVal); freeValue(&fontSizeVal);
        EXIT_FAILURE_HANDLER(); // Or return error
    }
    gSdlFontSize = font_size;

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG InitTextSystem] Loaded font '%s' at size %d.\n", font_path, font_size);
    #endif

    freeValue(&fontNameVal);
    freeValue(&fontSizeVal);
    return makeVoid();
}

Value executeBuiltinQuitTextSystem(AST *node) {
    if (node->child_count != 0) { /* ... error ... */ }

    if (gSdlFont) { // If a font is currently loaded, close it
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG QuitTextSystem] Closed active SDL_ttf font.\n");
        #endif
    }

    if (gSdlTtfInitialized) { // Only quit TTF if it was initialized
        TTF_Quit();
        gSdlTtfInitialized = false; // Reset the flag
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG QuitTextSystem] SDL_ttf system quit.\n");
        #endif
    } else {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG QuitTextSystem] SDL_ttf system was not initialized, no need to quit.\n");
        #endif
    }
    return makeVoid();
}

Value executeBuiltinDrawLine(AST *node) {
    if (node->child_count != 4) {
        fprintf(stderr, "Runtime error: DrawLine expects 4 integer arguments (x1, y1, x2, y2).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before DrawLine.\n");
        return makeVoid(); // Or EXIT
    }

    Value x1_val = eval(node->children[0]);
    Value y1_val = eval(node->children[1]);
    Value x2_val = eval(node->children[2]);
    Value y2_val = eval(node->children[3]);

    if (x1_val.type != TYPE_INTEGER || y1_val.type != TYPE_INTEGER ||
        x2_val.type != TYPE_INTEGER || y2_val.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: DrawLine arguments must be integers.\n");
        freeValue(&x1_val); freeValue(&y1_val); freeValue(&x2_val); freeValue(&y2_val);
        EXIT_FAILURE_HANDLER();
    }

    int x1 = (int)x1_val.i_val;
    int y1 = (int)y1_val.i_val;
    int x2 = (int)x2_val.i_val;
    int y2 = (int)y2_val.i_val;

    freeValue(&x1_val); freeValue(&y1_val); freeValue(&x2_val); freeValue(&y2_val);

    if (SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_SetRenderDrawColor failed in DrawLine: %s\n", SDL_GetError());
    }
    if (SDL_RenderDrawLine(gSdlRenderer, x1, y1, x2, y2) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_RenderDrawLine failed: %s\n", SDL_GetError());
    }
    return makeVoid();
}

Value executeBuiltinFillRect(AST *node) {
    if (node->child_count != 4) {
        fprintf(stderr, "Runtime error: FillRect expects 4 integer arguments (x1, y1, x2, y2).\n");
        EXIT_FAILURE_HANDLER();
    }
     if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before FillRect.\n");
        return makeVoid();
    }

    Value x1_val = eval(node->children[0]);
    Value y1_val = eval(node->children[1]);
    Value x2_val = eval(node->children[2]);
    Value y2_val = eval(node->children[3]);

    if (x1_val.type != TYPE_INTEGER || y1_val.type != TYPE_INTEGER ||
        x2_val.type != TYPE_INTEGER || y2_val.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: FillRect arguments must be integers.\n");
        freeValue(&x1_val); freeValue(&y1_val); freeValue(&x2_val); freeValue(&y2_val);
        EXIT_FAILURE_HANDLER();
    }

    SDL_Rect rect;
    rect.x = (int)x1_val.i_val;
    rect.y = (int)y1_val.i_val;
    rect.w = (int)x2_val.i_val - rect.x +1; // Assuming x2,y2 is bottom-right inclusive
    rect.h = (int)y2_val.i_val - rect.y +1; // Width/Height calculation might need adjustment based on x1,y1,x2,y2 meaning (corner vs w/h)
                                        // If x1,y1 is top-left and x2,y2 is width,height:
                                        // rect.w = (int)x2_val.i_val; rect.h = (int)y2_val.i_val;
                                        // For now, assuming x1,y1 and x2,y2 are opposite corners.

    freeValue(&x1_val); freeValue(&y1_val); freeValue(&x2_val); freeValue(&y2_val);

    // Normalize rect if x1 > x2 or y1 > y2
    if (rect.w < 0) { rect.x += rect.w; rect.w = -rect.w; }
    if (rect.h < 0) { rect.y += rect.h; rect.h = -rect.h; }


    if (SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_SetRenderDrawColor failed in FillRect: %s\n", SDL_GetError());
    }
    if (SDL_RenderFillRect(gSdlRenderer, &rect) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_RenderFillRect failed: %s\n", SDL_GetError());
    }
    return makeVoid();
}

// Helper for DrawCircle (Midpoint Circle Algorithm)
void DrawCirclePoints(int centerX, int centerY, int x, int y) {
    SDL_RenderDrawPoint(gSdlRenderer, centerX + x, centerY + y);
    SDL_RenderDrawPoint(gSdlRenderer, centerX - x, centerY + y);
    SDL_RenderDrawPoint(gSdlRenderer, centerX + x, centerY - y);
    SDL_RenderDrawPoint(gSdlRenderer, centerX - x, centerY - y);
    SDL_RenderDrawPoint(gSdlRenderer, centerX + y, centerY + x);
    SDL_RenderDrawPoint(gSdlRenderer, centerX - y, centerY + x);
    SDL_RenderDrawPoint(gSdlRenderer, centerX + y, centerY - x);
    SDL_RenderDrawPoint(gSdlRenderer, centerX - y, centerY - x);
}

Value executeBuiltinDrawCircle(AST *node) {
    if (node->child_count != 3) {
        fprintf(stderr, "Runtime error: DrawCircle expects 3 integer arguments (CenterX, CenterY, Radius).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before DrawCircle.\n");
        return makeVoid();
    }

    Value cx_val = eval(node->children[0]);
    Value cy_val = eval(node->children[1]);
    Value r_val = eval(node->children[2]);

    if (cx_val.type != TYPE_INTEGER || cy_val.type != TYPE_INTEGER || r_val.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: DrawCircle arguments must be integers.\n");
        freeValue(&cx_val); freeValue(&cy_val); freeValue(&r_val);
        EXIT_FAILURE_HANDLER();
    }

    int centerX = (int)cx_val.i_val;
    int centerY = (int)cy_val.i_val;
    int radius = (int)r_val.i_val;

    freeValue(&cx_val); freeValue(&cy_val); freeValue(&r_val);

    if (radius < 0) return makeVoid(); // Nothing to draw for negative radius

    if (SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_SetRenderDrawColor failed in DrawCircle: %s\n", SDL_GetError());
    }

    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        DrawCirclePoints(centerX, centerY, x, y);
        if (err <= 0) {
            y += 1;
            err += 2*y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2*x + 1;
        }
    }
    return makeVoid();
}

Value executeBuiltinOutTextXY(AST *node) {
    if (node->child_count != 3) { /* ... error ... */ }
    if (!gSdlInitialized || !gSdlRenderer) { /* ... error ... */ }

    // >>> Check if TTF system and font are ready <<<
    if (!gSdlTtfInitialized) {
        fprintf(stderr, "Runtime error: Text system not initialized. Call InitTextSystem before OutTextXY.\n");
        return makeVoid(); // Or EXIT_FAILURE_HANDLER
    }
    if (!gSdlFont) {
        fprintf(stderr, "Runtime error: No font loaded. Call InitTextSystem with a valid font before OutTextXY.\n");
        return makeVoid(); // Or EXIT_FAILURE_HANDLER
    }

    // ... (rest of the existing OutTextXY logic: eval args, TTF_RenderUTF8_Solid, etc.) ...
    // ...
    Value x_val = eval(node->children[0]);
    Value y_val = eval(node->children[1]);
    Value text_val = eval(node->children[2]);

    if (x_val.type != TYPE_INTEGER || y_val.type != TYPE_INTEGER || text_val.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: OutTextXY argument type mismatch.\n");
        freeValue(&x_val); freeValue(&y_val); freeValue(&text_val);
        EXIT_FAILURE_HANDLER();
    }

    int x = (int)x_val.i_val;
    int y = (int)y_val.i_val;
    const char* text_to_render = text_val.s_val ? text_val.s_val : "";

    SDL_Surface* textSurface = TTF_RenderUTF8_Solid(gSdlFont, text_to_render, gSdlCurrentColor);
    if (!textSurface) {
        fprintf(stderr, "Runtime error: TTF_RenderUTF8_Solid failed in OutTextXY: %s\n", TTF_GetError());
        goto cleanup_outtextxy; // Use goto for consistent cleanup
    }

    SDL_Texture* textTexture = SDL_CreateTextureFromSurface(gSdlRenderer, textSurface);
    if (!textTexture) {
        fprintf(stderr, "Runtime error: SDL_CreateTextureFromSurface failed in OutTextXY: %s\n", SDL_GetError());
        SDL_FreeSurface(textSurface);
        goto cleanup_outtextxy;
    }

    SDL_Rect destRect = { x, y, textSurface->w, textSurface->h };
    // Ensure blending is enabled for the texture if you want transparent backgrounds for text
    // SDL_SetTextureBlendMode(textTexture, SDL_BLENDMODE_BLEND); // If not set globally for textures
    if(SDL_RenderCopy(gSdlRenderer, textTexture, NULL, &destRect) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_RenderCopy failed in OutTextXY: %s\n", SDL_GetError());
    }


    SDL_DestroyTexture(textTexture);
    SDL_FreeSurface(textSurface);

cleanup_outtextxy:
    freeValue(&x_val);
    freeValue(&y_val);
    freeValue(&text_val);
    return makeVoid();
}

Value executeBuiltinGetMouseState(AST *node) {
    if (node->child_count != 3) {
        fprintf(stderr, "Runtime error: GetMouseState expects 3 VAR arguments (X, Y: Integer; Buttons: Integer).\n");
        EXIT_FAILURE_HANDLER();
    }
     if (!gSdlInitialized) {
        fprintf(stderr, "Runtime error: SDL not initialized before GetMouseState.\n");
        // Optionally set VAR params to defaults (e.g., 0)
        return makeVoid();
    }

    AST* x_arg_node = node->children[0];
    AST* y_arg_node = node->children[1];
    AST* buttons_arg_node = node->children[2];

    // Ensure arguments are actual VAR parameters (parser should set by_ref)
    // This check is more conceptual here; actual assignment relies on assignValueToLValue
    // if (!x_arg_node->by_ref || !y_arg_node->by_ref || !buttons_arg_node->by_ref) {
    //    fprintf(stderr, "Runtime error: GetMouseState arguments must be VAR parameters.\n");
    //    EXIT_FAILURE_HANDLER();
    // }

    int mse_x, mse_y;
    Uint32 sdl_buttons_state = SDL_GetMouseState(&mse_x, &mse_y);

    // Map SDL button state to Pscal button state (bitmask)
    // This mapping depends on how you define Pscal mouse button constants
    int pscal_buttons = 0;
    if (sdl_buttons_state & SDL_BUTTON_LMASK) pscal_buttons |= 1;  // Assuming Pscal 1 for Left
    if (sdl_buttons_state & SDL_BUTTON_MMASK) pscal_buttons |= 2;  // Assuming Pscal 2 for Middle
    if (sdl_buttons_state & SDL_BUTTON_RMASK) pscal_buttons |= 4;  // Assuming Pscal 4 for Right
    // Add SDL_BUTTON_X1MASK and SDL_BUTTON_X2MASK if needed

    Value val_x = makeInt(mse_x);
    Value val_y = makeInt(mse_y);
    Value val_buttons = makeInt(pscal_buttons);

    assignValueToLValue(x_arg_node, val_x);
    assignValueToLValue(y_arg_node, val_y);
    assignValueToLValue(buttons_arg_node, val_buttons);

    freeValue(&val_x);
    freeValue(&val_y);
    freeValue(&val_buttons);

    return makeVoid();
}

// Helper to find a free texture slot or return an error ID
int findFreeTextureID(void) {
    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        if (gSdlTextures[i] == NULL) {
            return i;
        }
    }
    return -1; // No free slots
}

Value executeBuiltinCreateTexture(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: CreateTexture expects 2 arguments (Width, Height: Integer).\n");
        return makeInt(-1); // Return -1 for error
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before CreateTexture.\n");
        return makeInt(-1);
    }

    Value widthVal = eval(node->children[0]);
    Value heightVal = eval(node->children[1]);

    if (widthVal.type != TYPE_INTEGER || heightVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: CreateTexture arguments must be integers.\n");
        freeValue(&widthVal); freeValue(&heightVal);
        return makeInt(-1);
    }

    int width = (int)widthVal.i_val;
    int height = (int)heightVal.i_val;
    freeValue(&widthVal); freeValue(&heightVal);

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Runtime error: CreateTexture dimensions must be positive.\n");
        return makeInt(-1);
    }

    int textureID = findFreeTextureID();
    if (textureID == -1) {
        fprintf(stderr, "Runtime error: Maximum number of textures reached (%d).\n", MAX_SDL_TEXTURES);
        return makeInt(-1);
    }

    // Create a streaming texture - RGBA8888 is common and easy for byte arrays
    // SDL_PIXELFORMAT_ARGB8888 means byte order in memory is B,G,R,A for a uint32_t
    // Or use SDL_PIXELFORMAT_RGBA8888 for R,G,B,A order if your Pscal array matches that.
    // Let's assume Pscal array will be R,G,B,A order and use SDL_PIXELFORMAT_RGBA8888
    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer,
                                                SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                width, height);
    if (!newTexture) {
        fprintf(stderr, "Runtime error: SDL_CreateTexture failed: %s\n", SDL_GetError());
        return makeInt(-1);
    }
    // Optional: Set blend mode if you need transparency with RGBA
    // SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);
    
    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);

    gSdlTextures[textureID] = newTexture;
    gSdlTextureWidths[textureID] = width;
    gSdlTextureHeights[textureID] = height;

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG CreateTexture] Created Texture ID %d (%dx%d).\n", textureID, width, height);
    #endif
    return makeInt(textureID);
}

Value executeBuiltinDestroyTexture(AST *node) {
    if (node->child_count != 1) { /* error */ return makeVoid(); }
    Value idVal = eval(node->children[0]);
    if (idVal.type != TYPE_INTEGER) { /* error */ freeValue(&idVal); return makeVoid(); }
    int textureID = (int)idVal.i_val;
    freeValue(&idVal);

    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime warning: DestroyTexture called with invalid TextureID %d.\n", textureID);
        return makeVoid();
    }

    SDL_DestroyTexture(gSdlTextures[textureID]);
    gSdlTextures[textureID] = NULL;
    gSdlTextureWidths[textureID] = 0;
    gSdlTextureHeights[textureID] = 0;
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG DestroyTexture] Destroyed Texture ID %d.\n", textureID);
    #endif
    return makeVoid();
}

Value executeBuiltinUpdateTexture(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: UpdateTexture expects 2 arguments (TextureID: Integer; PixelData: ARRAY OF Byte).\n");
        return makeVoid();
    }

    Value idVal = eval(node->children[0]);
    Value pixelDataVal = eval(node->children[1]); // This evaluates the Pscal array variable

    if (idVal.type != TYPE_INTEGER || pixelDataVal.type != TYPE_ARRAY) {
        fprintf(stderr, "Runtime error: UpdateTexture argument type mismatch.\n");
        goto cleanup_update;
    }
    if (pixelDataVal.element_type != TYPE_BYTE) {
         fprintf(stderr, "Runtime error: UpdateTexture PixelData must be an ARRAY OF Byte. Got array of %s.\n", varTypeToString(pixelDataVal.element_type));
        goto cleanup_update;
    }


    int textureID = (int)idVal.i_val;
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: UpdateTexture called with invalid TextureID %d.\n", textureID);
        goto cleanup_update;
    }

    int texWidth = gSdlTextureWidths[textureID];
    int texHeight = gSdlTextureHeights[textureID];
    int bytesPerPixel = 4; // Assuming RGBA8888
    int expectedPscalArraySize = texWidth * texHeight * bytesPerPixel;
    int pitch = texWidth * bytesPerPixel;

    // Calculate actual size of the Pscal array (Value struct for arrays has flat data)
    int pscalArrayTotalElements = 1;
    for(int i=0; i < pixelDataVal.dimensions; ++i) {
        pscalArrayTotalElements *= (pixelDataVal.upper_bounds[i] - pixelDataVal.lower_bounds[i] + 1);
    }

    if (pscalArrayTotalElements != expectedPscalArraySize) {
        fprintf(stderr, "Runtime error: UpdateTexture PixelData array size (%d) does not match texture dimensions*BPP (%dx%dx%d = %d).\n",
                pscalArrayTotalElements, texWidth, texHeight, bytesPerPixel, expectedPscalArraySize);
        goto cleanup_update;
    }

    // Create a temporary C buffer and copy data from Pscal's Value array
    unsigned char* c_pixel_buffer = (unsigned char*)malloc(expectedPscalArraySize);
    if (!c_pixel_buffer) {
        fprintf(stderr, "Runtime error: Failed to allocate C buffer for UpdateTexture.\n");
        goto cleanup_update;
    }

    for (int i = 0; i < expectedPscalArraySize; ++i) {
        // Pscal array elements are Value structs of TYPE_BYTE
        if (pixelDataVal.array_val[i].type != TYPE_BYTE) {
            fprintf(stderr, "Runtime error: UpdateTexture PixelData array element %d is not TYPE_BYTE.\n", i);
            free(c_pixel_buffer);
            goto cleanup_update;
        }
        c_pixel_buffer[i] = (unsigned char)pixelDataVal.array_val[i].i_val;
    }

    if (SDL_UpdateTexture(gSdlTextures[textureID], NULL, c_pixel_buffer, pitch) != 0) {
        fprintf(stderr, "Runtime error: SDL_UpdateTexture failed: %s\n", SDL_GetError());
    }

    free(c_pixel_buffer);

cleanup_update:
    freeValue(&idVal);
    freeValue(&pixelDataVal); // This will free the Pscal array Value structure and its contents
    return makeVoid();
}

Value executeBuiltinRenderCopy(AST *node) {
    if (node->child_count != 1) { /* error */ return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { /* error */ return makeVoid(); }

    Value idVal = eval(node->children[0]);
    if (idVal.type != TYPE_INTEGER) { /* error */ freeValue(&idVal); return makeVoid(); }
    int textureID = (int)idVal.i_val;
    freeValue(&idVal);

    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: RenderCopy called with invalid TextureID %d.\n", textureID);
        return makeVoid();
    }

    if (SDL_RenderCopy(gSdlRenderer, gSdlTextures[textureID], NULL, NULL) != 0) { // NULL src/dst rect = copy whole texture to whole renderer
        fprintf(stderr, "Runtime Warning: SDL_RenderCopy failed: %s\n", SDL_GetError());
    }
    return makeVoid();
}

Value executeBuiltinRenderCopyRect(AST *node) {
    if (node->child_count != 5) { /* error: ID, dx,dy,dw,dh */ return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { /* error */ return makeVoid(); }

    Value idVal = eval(node->children[0]);
    Value dxVal = eval(node->children[1]);
    Value dyVal = eval(node->children[2]);
    Value dwVal = eval(node->children[3]);
    Value dhVal = eval(node->children[4]);

    // ... (type checks for all being integer) ...
    // ... (freeValue for all) ...

    int textureID = (int)idVal.i_val;
    // ... (check textureID validity) ...

    SDL_Rect dstRect;
    dstRect.x = (int)dxVal.i_val;
    dstRect.y = (int)dyVal.i_val;
    dstRect.w = (int)dwVal.i_val;
    dstRect.h = (int)dhVal.i_val;

    if (SDL_RenderCopy(gSdlRenderer, gSdlTextures[textureID], NULL, &dstRect) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_RenderCopy (rect) failed: %s\n", SDL_GetError());
    }
    return makeVoid();
}

// Helper to draw horizontal line efficiently (used by FillCircle)
void DrawHorizontalLine(int x1, int x2, int y) {
    // SDL_RenderDrawLine can be used, ensure x1 <= x2 if needed by implementation
    if (SDL_RenderDrawLine(gSdlRenderer, x1, y, x2, y) != 0) {
         fprintf(stderr, "Runtime Warning: SDL_RenderDrawLine failed in DrawHorizontalLine: %s\n", SDL_GetError());
    }
}

Value executeBuiltinFillCircle(AST *node) {
    if (node->child_count != 3) {
        fprintf(stderr, "Runtime error: FillCircle expects 3 integer arguments (CenterX, CenterY, Radius).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before FillCircle.\n");
        return makeVoid(); // Or EXIT
    }

    Value cx_val = eval(node->children[0]);
    Value cy_val = eval(node->children[1]);
    Value r_val = eval(node->children[2]);

    if (cx_val.type != TYPE_INTEGER || cy_val.type != TYPE_INTEGER || r_val.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: FillCircle arguments must be integers.\n");
        freeValue(&cx_val); freeValue(&cy_val); freeValue(&r_val);
        EXIT_FAILURE_HANDLER();
    }

    int centerX = (int)cx_val.i_val;
    int centerY = (int)cy_val.i_val;
    int radius = (int)r_val.i_val;

    freeValue(&cx_val); freeValue(&cy_val); freeValue(&r_val);

    if (radius < 0) return makeVoid(); // Cannot draw negative radius

    if (SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_SetRenderDrawColor failed in FillCircle: %s\n", SDL_GetError());
        // Continue attempt anyway
    }

    // Simple Filling method: Draw horizontal lines based on circle equation
    // More efficient methods exist (like adapting Midpoint), but this is clearer
    for (int dy = -radius; dy <= radius; ++dy) {
        // Calculate horizontal span (dx) for this dy using circle equation: x^2 + y^2 = r^2
        // dx = sqrt(r^2 - dy^2)
        int dx = (int)round(sqrt((double)radius * radius - (double)dy * dy));
        int y = centerY + dy;
        int x1 = centerX - dx;
        int x2 = centerX + dx;
        DrawHorizontalLine(x1, x2, y);
    }
    // Note: SDL_RenderDrawLine includes both endpoints.

    return makeVoid();
}

Value executeBuiltinQuitRequested(AST *node) {
    // This function expects no arguments. Check the argument count.
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: QuitRequested expects 0 arguments.\n");
        // Consider adding error handling or exit here if arguments are found unexpectedly
        EXIT_FAILURE_HANDLER(); // Exit on invalid built-in usage
    }
    // Return a Pascal BOOLEAN value based on the C global break_requested flag
    // In C, 0 is false, non-zero is true.
    return makeBoolean(break_requested != 0);
}

// --- Implementation for SdlCleanupAtExit ---
// This function is called at the end of the program to clean up SDL,
// SDL_mixer, and SDL_ttf resources.
// It cleans up global SDL/Audio/TTF state variables (gSdlWindow, gSdlRenderer, etc.)
// and the underlying library resources.
void SdlCleanupAtExit(void) {
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SDL] Running SdlCleanupAtExit (Final Program Exit Cleanup)...\n");
    #endif

    // --- Clean up SDL_ttf resources ---
    if (gSdlFont) {
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: TTF_CloseFont successful.\n");
        #endif
    }
    if (gSdlTtfInitialized) {
        TTF_Quit();
        gSdlTtfInitialized = false;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: TTF_Quit successful.\n");
        #endif
    }

    // --- Clean up SDL_mixer audio resources ---
    // Halt any currently playing sound effects and music (good practice before cleanup)
    // Only do this if the sound system was initialized by pscal at some point.
    // However, SDL_Init(SDL_INIT_AUDIO) might have been called by SDL_Init(SDL_INIT_EVERYTHING)
    // even if Audio_InitSystem wasn't. For safety, check if SDL_mixer was actually used.
    // A simple check: if gSoundSystemInitialized was ever true, or if SDL_WasInit(SDL_INIT_AUDIO) is true.

    // If Audio_QuitSystem might have already freed sounds, this loop is safe as it checks for NULL.
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (gLoadedSounds[i] != NULL) {
            Mix_FreeChunk(gLoadedSounds[i]);
            gLoadedSounds[i] = NULL;
            DEBUG_PRINT("[DEBUG AUDIO] SdlCleanupAtExit: Auto-freed sound chunk at index %d.\n", i);
        }
    }

    // Close the audio device if it's still considered open by SDL_mixer
    // Mix_QuerySpec returns 0 if not open, non-zero if open.
    int open_freq, open_channels;
    Uint16 open_format;
    if (Mix_QuerySpec(&open_freq, &open_format, &open_channels) != 0) {
        Mix_CloseAudio();
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG AUDIO] SdlCleanupAtExit: Mix_CloseAudio successful.\n");
        #endif
    } else {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG AUDIO] SdlCleanupAtExit: Mix_CloseAudio skipped (audio not open or already closed by Audio_QuitSystem).\n");
        #endif
    }

    // Quit SDL_mixer subsystems. This should be safe to call even if already called,
    // but ideally, it's called only once.
    // If Audio_QuitSystem is changed to NOT call Mix_Quit(), this is the sole place.
    Mix_Quit(); // This cleans up all initialized formats (OGG, MP3, etc.)
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG AUDIO] SdlCleanupAtExit: Mix_Quit successful.\n");
    #endif
    // Reset our flag if Audio_QuitSystem didn't
    gSoundSystemInitialized = false;


    // --- Clean up core SDL video and timer resources ---
    if (gSdlRenderer) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: SDL_DestroyRenderer successful.\n");
        #endif
    }
    if (gSdlWindow) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: SDL_DestroyWindow successful.\n");
        #endif
    }
    if (gSdlInitialized) { // This flag tracks if SDL_Init() for video/timer was called
        SDL_Quit(); // This quits all initialized SDL subsystems (including SDL_INIT_AUDIO if it was ever inited)
        gSdlInitialized = false;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: SDL_Quit successful.\n");
        #endif
    }

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit finished.\n");
    #endif
}

Value executeBuiltinRenderCopyEx(AST *node) {
    // Expected arguments:
    // 0: TextureID (Integer)
    // 1: SrcX (Integer)
    // 2: SrcY (Integer)
    // 3: SrcW (Integer)
    // 4: SrcH (Integer)
    // 5: DstX (Integer)
    // 6: DstY (Integer)
    // 7: DstW (Integer)
    // 8: DstH (Integer)
    // 9: Angle (Real)
    // 10: RotationPointX (Integer, relative to DstW/H, or special value for center)
    // 11: RotationPointY (Integer, relative to DstW/H, or special value for center)
    // 12: FlipMode (Integer)

    if (node->child_count != 13) {
        fprintf(stderr, "Runtime error: RenderCopyEx expects 13 arguments.\n");
        EXIT_FAILURE_HANDLER(); // Or return makeVoid() after freeing any evaluated args
    }

    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before RenderCopyEx.\n");
        return makeVoid();
    }

    // Evaluate all arguments
    Value texID_val = eval(node->children[0]);
    Value srcX_val  = eval(node->children[1]);
    Value srcY_val  = eval(node->children[2]);
    Value srcW_val  = eval(node->children[3]);
    Value srcH_val  = eval(node->children[4]);
    Value dstX_val  = eval(node->children[5]);
    Value dstY_val  = eval(node->children[6]);
    Value dstW_val  = eval(node->children[7]);
    Value dstH_val  = eval(node->children[8]);
    Value angle_val = eval(node->children[9]);
    Value rotX_val  = eval(node->children[10]);
    Value rotY_val  = eval(node->children[11]);
    Value flip_val  = eval(node->children[12]);

    // Type checking
    if (texID_val.type != TYPE_INTEGER ||
        srcX_val.type  != TYPE_INTEGER || srcY_val.type  != TYPE_INTEGER ||
        srcW_val.type  != TYPE_INTEGER || srcH_val.type  != TYPE_INTEGER ||
        dstX_val.type  != TYPE_INTEGER || dstY_val.type  != TYPE_INTEGER ||
        dstW_val.type  != TYPE_INTEGER || dstH_val.type  != TYPE_INTEGER ||
        angle_val.type != TYPE_REAL    || // Angle is Real
        rotX_val.type  != TYPE_INTEGER || rotY_val.type  != TYPE_INTEGER ||
        flip_val.type  != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: RenderCopyEx argument type mismatch.\n");
        // Free all evaluated values before exiting
        freeValue(&texID_val); freeValue(&srcX_val); freeValue(&srcY_val);
        freeValue(&srcW_val); freeValue(&srcH_val); freeValue(&dstX_val);
        freeValue(&dstY_val); freeValue(&dstW_val); freeValue(&dstH_val);
        freeValue(&angle_val); freeValue(&rotX_val); freeValue(&rotY_val);
        freeValue(&flip_val);
        EXIT_FAILURE_HANDLER();
    }

    int textureID = (int)texID_val.i_val;
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: RenderCopyEx called with invalid or unloaded TextureID %d.\n", textureID);
        // Free remaining values
        // (Simplified cleanup for brevity, a goto or more structured cleanup is better for many args)
        goto cleanup_rendercopyex;
    }
    SDL_Texture* texture = gSdlTextures[textureID];

    SDL_Rect srcRect;
    SDL_Rect* srcRectPtr = NULL;
    srcRect.x = (int)srcX_val.i_val;
    srcRect.y = (int)srcY_val.i_val;
    srcRect.w = (int)srcW_val.i_val;
    srcRect.h = (int)srcH_val.i_val;
    if (srcRect.w > 0 && srcRect.h > 0) {
        srcRectPtr = &srcRect;
    }

    SDL_Rect dstRect;
    dstRect.x = (int)dstX_val.i_val;
    dstRect.y = (int)dstY_val.i_val;
    dstRect.w = (int)dstW_val.i_val;
    dstRect.h = (int)dstH_val.i_val;

    double angle_degrees = angle_val.r_val;

    SDL_Point rotationCenter;
    SDL_Point* centerPtr = NULL;
    int pscalRotX = (int)rotX_val.i_val;
    int pscalRotY = (int)rotY_val.i_val;

    // If Pscal user passes a conventional negative value (e.g., -1) for center,
    // SDL_RenderCopyEx uses the dstRect's center when centerPtr is NULL.
    // Otherwise, use the provided relative coordinates.
    if (pscalRotX >= 0 && pscalRotY >= 0) {
        rotationCenter.x = pscalRotX;
        rotationCenter.y = pscalRotY;
        centerPtr = &rotationCenter;
    } else {
        centerPtr = NULL; // Use center of dstRect for rotation
    }


    SDL_RendererFlip sdl_flip = SDL_FLIP_NONE;
    int flipMode = (int)flip_val.i_val;
    if (flipMode == 1) sdl_flip = SDL_FLIP_HORIZONTAL;
    else if (flipMode == 2) sdl_flip = SDL_FLIP_VERTICAL;
    else if (flipMode == 3) sdl_flip = (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);


    if (SDL_RenderCopyEx(gSdlRenderer, texture, srcRectPtr, &dstRect, angle_degrees, centerPtr, sdl_flip) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_RenderCopyEx failed: %s\n", SDL_GetError());
    }

cleanup_rendercopyex:
    freeValue(&texID_val); freeValue(&srcX_val); freeValue(&srcY_val);
    freeValue(&srcW_val); freeValue(&srcH_val); freeValue(&dstX_val);
    freeValue(&dstY_val); freeValue(&dstW_val); freeValue(&dstH_val);
    freeValue(&angle_val); freeValue(&rotX_val); freeValue(&rotY_val);
    freeValue(&flip_val);

    return makeVoid();
}

