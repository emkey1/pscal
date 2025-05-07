//
//  sdl.c
//  Pscal
//
//  Created by Michael Miller on 5/7/25.
//
#include "globals.h"

void InitializeTextureSystem(void) {
    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        gSdlTextures[i] = NULL;
        gSdlTextureWidths[i] = 0;
        gSdlTextureHeights[i] = 0;
    }
}
