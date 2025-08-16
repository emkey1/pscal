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
    if (arg_count != 1 || (args[0].type != TYPE_INTEGER && args[0].type != TYPE_BYTE)) {
        runtimeError(vm, "SetColor expects 1 argument (color index 0-255).");
        return makeVoid();
    }
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

Value vmBuiltinPollkey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "PollKey expects 0 arguments.");
        return makeInt(0);
    }
    if (!gSdlInitialized || !gSdlWindow || !gSdlRenderer) {
        runtimeError(vm, "Graphics mode not initialized before PollKey.");
        return makeInt(0);
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            break_requested = 1;
            return makeInt(0);
        } else if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_q) {
                break_requested = 1;
            }
            return makeInt((int)event.key.keysym.sym);
        }
    }
    return makeInt(0);
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
