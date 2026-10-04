/* 最小 SDL2_image 桩头：仅用于无 SDL 环境下的语法/类型检查。 */
#ifndef SDL_IMAGE_H_STUB
#define SDL_IMAGE_H_STUB

#include "SDL.h"

#define IMG_INIT_JPG 0x00000001
#define IMG_INIT_PNG 0x00000002

int IMG_Init(int flags);
void IMG_Quit(void);
SDL_Texture *IMG_LoadTexture(SDL_Renderer *renderer, const char *file);
const char *IMG_GetError(void);

#endif
