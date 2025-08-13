//
//  sdl.c
//  Pscal
//
//  Created by Michael Miller on 5/7/25.
//
#ifdef SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
// Include SDL_mixer header directly
#include <SDL2/SDL_mixer.h>
// Include audio.h directly (declares MAX_SOUNDS and gLoadedSounds)
#include "audio.h"

#include "core/utils.h"
#include "vm/vm.h"

#include "sdl.h" // This header includes SDL/SDL_ttf headers
#include "globals.h" // Includes SDL.h and SDL_ttf.h via its includes, and audio.h
#include "types.h"
#include "ast.h"
#include "interpreter.h"
#include "utils.h"
#include "builtin.h"

// SDL Global Variable Definitions
SDL_Window* gSdlWindow = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_Color gSdlCurrentColor = { 255, 255, 255, 255 }; // Default white
bool gSdlInitialized = false;
int gSdlWidth = 0;
int gSdlHeight = 0;
TTF_Font* gSdlFont = NULL;
int gSdlFontSize   = 16;
SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES];
int gSdlTextureWidths[MAX_SDL_TEXTURES];
int gSdlTextureHeights[MAX_SDL_TEXTURES];
bool gSdlTtfInitialized = false;
bool gSdlImageInitialized = false; // Tracks if IMG_Init was called for PNG/JPG etc.

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
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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

// Pscal: Function CreateTargetTexture(Width, Height: Integer): Integer;
Value executeBuiltinCreateTargetTexture(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: CreateTargetTexture expects 2 arguments (Width, Height: Integer).\n");
        return makeInt(-1); // Error code
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics system not initialized before CreateTargetTexture.\n");
        return makeInt(-1);
    }

    Value widthVal = eval(node->children[0]);
    Value heightVal = eval(node->children[1]);

    if (widthVal.type != TYPE_INTEGER || heightVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: CreateTargetTexture arguments must be integers.\n");
        freeValue(&widthVal); freeValue(&heightVal);
        return makeInt(-1);
    }

    int width = (int)widthVal.i_val;
    int height = (int)heightVal.i_val;
    freeValue(&widthVal); freeValue(&heightVal);

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Runtime error: CreateTargetTexture dimensions must be positive.\n");
        return makeInt(-1);
    }

    int textureID = findFreeTextureID(); // Assuming this helper exists and returns 0-based ID
    if (textureID == -1) {
        fprintf(stderr, "Runtime error: Maximum number of textures reached (%d).\n", MAX_SDL_TEXTURES);
        return makeInt(-1);
    }

    // Create a texture that can be used as a render target
    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer,
                                                SDL_PIXELFORMAT_RGBA8888, // A common format
                                                SDL_TEXTUREACCESS_TARGET, // <<< KEY DIFFERENCE
                                                width, height);
    if (!newTexture) {
        fprintf(stderr, "Runtime error: SDL_CreateTexture (for target) failed: %s\n", SDL_GetError());
        return makeInt(-1);
    }
    
#ifdef DEBUG
Uint32 temp_format;
int temp_access, temp_w, temp_h;
if (SDL_QueryTexture(newTexture, &temp_format, &temp_access, &temp_w, &temp_h) == 0) {
    fprintf(stderr, "[DEBUG CreateTargetTexture] IMMEDIATELY after creation, TextureID %d, created newTexture %p has access flags: %d (Target should be 2)\n",
            textureID, (void*)newTexture, temp_access);
} else {
    fprintf(stderr, "[DEBUG CreateTargetTexture] IMMEDIATELY after creation, TextureID %d, FAILED to query newTexture %p: %s\n",
            textureID, (void*)newTexture, SDL_GetError());
}
#endif
    
    // Optional: Set blend mode if you need transparency when rendering this texture later
    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);


    gSdlTextures[textureID] = newTexture;
    gSdlTextureWidths[textureID] = width;
    gSdlTextureHeights[textureID] = height;

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SDL] Created Target Texture ID %d (%dx%d).\n", textureID, width, height);
    #endif
    return makeInt(textureID); // Return 0-based TextureID
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
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before RenderCopy.\n");
        return makeVoid();
    }

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
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before RenderCopyRect.\n");
        return makeVoid();
    }

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
        //EXIT_FAILURE_HANDLER(); // Or return makeVoid() after freeing any evaluated args
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
    // Old RotationCenterY position (index 10) is now RotationCenterY
    Value rotY_val  = eval(node->children[11]);
    // Old FlipMode position (index 11) is now FlipMode
    Value flip_val  = eval(node->children[12]);

#ifdef DEBUG
fprintf(stderr, "[DEBUG RenderCopyEx] Evaluated arg types before check:\n");
fprintf(stderr, "  texID: %s\n", varTypeToString(texID_val.type));
fprintf(stderr, "  srcX: %s, srcY: %s, srcW: %s, srcH: %s\n", varTypeToString(srcX_val.type), varTypeToString(srcY_val.type), varTypeToString(srcW_val.type), varTypeToString(srcH_val.type));
fprintf(stderr, "  dstX: %s, dstY: %s, dstW: %s, dstH: %s\n", varTypeToString(dstX_val.type), varTypeToString(dstY_val.type), varTypeToString(dstW_val.type), varTypeToString(dstH_val.type));
fprintf(stderr, "  ANGLE: %s (Value: %f if real)\n", varTypeToString(angle_val.type), angle_val.type == TYPE_REAL ? angle_val.r_val : 0.0); // Print angle value
fprintf(stderr, "  rotX: %s, rotY: %s\n", varTypeToString(rotX_val.type), varTypeToString(rotY_val.type));
fprintf(stderr, "  flip: %s\n", varTypeToString(flip_val.type));
fflush(stderr);
#endif

    // Type checking
    if (texID_val.type != TYPE_INTEGER || /* ... up to dstH_val ... */
         rotX_val.type  != TYPE_INTEGER || // New child[9]
         rotY_val.type  != TYPE_INTEGER || // New child[10]
         flip_val.type  != TYPE_INTEGER || // New child[11]
         angle_val.type != TYPE_REAL) {    // New child[12]
         fprintf(stderr, "Runtime error: RenderCopyEx argument type mismatch (Angle Last Test). Angle expected REAL, got %s. RotX got %s, RotY got %s, Flip got %s\n",
             varTypeToString(angle_val.type), varTypeToString(rotX_val.type), varTypeToString(rotY_val.type), varTypeToString(flip_val.type) );
       
        // Free all evaluated values before exiting
      //  freeValue(&texID_val); freeValue(&srcX_val); freeValue(&srcY_val);
      //  freeValue(&srcW_val); freeValue(&srcH_val); freeValue(&dstX_val);
      //  freeValue(&dstY_val); freeValue(&dstW_val); freeValue(&dstH_val);
      //  freeValue(&angle_val); freeValue(&rotX_val); freeValue(&rotY_val);
      //  freeValue(&flip_val);
      //  EXIT_FAILURE_HANDLER();
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

// --- 1. LoadImageToTexture ---
// Pscal: Function LoadImageToTexture(FilePath: String): Integer;
Value executeBuiltinLoadImageToTexture(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: LoadImageToTexture expects 1 argument (FilePath: String).\n");
        return makeInt(-1); // Error code
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics system not initialized before LoadImageToTexture.\n");
        return makeInt(-1);
    }

    // Initialize SDL_image if not already done (for PNG, JPG support)
    if (!gSdlImageInitialized) {
        int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) {
            fprintf(stderr, "Runtime error: SDL_image initialization failed: %s\n", IMG_GetError());
            // No texture loaded, so just return error. SDL_image doesn't need explicit IMG_Quit for basic IMG_LoadTexture.
            // IMG_Quit() is called in SdlCleanupAtExit.
            return makeInt(-1);
        }
        gSdlImageInitialized = true;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SDL_image initialized (PNG, JPG).\n");
        #endif
    }

    Value filePathVal = eval(node->children[0]);
    if (filePathVal.type != TYPE_STRING || !filePathVal.s_val) {
        fprintf(stderr, "Runtime error: LoadImageToTexture argument must be a valid string.\n");
        freeValue(&filePathVal);
        return makeInt(-1);
    }

    const char* filePath = filePathVal.s_val;
    int free_slot = findFreeTextureID();

    if (free_slot == -1) {
        fprintf(stderr, "Runtime error: No free texture slots available for LoadImageToTexture.\n");
        freeValue(&filePathVal);
        return makeInt(-1);
    }

    SDL_Texture* newTexture = IMG_LoadTexture(gSdlRenderer, filePath);
    if (!newTexture) {
        fprintf(stderr, "Runtime error: Failed to load image '%s' as texture: %s\n", filePath, IMG_GetError());
        freeValue(&filePathVal);
        return makeInt(-1);
    }

    // Store texture and its dimensions
    gSdlTextures[free_slot] = newTexture;
    SDL_QueryTexture(newTexture, NULL, NULL, &gSdlTextureWidths[free_slot], &gSdlTextureHeights[free_slot]);
    
    // SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND); // Enable alpha blending by default for loaded images

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SDL] Loaded image '%s' to TextureID %d (%dx%d).\n",
            filePath, free_slot, gSdlTextureWidths[free_slot], gSdlTextureHeights[free_slot]);
    #endif

    freeValue(&filePathVal);
    return makeInt(free_slot); // Return 0-based TextureID
}

// --- 2. GetTextSize ---
// Pscal: Procedure GetTextSize(Text: String; var Width, Height: Integer);
Value executeBuiltinGetTextSize(AST *node) {
    if (node->child_count != 3) { // Text, VAR Width, VAR Height
        fprintf(stderr, "Runtime error: GetTextSize expects 3 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlTtfInitialized || !gSdlFont) {
        fprintf(stderr, "Runtime error: Text system or font not initialized before GetTextSize.\n");
        // Optionally set width/height to 0 or error values for the VAR params
        // For now, let's be strict and exit.
        EXIT_FAILURE_HANDLER();
    }

    Value textVal = eval(node->children[0]);
    // VAR params are nodes themselves, not evaluated here directly.
    AST* widthLvalNode = node->children[1];
    AST* heightLvalNode = node->children[2];

    if (textVal.type != TYPE_STRING || !textVal.s_val) {
        fprintf(stderr, "Runtime error: GetTextSize first argument must be a string.\n");
        freeValue(&textVal);
        EXIT_FAILURE_HANDLER();
    }
    // Ensure VAR params are indeed LValues (variables)
    if (widthLvalNode->type != AST_VARIABLE || heightLvalNode->type != AST_VARIABLE) {
        fprintf(stderr, "Runtime error: GetTextSize width/height parameters must be variables.\n");
        freeValue(&textVal);
        EXIT_FAILURE_HANDLER();
    }


    int w = 0, h = 0;
    if (TTF_SizeUTF8(gSdlFont, textVal.s_val, &w, &h) != 0) {
        fprintf(stderr, "Runtime warning: TTF_SizeUTF8 failed in GetTextSize: %s\n", TTF_GetError());
        // Set w,h to 0 or error indicators if preferred.
        w = 0; h = 0;
    }

    freeValue(&textVal); // Free the evaluated string

    // Assign results back to Pscal VAR parameters
    updateSymbol(widthLvalNode->token->value, makeInt(w));
    updateSymbol(heightLvalNode->token->value, makeInt(h));

    return makeVoid();
}

// --- 3. SetRenderTarget ---
// Pscal: Procedure SetRenderTarget(TextureID: Integer); (-1 or PscalNilId for screen)
Value executeBuiltinSetRenderTarget(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: SetRenderTarget expects 1 argument (TextureID: Integer).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics system not initialized before SetRenderTarget.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value texIDVal = eval(node->children[0]);
    if (texIDVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: SetRenderTarget argument must be an integer TextureID.\n");
        freeValue(&texIDVal);
        EXIT_FAILURE_HANDLER();
    }
    int textureID = (int)texIDVal.i_val;
    freeValue(&texIDVal);

    SDL_Texture* targetTexture = NULL;
    if (textureID >= 0 && textureID < MAX_SDL_TEXTURES && gSdlTextures[textureID] != NULL) {
        Uint32 format;
        int access; // Will be filled by SDL_QueryTexture
        int w, h;
        if (SDL_QueryTexture(gSdlTextures[textureID], &format, &access, &w, &h) == 0) {
            if (access == SDL_TEXTUREACCESS_TARGET) {
                targetTexture = gSdlTextures[textureID];
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG SDL] TextureID %d confirmed as SDL_TEXTUREACCESS_TARGET.\n", textureID);
                #endif
            } else {
                fprintf(stderr, "Runtime warning: TextureID %d was not created with Target access. Actual access flags: %d (Expected: %d for TARGET). Cannot set as render target.\n",
                        textureID, access, SDL_TEXTUREACCESS_TARGET);
                // targetTexture remains NULL, will default to screen
            }
        } else {
             fprintf(stderr, "Runtime warning: Could not query texture %d for SetRenderTarget: %s\n", textureID, SDL_GetError());
             // targetTexture remains NULL
        }
    }else if (textureID >= MAX_SDL_TEXTURES || (textureID >=0 && gSdlTextures[textureID] == NULL) ) {
        fprintf(stderr, "Runtime warning: Invalid TextureID %d passed to SetRenderTarget. Defaulting to screen.\n", textureID);
        // targetTexture remains NULL, which means render to the default window
    }
    // If textureID is < 0 (e.g., -1 convention for screen), targetTexture remains NULL.

    if (SDL_SetRenderTarget(gSdlRenderer, targetTexture) != 0) {
        fprintf(stderr, "Runtime error: SDL_SetRenderTarget failed: %s\n", SDL_GetError());
        // Potentially EXIT_FAILURE_HANDLER();
    }
    #ifdef DEBUG
    if (targetTexture) {
        fprintf(stderr, "[DEBUG SDL] Render target set to TextureID %d.\n", textureID);
    } else {
        fprintf(stderr, "[DEBUG SDL] Render target set to screen/default window.\n");
    }
    #endif

    return makeVoid();
}


// --- 4. DrawPolygon ---
// Pscal: Procedure DrawPolygon(Points: ARRAY OF PointRecord; NumPoints: Integer);
Value executeBuiltinDrawPolygon(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: DrawPolygon expects 2 arguments (PointsArray, NumPoints).\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized for DrawPolygon.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value pointsArrayVal = eval(node->children[0]); // This is the Pscal ARRAY OF PointRecord
    Value numPointsVal = eval(node->children[1]);

    if (pointsArrayVal.type != TYPE_ARRAY || numPointsVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: DrawPolygon argument type mismatch.\n");
        goto cleanup_drawpoly;
    }
    // Further check: pointsArrayVal.element_type should be TYPE_RECORD,
    // and that record should match your PointRecord structure. This is hard to check deeply here
    // without more metadata. Assume Pscal type checking handled it somewhat.
    if (pointsArrayVal.element_type != TYPE_RECORD) {
        fprintf(stderr, "Runtime error: DrawPolygon Points argument must be an ARRAY OF PointRecord.\n");
        goto cleanup_drawpoly;
    }


    int numPoints = (int)numPointsVal.i_val;
    if (numPoints < 2) { // Need at least 2 points for a line, 3 for a polygon "area"
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] DrawPolygon called with %d points, less than 2. Nothing to draw.\n", numPoints);
        #endif
        goto cleanup_drawpoly;
    }

    // Calculate total elements in the flat Pscal array
    int total_elements_in_pascal_array = 1;
    for(int i=0; i < pointsArrayVal.dimensions; ++i) { // Assuming 1D array of records
        total_elements_in_pascal_array *= (pointsArrayVal.upper_bounds[i] - pointsArrayVal.lower_bounds[i] + 1);
    }
    if (numPoints > total_elements_in_pascal_array) {
        fprintf(stderr, "Runtime error: NumPoints (%d) exceeds actual size of Pscal PointsArray (%d).\n", numPoints, total_elements_in_pascal_array);
        goto cleanup_drawpoly;
    }


    SDL_Point* sdlPoints = malloc(sizeof(SDL_Point) * (numPoints + 1)); // +1 if we need to close it manually by adding first point
    if (!sdlPoints) {
        fprintf(stderr, "Memory allocation failed for SDL_Point array in DrawPolygon.\n");
        goto cleanup_drawpoly;
    }

    for (int i = 0; i < numPoints; i++) {
        Value recordValue = pointsArrayVal.array_val[i]; // This is a Value of TYPE_RECORD
        if (recordValue.type != TYPE_RECORD || !recordValue.record_val) {
            fprintf(stderr, "Runtime error: Element %d in PointsArray is not a valid PointRecord.\n", i);
            free(sdlPoints);
            goto cleanup_drawpoly;
        }
        FieldValue* fieldX = recordValue.record_val;
        FieldValue* fieldY = fieldX ? fieldX->next : NULL;

        if (fieldX && fieldX->name && strcasecmp(fieldX->name, "x") == 0 && fieldX->value.type == TYPE_INTEGER &&
            fieldY && fieldY->name && strcasecmp(fieldY->name, "y") == 0 && fieldY->value.type == TYPE_INTEGER) {
            sdlPoints[i].x = (int)fieldX->value.i_val;
            sdlPoints[i].y = (int)fieldY->value.i_val;
        } else {
            fprintf(stderr, "Runtime error: PointRecord at index %d in PointsArray does not have correct X,Y integer fields.\n", i);
            free(sdlPoints);
            goto cleanup_drawpoly;
        }
    }

    // SDL_RenderDrawLines draws lines between (points[0], points[1]), (points[1], points[2]), etc.
    // To close the polygon, we add the first point to the end of the list of points for SDL_RenderDrawLines.
    if (numPoints > 1) { // Only makes sense if there's more than one point
        sdlPoints[numPoints] = sdlPoints[0]; // Close the polygon
    }

    if (SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a) != 0) {
         fprintf(stderr, "Runtime Warning: SetRenderDrawColor failed in DrawPolygon: %s\n", SDL_GetError());
    }

    // If numPoints is 2, it draws a line. If numPoints > 2, it draws connected lines forming the polygon outline.
    // We pass numPoints + 1 because we added the closing point.
    if (SDL_RenderDrawLines(gSdlRenderer, sdlPoints, numPoints > 1 ? numPoints + 1 : numPoints) != 0) {
        fprintf(stderr, "Runtime Warning: SDL_RenderDrawLines failed in DrawPolygon: %s\n", SDL_GetError());
    }

    free(sdlPoints);

cleanup_drawpoly:
    freeValue(&pointsArrayVal);
    freeValue(&numPointsVal);
    return makeVoid();
}


// --- 5. GetPixelColor ---
// Pscal: Procedure GetPixelColor(X, Y: Integer; var R, G, B, A: Byte);
Value executeBuiltinGetPixelColor(AST *node) {
    if (node->child_count != 6) { // X, Y, VAR R, VAR G, VAR B, VAR A
        fprintf(stderr, "Runtime error: GetPixelColor expects 6 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized for GetPixelColor.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value xVal = eval(node->children[0]);
    Value yVal = eval(node->children[1]);
    // VAR params are nodes
    AST* rLvalNode = node->children[2];
    AST* gLvalNode = node->children[3];
    AST* bLvalNode = node->children[4];
    AST* aLvalNode = node->children[5];

    if (xVal.type != TYPE_INTEGER || yVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: GetPixelColor X,Y coordinates must be integers.\n");
        goto cleanup_getpixel;
    }
    // Check LValue nodes are variables
    if (rLvalNode->type != AST_VARIABLE || gLvalNode->type != AST_VARIABLE ||
        bLvalNode->type != AST_VARIABLE || aLvalNode->type != AST_VARIABLE) {
        fprintf(stderr, "Runtime error: GetPixelColor R,G,B,A parameters must be variables.\n");
        goto cleanup_getpixel;
    }


    int x = (int)xVal.i_val;
    int y = (int)yVal.i_val;

    SDL_Rect pixelRect = {x, y, 1, 1};
    Uint8 rgba[4] = {0, 0, 0, 0}; // R, G, B, A

    // Read the pixel from the current render target
    // SDL_RenderReadPixels reads into a buffer of pixels in the renderer's format.
    // It's often ARGB8888 or RGBA8888 on modern systems.
    // We need to know the renderer's format to interpret the bytes correctly.
    // For simplicity here, we assume we can read it into a small known format buffer or surface.

    // A more robust way:
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        fprintf(stderr, "Runtime error: Could not create surface for GetPixelColor: %s\n", SDL_GetError());
        goto cleanup_getpixel;
    }

    if (SDL_RenderReadPixels(gSdlRenderer, &pixelRect, surface->format->format, surface->pixels, surface->pitch) != 0) {
        fprintf(stderr, "Runtime warning: SDL_RenderReadPixels failed in GetPixelColor: %s\n", SDL_GetError());
        // Set VAR params to default (e.g., 0) or error indicators
        updateSymbol(rLvalNode->token->value, makeByte(0));
        updateSymbol(gLvalNode->token->value, makeByte(0));
        updateSymbol(bLvalNode->token->value, makeByte(0));
        updateSymbol(aLvalNode->token->value, makeByte(0));
        SDL_FreeSurface(surface);
        goto cleanup_getpixel;
    }

    // Get the single pixel from the surface
    Uint32 pixelValue = ((Uint32*)surface->pixels)[0];
    SDL_GetRGBA(pixelValue, surface->format, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);

    SDL_FreeSurface(surface);

    // Assign to Pscal VAR parameters (as Byte)
    updateSymbol(rLvalNode->token->value, makeByte(rgba[0]));
    updateSymbol(gLvalNode->token->value, makeByte(rgba[1]));
    updateSymbol(bLvalNode->token->value, makeByte(rgba[2]));
    updateSymbol(aLvalNode->token->value, makeByte(rgba[3]));

cleanup_getpixel:
    freeValue(&xVal);
    freeValue(&yVal);
    return makeVoid();
}

// --- MODIFIED SdlCleanupAtExit to include IMG_Quit ---
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

    // --- Clean up SDL_image resources --- // <<< NEW SECTION
    if (gSdlImageInitialized) {
        IMG_Quit();
        gSdlImageInitialized = false;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: IMG_Quit successful.\n");
        #endif
    }
    // --- END NEW SECTION ---


    // --- Clean up SDL_mixer audio resources ---
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (gLoadedSounds[i] != NULL) {
            Mix_FreeChunk(gLoadedSounds[i]);
            gLoadedSounds[i] = NULL;
            DEBUG_PRINT("[DEBUG AUDIO] SdlCleanupAtExit: Auto-freed sound chunk at index %d.\n", i);
        }
    }
    int open_freq, open_channels;
    Uint16 open_format;
    if (Mix_QuerySpec(&open_freq, &open_format, &open_channels) != 0) {
        Mix_CloseAudio();
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG AUDIO] SdlCleanupAtExit: Mix_CloseAudio successful.\n");
        #endif
    } else {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG AUDIO] SdlCleanupAtExit: Mix_CloseAudio skipped (audio not open or already closed by audioQuitSystem).\n");
        #endif
    }
    Mix_Quit();
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG AUDIO] SdlCleanupAtExit: Mix_Quit successful.\n");
    #endif
    gSoundSystemInitialized = false;


    // --- Clean up core SDL video and timer resources ---
    // Destroy textures first
/*    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        if (gSdlTextures[i] != NULL) {
            SDL_DestroyTexture(gSdlTextures[i]);
            gSdlTextures[i] = NULL;
        }
    }
 */
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
    if (gSdlInitialized) {
        SDL_Quit();
        gSdlInitialized = false;
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit: SDL_Quit successful.\n");
        #endif
    }

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SDL] SdlCleanupAtExit finished.\n");
    #endif
}

// Pscal: Procedure SetAlphaBlend(Enable: Boolean);
Value executeBuiltinSetAlphaBlend(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: SetAlphaBlend expects 1 argument (Enable: Boolean).\n");
        // Not calling EXIT_FAILURE_HANDLER for this type of error, just returning.
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime warning: Graphics system not initialized before SetAlphaBlend. Call ignored.\n");
        return makeVoid();
    }

    Value enableVal = eval(node->children[0]);
    if (enableVal.type != TYPE_BOOLEAN) {
        fprintf(stderr, "Runtime error: SetAlphaBlend argument must be a Boolean. Got %s.\n", varTypeToString(enableVal.type));
        freeValue(&enableVal); // Free evaluated value if it was complex
        return makeVoid();
    }

    SDL_BlendMode blendModeToSet;
    if (enableVal.i_val != 0) { // Pscal TRUE (non-zero integer for boolean)
        blendModeToSet = SDL_BLENDMODE_BLEND; // Enable alpha blending
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SetAlphaBlend: Enabling SDL_BLENDMODE_BLEND.\n");
        #endif
    } else { // Pscal FALSE (zero integer for boolean)
        blendModeToSet = SDL_BLENDMODE_NONE;  // Disable alpha blending (opaque)
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG SDL] SetAlphaBlend: Enabling SDL_BLENDMODE_NONE.\n");
        #endif
    }

    if (SDL_SetRenderDrawBlendMode(gSdlRenderer, blendModeToSet) != 0) {
        fprintf(stderr, "Runtime warning: SDL_SetRenderDrawBlendMode failed in SetAlphaBlend: %s\n", SDL_GetError());
    }

    freeValue(&enableVal); // Booleans don't have heap data, but good practice
    return makeVoid();
}

// Pscal: Function GetTicks: Cardinal; (Cardinal often maps to an unsigned 32-bit integer)
Value executeBuiltinGetTicks(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: GetTicks expects 0 arguments.\n");
        // EXIT_FAILURE_HANDLER(); // Or return a sensible default if preferred
        return makeInt(0); // Return 0 or an error indicator
    }

    // SDL_GetTicks() returns Uint32, which is unsigned.
    // makeInt() currently creates a Value with i_val (long long).
    // This is fine as Uint32 will fit into long long without loss.
    // The Pscal type 'Cardinal' would ideally map to an unsigned integer type
    // in your Pscal type system, but returning it as a standard INTEGER value
    // in Pscal is acceptable for now.
    Uint32 ticks = SDL_GetTicks();

    #ifdef DEBUG
    // fprintf(stderr, "[DEBUG SDL] GetTicks returning: %u\n", ticks); // Use %u for Uint32
    #endif

    return makeInt((long long)ticks); // Cast Uint32 to long long for makeInt
}

// Pscal: FUNCTION RenderTextToTexture(TextToRender: String; R, G, B: Byte): Integer;
Value executeBuiltinRenderTextToTexture(AST *node) {
    if (node->child_count != 4) { // Text, R, G, B
        fprintf(stderr, "Runtime error: RenderTextToTexture expects 4 arguments (Text: String; R, G, B: Byte).\n");
        return makeInt(-1); // Error code
    }

    // Ensure graphics and text systems are initialized
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics system not initialized before RenderTextToTexture.\n");
        return makeInt(-1);
    }
    if (!gSdlTtfInitialized || !gSdlFont) {
        fprintf(stderr, "Runtime error: Text system or font not initialized before RenderTextToTexture.\n");
        return makeInt(-1);
    }

    // Evaluate arguments
    Value textVal = eval(node->children[0]);
    Value rVal = eval(node->children[1]);
    Value gVal = eval(node->children[2]);
    Value bVal = eval(node->children[3]);

    // Type checking
    if (textVal.type != TYPE_STRING ||
        (rVal.type != TYPE_INTEGER && rVal.type != TYPE_BYTE) ||
        (gVal.type != TYPE_INTEGER && gVal.type != TYPE_BYTE) ||
        (bVal.type != TYPE_INTEGER && bVal.type != TYPE_BYTE)) {
        fprintf(stderr, "Runtime error: RenderTextToTexture argument type mismatch. Expected (String, Byte, Byte, Byte).\n");
        freeValue(&textVal);
        freeValue(&rVal);
        freeValue(&gVal);
        freeValue(&bVal);
        return makeInt(-1);
    }

    const char* text_to_render = textVal.s_val ? textVal.s_val : "";
    SDL_Color textColor;
    textColor.r = (Uint8)(rVal.i_val & 0xFF);
    textColor.g = (Uint8)(gVal.i_val & 0xFF);
    textColor.b = (Uint8)(bVal.i_val & 0xFF);
    textColor.a = 255; // Full opacity for text rendering

    // Free evaluated color arguments
    freeValue(&rVal);
    freeValue(&gVal);
    freeValue(&bVal);

    // Render text to surface
    SDL_Surface* textSurface = TTF_RenderUTF8_Solid(gSdlFont, text_to_render, textColor);
    if (!textSurface) {
        fprintf(stderr, "Runtime error: TTF_RenderUTF8_Solid failed in RenderTextToTexture: %s\n", TTF_GetError());
        freeValue(&textVal);
        return makeInt(-1);
    }

    // Free the evaluated text string now that the surface is created
    freeValue(&textVal);

    // Create texture from surface
    SDL_Texture* newTexture = SDL_CreateTextureFromSurface(gSdlRenderer, textSurface);
    if (!newTexture) {
        fprintf(stderr, "Runtime error: SDL_CreateTextureFromSurface failed in RenderTextToTexture: %s\n", SDL_GetError());
        SDL_FreeSurface(textSurface);
        return makeInt(-1);
    }

    // The surface is no longer needed after creating the texture
    SDL_FreeSurface(textSurface);

    // Find a free slot for the new texture
    int textureID = findFreeTextureID(); // Ensure this function is accessible
    if (textureID == -1) {
        fprintf(stderr, "Runtime error: Maximum number of textures reached. Cannot create text texture.\n");
        SDL_DestroyTexture(newTexture); // Clean up the created texture
        return makeInt(-1);
    }

    // Store the texture and its dimensions
    gSdlTextures[textureID] = newTexture;
    SDL_QueryTexture(newTexture, NULL, NULL, &gSdlTextureWidths[textureID], &gSdlTextureHeights[textureID]);

    // Important for rendering text with transparent backgrounds correctly
    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG SDL] RenderTextToTexture: Created texture ID %d for text \"%s\" (%dx%d).\n",
            textureID, text_to_render, gSdlTextureWidths[textureID], gSdlTextureHeights[textureID]);
    #endif

    return makeInt(textureID); // Return the 0-based TextureID
}

Value vmBuiltinInitgraph(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3 || args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER || args[2].type != TYPE_STRING) {
        runtimeError(vm, "VM Error: InitGraph expects (Integer, Integer, String)");
        return makeVoid();
    }

    if (!gSdlInitialized) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
            runtimeError(vm, "Runtime error: SDL_Init failed in InitGraph: %s", SDL_GetError());
            return makeVoid();
        }
        gSdlInitialized = true;
    }

    if (gSdlWindow || gSdlRenderer) {
        if(gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
        if(gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
    }

    int width = (int)args[0].i_val;
    int height = (int)args[1].i_val;
    const char* title = args[2].s_val ? args[2].s_val : "Pscal Graphics";

    if (width <= 0 || height <= 0) {
        runtimeError(vm, "Runtime error: InitGraph width and height must be positive.");
        return makeVoid();
    }
    
    gSdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
    if (!gSdlWindow) {
        runtimeError(vm, "Runtime error: SDL_CreateWindow failed: %s", SDL_GetError());
        return makeVoid();
    }

    gSdlWidth = width;
    gSdlHeight = height;

    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!gSdlRenderer) {
        runtimeError(vm, "Runtime error: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
        return makeVoid();
    }

    InitializeTextureSystem();
    
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderPresent(gSdlRenderer);

    gSdlCurrentColor.r = 255; gSdlCurrentColor.g = 255; gSdlCurrentColor.b = 255; gSdlCurrentColor.a = 255;
    
    return makeVoid();
}

Value vmBuiltinClosegraph(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "CloseGraph expects 0 arguments.");
    if (gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
    if (gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
    return makeVoid();
}

Value vmBuiltinFillrect(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) { runtimeError(vm, "FillRect expects 4 integer arguments."); return makeVoid(); }
    // ... type checks for all 4 args being integer ...
    SDL_Rect rect;
    rect.x = (int)args[0].i_val;
    rect.y = (int)args[1].i_val;
    rect.w = (int)args[2].i_val - rect.x + 1;
    rect.h = (int)args[3].i_val - rect.y + 1;
    if (rect.w < 0) { rect.x += rect.w; rect.w = -rect.w; }
    if (rect.h < 0) { rect.y += rect.h; rect.h = -rect.h; }
    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderFillRect(gSdlRenderer, &rect);
    return makeVoid();
}

Value vmBuiltinUpdatetexture(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "UpdateTexture expects 2 arguments (TextureID: Integer; PixelData: ARRAY OF Byte).");
        return makeVoid();
    }

    Value idVal = args[0];
    Value pixelDataVal = args[1];

    if (idVal.type != TYPE_INTEGER || pixelDataVal.type != TYPE_ARRAY) {
        runtimeError(vm, "UpdateTexture argument type mismatch.");
        return makeVoid();
    }
    if (pixelDataVal.element_type != TYPE_BYTE) {
        runtimeError(vm, "UpdateTexture PixelData must be an ARRAY OF Byte. Got array of %s.", varTypeToString(pixelDataVal.element_type));
        return makeVoid();
    }

    int textureID = (int)idVal.i_val;
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        runtimeError(vm, "UpdateTexture called with invalid TextureID %d.", textureID);
        return makeVoid();
    }

    int texWidth = gSdlTextureWidths[textureID];
    int pitch = texWidth * 4; // 4 bytes per pixel for RGBA8888
    int expectedPscalArraySize = texWidth * gSdlTextureHeights[textureID] * 4;
    int pscalArrayTotalElements = calculateArrayTotalSize(&pixelDataVal);

    if (pscalArrayTotalElements != expectedPscalArraySize) {
        runtimeError(vm, "UpdateTexture PixelData array size (%d) does not match texture dimensions*BPP (%d).",
                pscalArrayTotalElements, expectedPscalArraySize);
        return makeVoid();
    }

    // Since the VM passes direct Value pointers, we can create a temporary C buffer from them.
    unsigned char* c_pixel_buffer = (unsigned char*)malloc(expectedPscalArraySize);
    if (!c_pixel_buffer) {
        runtimeError(vm, "Failed to allocate C buffer for UpdateTexture.");
        return makeVoid();
    }

    for (int i = 0; i < expectedPscalArraySize; ++i) {
        c_pixel_buffer[i] = (unsigned char)pixelDataVal.array_val[i].i_val;
    }

    if (SDL_UpdateTexture(gSdlTextures[textureID], NULL, c_pixel_buffer, pitch) != 0) {
        runtimeError(vm, "SDL_UpdateTexture failed: %s", SDL_GetError());
    }

    free(c_pixel_buffer);
    return makeVoid();
}

Value vmBuiltinUpdatescreen(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "UpdateScreen expects 0 arguments.");
    if (gSdlRenderer) SDL_RenderPresent(gSdlRenderer);
    return makeVoid();
}

Value vmBuiltinCleardevice(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "Runtime error: ClearDevice expects 0 arguments.");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Runtime error: Graphics mode not initialized before ClearDevice.");
        return makeVoid();
    }
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gSdlRenderer);
    return makeVoid();
}

Value vmBuiltinGetmaxx(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "GetMaxX expects 0 arguments.");
    return makeInt(gSdlWidth > 0 ? gSdlWidth - 1 : 0);
}

Value vmBuiltinGetmaxy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "GetMaxY expects 0 arguments.");
    return makeInt(gSdlHeight > 0 ? gSdlHeight - 1 : 0);
}

Value vmBuiltinGetticks(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "GetTicks expects 0 arguments.");
        return makeInt(0);
    }
    return makeInt((long long)SDL_GetTicks());
}

Value vmBuiltinSetrgbcolor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) { runtimeError(vm, "SetRGBColor expects 3 arguments."); return makeVoid(); }
    gSdlCurrentColor.r = (Uint8)args[0].i_val;
    gSdlCurrentColor.g = (Uint8)args[1].i_val;
    gSdlCurrentColor.b = (Uint8)args[2].i_val;
    gSdlCurrentColor.a = 255;
    if (gSdlRenderer) {
        SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    }
    return makeVoid();
}

Value vmBuiltinQuittextsystem(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) runtimeError(vm, "QuitTextSystem expects 0 arguments.");
    if (gSdlFont) { TTF_CloseFont(gSdlFont); gSdlFont = NULL; }
    if (gSdlTtfInitialized) { TTF_Quit(); gSdlTtfInitialized = false; }
    return makeVoid();
}

Value vmBuiltinGettextsize(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) { runtimeError(vm, "GetTextSize expects 3 arguments."); return makeVoid(); }
    if (!gSdlFont) { runtimeError(vm, "Font not initialized for GetTextSize."); return makeVoid(); }
    
    const char* text = AS_STRING(args[0]);
    Value* width_ptr = (Value*)args[1].ptr_val;
    Value* height_ptr = (Value*)args[2].ptr_val;
    
    int w, h;
    TTF_SizeUTF8(gSdlFont, text, &w, &h);
    
    freeValue(width_ptr); *width_ptr = makeInt(w);
    freeValue(height_ptr); *height_ptr = makeInt(h);
    
    return makeVoid();
}

Value vmBuiltinGetmousestate(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "GetMouseState expects 3 arguments.");
        return makeVoid();
    }

    // --- ADD SAFETY CHECKS ---
    if (args[0].type != TYPE_POINTER || args[1].type != TYPE_POINTER || args[2].type != TYPE_POINTER) {
        runtimeError(vm, "GetMouseState requires VAR parameters, but a non-pointer type was received.");
        return makeVoid();
    }

    Value* x_ptr = (Value*)args[0].ptr_val;
    Value* y_ptr = (Value*)args[1].ptr_val;
    Value* buttons_ptr = (Value*)args[2].ptr_val;

    if (!x_ptr || !y_ptr || !buttons_ptr) {
        runtimeError(vm, "GetMouseState received a NIL pointer for a VAR parameter.");
        return makeVoid();
    }
    // --- END SAFETY CHECKS ---

    int mse_x, mse_y;
    Uint32 sdl_buttons = SDL_GetMouseState(&mse_x, &mse_y);
    
    int pscal_buttons = 0;
    if (sdl_buttons & SDL_BUTTON_LMASK) pscal_buttons |= 1;
    if (sdl_buttons & SDL_BUTTON_MMASK) pscal_buttons |= 2;
    if (sdl_buttons & SDL_BUTTON_RMASK) pscal_buttons |= 4;

    // Safely assign the retrieved values
    freeValue(x_ptr);
    *x_ptr = makeInt(mse_x);

    freeValue(y_ptr);
    *y_ptr = makeInt(mse_y);

    freeValue(buttons_ptr);
    *buttons_ptr = makeInt(pscal_buttons);

    return makeVoid();
}

Value vmBuiltinDestroytexture(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) { runtimeError(vm, "DestroyTexture expects 1 integer argument."); return makeVoid(); }
    int textureID = (int)args[0].i_val;
    if (textureID >= 0 && textureID < MAX_SDL_TEXTURES && gSdlTextures[textureID]) {
        SDL_DestroyTexture(gSdlTextures[textureID]);
        gSdlTextures[textureID] = NULL;
    }
    return makeVoid();
}

Value vmBuiltinRendercopyrect(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5) {
        fprintf(stderr, "Runtime error: RenderCopyRect expects 5 arguments.\n");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before RenderCopyRect.\n");
        return makeVoid();
    }
    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER || args[2].type != TYPE_INTEGER ||
        args[3].type != TYPE_INTEGER || args[4].type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: RenderCopyRect expects integer arguments (TextureID, DestX, DestY, DestW, DestH).\n");
        return makeVoid();
    }
    int textureID = (int)args[0].i_val;
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || !gSdlTextures[textureID]) {
        fprintf(stderr, "Runtime error: RenderCopyRect called with invalid TextureID.\n");
        return makeVoid();
    }
    SDL_Rect dstRect = { (int)args[1].i_val, (int)args[2].i_val, (int)args[3].i_val, (int)args[4].i_val };
    SDL_RenderCopy(gSdlRenderer, gSdlTextures[textureID], NULL, &dstRect);
    return makeVoid();
}

Value vmBuiltinSetalphablend(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_BOOLEAN) { runtimeError(vm, "SetAlphaBlend expects 1 boolean argument."); return makeVoid(); }
    SDL_BlendMode mode = AS_BOOLEAN(args[0]) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
    if (gSdlRenderer) SDL_SetRenderDrawBlendMode(gSdlRenderer, mode);
    return makeVoid();
}

Value vmBuiltinRendertexttotexture(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) { runtimeError(vm, "RenderTextToTexture expects 4 arguments."); return makeInt(-1); }
    if (!gSdlFont) { runtimeError(vm, "Font not initialized for RenderTextToTexture."); return makeInt(-1); }

    const char* text = AS_STRING(args[0]);
    SDL_Color color = { (Uint8)args[1].i_val, (Uint8)args[2].i_val, (Uint8)args[3].i_val, 255 };
    
    SDL_Surface* surf = TTF_RenderUTF8_Solid(gSdlFont, text, color);
    if (!surf) return makeInt(-1);

    SDL_Texture* tex = SDL_CreateTextureFromSurface(gSdlRenderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return makeInt(-1);

    int free_slot = -1;
    for(int i = 0; i < MAX_SDL_TEXTURES; i++) { if (!gSdlTextures[i]) { free_slot = i; break; } }
    if (free_slot == -1) { SDL_DestroyTexture(tex); return makeInt(-1); }

    gSdlTextures[free_slot] = tex;
    SDL_QueryTexture(tex, NULL, NULL, &gSdlTextureWidths[free_slot], &gSdlTextureHeights[free_slot]);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    return makeInt(free_slot);
}

Value vmBuiltinInittextsystem(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "InitTextSystem expects 2 arguments (FontFileName: String; FontSize: Integer).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Graphics system not initialized before InitTextSystem.");
        return makeVoid();
    }
    // Lazy Initialize SDL_ttf if not already done
    if (!gSdlTtfInitialized) {
        if (TTF_Init() == -1) {
            runtimeError(vm, "SDL_ttf system initialization failed: %s", TTF_GetError());
            return makeVoid();
        }
        gSdlTtfInitialized = true;
    }

    // Get arguments from the args array
    Value fontNameVal = args[0];
    Value fontSizeVal = args[1];

    if (fontNameVal.type != TYPE_STRING || fontSizeVal.type != TYPE_INTEGER) {
        runtimeError(vm, "InitTextSystem argument type mismatch. Expected (String, Integer).");
        return makeVoid(); // Don't free args, they are on the VM stack
    }

    const char* font_path = fontNameVal.s_val;
    int font_size = (int)fontSizeVal.i_val;

    // Close previous font if one was loaded
    if (gSdlFont) {
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
    }

    gSdlFont = TTF_OpenFont(font_path, font_size);
    if (!gSdlFont) {
        runtimeError(vm, "Failed to load font '%s': %s", font_path, TTF_GetError());
        return makeVoid();
    }
    gSdlFontSize = font_size;

    return makeVoid();
}

Value vmBuiltinCreatetargettexture(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "CreateTargetTexture expects 2 arguments (Width, Height: Integer)."); return makeInt(-1); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before CreateTargetTexture."); return makeInt(-1); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER) { runtimeError(vm, "CreateTargetTexture arguments must be integers."); return makeInt(-1); }

    int width = (int)args[0].i_val;
    int height = (int)args[1].i_val;

    if (width <= 0 || height <= 0) { runtimeError(vm, "CreateTargetTexture dimensions must be positive."); return makeInt(-1); }

    int textureID = findFreeTextureID();
    if (textureID == -1) { runtimeError(vm, "Maximum number of textures reached."); return makeInt(-1); }

    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (!newTexture) { runtimeError(vm, "SDL_CreateTexture (for target) failed: %s", SDL_GetError()); return makeInt(-1); }

    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);

    gSdlTextures[textureID] = newTexture;
    gSdlTextureWidths[textureID] = width;
    gSdlTextureHeights[textureID] = height;
    return makeInt(textureID);
}

Value vmBuiltinCreatetexture(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "CreateTexture expects 2 arguments (Width, Height: Integer)."); return makeInt(-1); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics not initialized before CreateTexture."); return makeInt(-1); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER) { runtimeError(vm, "CreateTexture arguments must be integers."); return makeInt(-1); }

    int width = (int)args[0].i_val;
    int height = (int)args[1].i_val;

    if (width <= 0 || height <= 0) { runtimeError(vm, "CreateTexture dimensions must be positive."); return makeInt(-1); }

    int textureID = findFreeTextureID();
    if (textureID == -1) { runtimeError(vm, "Maximum number of textures reached."); return makeInt(-1); }

    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!newTexture) { runtimeError(vm, "SDL_CreateTexture failed: %s", SDL_GetError()); return makeInt(-1); }

    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);

    gSdlTextures[textureID] = newTexture;
    gSdlTextureWidths[textureID] = width;
    gSdlTextureHeights[textureID] = height;
    return makeInt(textureID);
}

Value vmBuiltinDrawcircle(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) { runtimeError(vm, "DrawCircle expects 3 integer arguments (CenterX, CenterY, Radius)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before DrawCircle."); return makeVoid(); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER || args[2].type != TYPE_INTEGER) { runtimeError(vm, "DrawCircle arguments must be integers."); return makeVoid(); }

    int centerX = (int)args[0].i_val;
    int centerY = (int)args[1].i_val;
    int radius = (int)args[2].i_val;

    if (radius < 0) return makeVoid();

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);

    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        SDL_RenderDrawPoint(gSdlRenderer, centerX + x, centerY + y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - x, centerY + y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX + x, centerY - y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - x, centerY - y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX + y, centerY + x);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - y, centerY + x);
        SDL_RenderDrawPoint(gSdlRenderer, centerX + y, centerY - x);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - y, centerY - x);
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

Value vmBuiltinDrawline(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) { runtimeError(vm, "DrawLine expects 4 integer arguments (x1, y1, x2, y2)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before DrawLine."); return makeVoid(); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER ||
        args[2].type != TYPE_INTEGER || args[3].type != TYPE_INTEGER) { runtimeError(vm, "DrawLine arguments must be integers."); return makeVoid(); }

    int x1 = (int)args[0].i_val;
    int y1 = (int)args[1].i_val;
    int x2 = (int)args[2].i_val;
    int y2 = (int)args[3].i_val;

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderDrawLine(gSdlRenderer, x1, y1, x2, y2);
    return makeVoid();
}

Value vmBuiltinDrawpolygon(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "DrawPolygon expects 2 arguments (PointsArray, NumPoints)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics not initialized for DrawPolygon."); return makeVoid(); }

    if (args[0].type != TYPE_ARRAY || args[1].type != TYPE_INTEGER) { runtimeError(vm, "DrawPolygon argument type mismatch."); return makeVoid(); }
    if (args[0].element_type != TYPE_RECORD) { runtimeError(vm, "DrawPolygon Points argument must be an ARRAY OF PointRecord."); return makeVoid(); }

    int numPoints = (int)args[1].i_val;
    if (numPoints < 2) return makeVoid();

    int total_elements_in_pascal_array = 1;
    for(int i=0; i < args[0].dimensions; ++i) { total_elements_in_pascal_array *= (args[0].upper_bounds[i] - args[0].lower_bounds[i] + 1); }
    if (numPoints > total_elements_in_pascal_array) { runtimeError(vm, "NumPoints exceeds actual size of Pscal PointsArray."); return makeVoid(); }

    SDL_Point* sdlPoints = malloc(sizeof(SDL_Point) * (numPoints + 1));
    if (!sdlPoints) { runtimeError(vm, "Memory allocation failed for SDL_Point array in DrawPolygon."); return makeVoid(); }

    for (int i = 0; i < numPoints; i++) {
        Value recordValue = args[0].array_val[i];
        if (recordValue.type != TYPE_RECORD || !recordValue.record_val) { runtimeError(vm, "Element in PointsArray is not a valid PointRecord."); free(sdlPoints); return makeVoid(); }
        FieldValue* fieldX = recordValue.record_val;
        FieldValue* fieldY = fieldX ? fieldX->next : NULL;

        if (fieldX && strcasecmp(fieldX->name, "x") == 0 && fieldX->value.type == TYPE_INTEGER &&
            fieldY && strcasecmp(fieldY->name, "y") == 0 && fieldY->value.type == TYPE_INTEGER) {
            sdlPoints[i].x = (int)fieldX->value.i_val;
            sdlPoints[i].y = (int)fieldY->value.i_val;
        } else { runtimeError(vm, "PointRecord does not have correct X,Y integer fields."); free(sdlPoints); return makeVoid(); }
    }

    if (numPoints > 1) { sdlPoints[numPoints] = sdlPoints[0]; } // Close the polygon

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderDrawLines(gSdlRenderer, sdlPoints, numPoints > 1 ? numPoints + 1 : numPoints);

    free(sdlPoints);
    return makeVoid();
}

Value vmBuiltinDrawrect(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) { runtimeError(vm, "DrawRect expects 4 integer arguments (X1, Y1, X2, Y2)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before DrawRect."); return makeVoid(); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER ||
        args[2].type != TYPE_INTEGER || args[3].type != TYPE_INTEGER) { runtimeError(vm, "DrawRect arguments must be integers."); return makeVoid(); }

    int x1 = (int)args[0].i_val;
    int y1 = (int)args[1].i_val;
    int x2 = (int)args[2].i_val;
    int y2 = (int)args[3].i_val;

    SDL_Rect rect;
    rect.x = (x1 < x2) ? x1 : x2;
    rect.y = (y1 < y2) ? y1 : y2;
    rect.w = abs(x2 - x1) + 1;
    rect.h = abs(y2 - y1) + 1;

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderDrawRect(gSdlRenderer, &rect);
    return makeVoid();
}

Value vmBuiltinGetpixelcolor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 6) { runtimeError(vm, "GetPixelColor expects 6 arguments (X, Y: Integer; var R, G, B, A: Byte)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics not initialized for GetPixelColor."); return makeVoid(); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER) { runtimeError(vm, "GetPixelColor X,Y coordinates must be integers."); return makeVoid(); }

    if (args[2].type != TYPE_POINTER || args[3].type != TYPE_POINTER ||
        args[4].type != TYPE_POINTER || args[5].type != TYPE_POINTER) { runtimeError(vm, "GetPixelColor R,G,B,A parameters must be VAR Byte."); return makeVoid(); }

    int x = (int)args[0].i_val;
    int y = (int)args[1].i_val;

    Value* r_ptr = (Value*)args[2].ptr_val;
    Value* g_ptr = (Value*)args[3].ptr_val;
    Value* b_ptr = (Value*)args[4].ptr_val;
    Value* a_ptr = (Value*)args[5].ptr_val;

    if (!r_ptr || !g_ptr || !b_ptr || !a_ptr) { runtimeError(vm, "Null pointer for RGBA output in GetPixelColor."); return makeVoid(); }

    SDL_Rect pixelRect = {x, y, 1, 1};
    Uint8 rgba[4] = {0, 0, 0, 0};

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) { runtimeError(vm, "Could not create surface for GetPixelColor: %s", SDL_GetError()); return makeVoid(); }

    if (SDL_RenderReadPixels(gSdlRenderer, &pixelRect, surface->format->format, surface->pixels, surface->pitch) != 0) {
        runtimeError(vm, "SDL_RenderReadPixels failed in GetPixelColor: %s", SDL_GetError());
        SDL_FreeSurface(surface);
        return makeVoid();
    }

    Uint32 pixelValue = ((Uint32*)surface->pixels)[0];
    SDL_GetRGBA(pixelValue, surface->format, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);

    SDL_FreeSurface(surface);

    freeValue(r_ptr); *r_ptr = makeByte(rgba[0]);
    freeValue(g_ptr); *g_ptr = makeByte(rgba[1]);
    freeValue(b_ptr); *b_ptr = makeByte(rgba[2]);
    freeValue(a_ptr); *a_ptr = makeByte(rgba[3]);

    return makeVoid();
}

Value vmBuiltinLoadimagetotexture(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) { runtimeError(vm, "LoadImageToTexture expects 1 argument (FilePath: String)."); return makeInt(-1); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before LoadImageToTexture."); return makeInt(-1); }

    if (!gSdlImageInitialized) {
        int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) { runtimeError(vm, "SDL_image initialization failed: %s", IMG_GetError()); return makeInt(-1); }
        gSdlImageInitialized = true;
    }

    const char* filePath = args[0].s_val;
    int free_slot = findFreeTextureID();
    if (free_slot == -1) { runtimeError(vm, "No free texture slots available for LoadImageToTexture."); return makeInt(-1); }

    SDL_Texture* newTexture = IMG_LoadTexture(gSdlRenderer, filePath);
    if (!newTexture) { runtimeError(vm, "Failed to load image '%s' as texture: %s", filePath, IMG_GetError()); return makeInt(-1); }

    gSdlTextures[free_slot] = newTexture;
    SDL_QueryTexture(newTexture, NULL, NULL, &gSdlTextureWidths[free_slot], &gSdlTextureHeights[free_slot]);
    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);
    return makeInt(free_slot);
}

Value vmBuiltinOuttextxy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) { runtimeError(vm, "OutTextXY expects 3 arguments (X, Y: Integer; Text: String)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before OutTextXY."); return makeVoid(); }
    if (!gSdlTtfInitialized || !gSdlFont) { runtimeError(vm, "Text system or font not initialized before OutTextXY."); return makeVoid(); }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER || args[2].type != TYPE_STRING) { runtimeError(vm, "OutTextXY argument type mismatch."); return makeVoid(); }

    int x = (int)args[0].i_val;
    int y = (int)args[1].i_val;
    const char* text_to_render = args[2].s_val ? args[2].s_val : "";

    SDL_Surface* textSurface = TTF_RenderUTF8_Solid(gSdlFont, text_to_render, gSdlCurrentColor);
    if (!textSurface) { runtimeError(vm, "TTF_RenderUTF8_Solid failed in OutTextXY: %s", TTF_GetError()); return makeVoid(); }

    SDL_Texture* textTexture = SDL_CreateTextureFromSurface(gSdlRenderer, textSurface);
    if (!textTexture) { SDL_FreeSurface(textSurface); runtimeError(vm, "SDL_CreateTextureFromSurface failed in OutTextXY: %s", SDL_GetError()); return makeVoid(); }

    SDL_Rect destRect = { x, y, textSurface->w, textSurface->h };
    SDL_RenderCopy(gSdlRenderer, textTexture, NULL, &destRect);

    SDL_DestroyTexture(textTexture);
    SDL_FreeSurface(textSurface);
    return makeVoid();
}

Value vmBuiltinRendercopy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: RenderCopy expects 1 argument (TextureID: Integer).\n");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before RenderCopy.\n");
        return makeVoid();
    }

    int textureID = (int)args[0].i_val;
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: RenderCopy called with invalid TextureID.\n");
        return makeVoid();
    }

    SDL_RenderCopy(gSdlRenderer, gSdlTextures[textureID], NULL, NULL);
    return makeVoid();
}

Value vmBuiltinRendercopyex(VM* vm, int arg_count, Value* args) {
    if (arg_count != 13) {
        fprintf(stderr, "Runtime error: RenderCopyEx expects 13 arguments.\n");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before RenderCopyEx.\n");
        return makeVoid();
    }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER || args[2].type != TYPE_INTEGER ||
        args[3].type != TYPE_INTEGER || args[4].type != TYPE_INTEGER || args[5].type != TYPE_INTEGER ||
        args[6].type != TYPE_INTEGER || args[7].type != TYPE_INTEGER || args[8].type != TYPE_INTEGER ||
        args[9].type != TYPE_REAL || args[10].type != TYPE_INTEGER || args[11].type != TYPE_INTEGER ||
        args[12].type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: RenderCopyEx argument type mismatch. Expected (Int,Int,Int,Int,Int,Int,Int,Int,Int,Real,Int,Int,Int).\n");
        return makeVoid();
    }

    int textureID = (int)args[0].i_val;
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: RenderCopyEx called with invalid or unloaded TextureID.\n");
        return makeVoid();
    }
    SDL_Texture* texture = gSdlTextures[textureID];

    SDL_Rect srcRect = { (int)args[1].i_val, (int)args[2].i_val, (int)args[3].i_val, (int)args[4].i_val };
    SDL_Rect* srcRectPtr = (srcRect.w > 0 && srcRect.h > 0) ? &srcRect : NULL;

    SDL_Rect dstRect = { (int)args[5].i_val, (int)args[6].i_val, (int)args[7].i_val, (int)args[8].i_val };

    double angle_degrees = args[9].r_val;

    SDL_Point rotationCenter;
    SDL_Point* centerPtr = NULL;
    int pscalRotX = (int)args[10].i_val;
    int pscalRotY = (int)args[11].i_val;

    if (pscalRotX >= 0 && pscalRotY >= 0) {
        rotationCenter.x = pscalRotX;
        rotationCenter.y = pscalRotY;
        centerPtr = &rotationCenter;
    }

    SDL_RendererFlip sdl_flip = SDL_FLIP_NONE;
    int flipMode = (int)args[12].i_val;
    if (flipMode == 1) sdl_flip = SDL_FLIP_HORIZONTAL;
    else if (flipMode == 2) sdl_flip = SDL_FLIP_VERTICAL;
    else if (flipMode == 3) sdl_flip = (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);

    SDL_RenderCopyEx(gSdlRenderer, texture, srcRectPtr, &dstRect, angle_degrees, centerPtr, sdl_flip);
    return makeVoid();
}

Value vmBuiltinSetcolor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) { runtimeError(vm, "SetColor expects 1 argument (color index 0-255)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before SetColor."); return makeVoid(); }

    long long colorCode = args[0].i_val;

    if (colorCode >= 0 && colorCode <= 15) {
         unsigned char intensity = (colorCode > 7) ? 255 : 192;
         gSdlCurrentColor.r = (colorCode & 4) ? intensity : 0;
         gSdlCurrentColor.g = (colorCode & 2) ? intensity : 0;
         gSdlCurrentColor.b = (colorCode & 1) ? intensity : 0;
         if (colorCode == 6) { gSdlCurrentColor.g = intensity / 2; }
         if (colorCode == 7 || colorCode == 15) { gSdlCurrentColor.r=intensity; gSdlCurrentColor.g=intensity; gSdlCurrentColor.b=intensity; }
         if (colorCode == 8) {gSdlCurrentColor.r = 128; gSdlCurrentColor.g = 128; gSdlCurrentColor.b = 128;}
         if (colorCode == 0) {gSdlCurrentColor.r = 0; gSdlCurrentColor.g = 0; gSdlCurrentColor.b = 0;}
    } else {
         int c = (int)(colorCode % 256);
         gSdlCurrentColor.r = (c * 3) % 256;
         gSdlCurrentColor.g = (c * 5) % 256;
         gSdlCurrentColor.b = (c * 7) % 256;
    }
    gSdlCurrentColor.a = 255;

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);

    return makeVoid();
}

Value vmBuiltinSetrendertarget(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) { runtimeError(vm, "SetRenderTarget expects 1 argument (TextureID: Integer)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before SetRenderTarget."); return makeVoid(); }

    int textureID = (int)args[0].i_val;

    SDL_Texture* targetTexture = NULL;
    if (textureID >= 0 && textureID < MAX_SDL_TEXTURES && gSdlTextures[textureID] != NULL) {
        Uint32 format;
        int access;
        int w, h;
        if (SDL_QueryTexture(gSdlTextures[textureID], &format, &access, &w, &h) == 0) {
            if (access == SDL_TEXTUREACCESS_TARGET) {
                targetTexture = gSdlTextures[textureID];
            } else {
                runtimeError(vm, "TextureID %d was not created with Target access. Cannot set as render target.", textureID);
            }
        } else { runtimeError(vm, "Could not query texture %d for SetRenderTarget: %s", textureID, SDL_GetError()); }
    } else if (textureID >= MAX_SDL_TEXTURES || (textureID >=0 && gSdlTextures[textureID] == NULL)) {
        runtimeError(vm, "Invalid TextureID %d passed to SetRenderTarget. Defaulting to screen.", textureID);
    }

    SDL_SetRenderTarget(gSdlRenderer, targetTexture);
    return makeVoid();
}

Value vmBuiltinWaitkeyevent(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "WaitKeyEvent expects 0 arguments."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlWindow || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before WaitKeyEvent."); return makeVoid(); }

    SDL_Event event;
    int waiting = 1;
    while (waiting) {
        if (SDL_WaitEvent(&event)) {
            if (event.type == SDL_QUIT) { waiting = 0; }
            else if (event.type == SDL_KEYDOWN) { waiting = 0; }
        } else {
            runtimeError(vm, "SDL_WaitEvent failed: %s", SDL_GetError());
            waiting = 0;
        }
    }
    return makeVoid();
}

Value vmBuiltinFillcircle(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "FillCircle expects 3 integer arguments (CenterX, CenterY, Radius).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Graphics mode not initialized before FillCircle.");
        return makeVoid();
    }

    // Type checking for arguments
    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER || args[2].type != TYPE_INTEGER) {
        runtimeError(vm, "FillCircle arguments must be integers.");
        return makeVoid();
    }

    int centerX = (int)args[0].i_val;
    int centerY = (int)args[1].i_val;
    int radius = (int)args[2].i_val;

    if (radius < 0) return makeVoid();

    // Set the draw color from the global state
    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    
    // Efficient filling method using horizontal lines
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = (int)floor(sqrt((double)(radius * radius) - (dy * dy)));
        SDL_RenderDrawLine(gSdlRenderer, centerX - dx, centerY + dy, centerX + dx, centerY + dy);
    }

    return makeVoid();
}

Value vmBuiltinGraphloop(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "GraphLoop expects 1 argument (milliseconds).");
        return makeVoid();
    }
    if (args[0].type != TYPE_INTEGER && args[0].type != TYPE_WORD && args[0].type != TYPE_BYTE) {
        runtimeError(vm, "GraphLoop argument must be an integer-like type.");
        return makeVoid();
    }

    long long ms = args[0].i_val;
    if (ms < 0) ms = 0;

    if (gSdlInitialized && gSdlWindow && gSdlRenderer) {
        Uint32 startTime = SDL_GetTicks();
        Uint32 targetTime = startTime + (Uint32)ms;
        SDL_Event event;

        while (SDL_GetTicks() < targetTime) {
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    break_requested = 1;
                    return makeVoid(); // Exit immediately on quit event
                }
            }
            SDL_Delay(1); // Prevent 100% CPU usage
        }
    }
    return makeVoid();
}

Value vmBuiltinPutpixel(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "PutPixel expects 2 arguments (X, Y).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Graphics mode not initialized before PutPixel.");
        return makeVoid();
    }

    if (args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER) {
        runtimeError(vm, "PutPixel coordinates must be integers.");
        return makeVoid();
    }

    int x = (int)args[0].i_val;
    int y = (int)args[1].i_val;

    // Set the draw color from the global state
    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    
    // Draw the point
    SDL_RenderDrawPoint(gSdlRenderer, x, y);
    
    return makeVoid();
}
#endif
