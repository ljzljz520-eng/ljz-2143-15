/* 最小 SDL2 桩头：仅用于无 SDL 环境下的语法/类型检查，签名与 SDL2 一致。 */
#ifndef SDL_H_STUB
#define SDL_H_STUB

#include <stdint.h>

typedef uint8_t Uint8;
typedef uint32_t Uint32;
typedef int32_t Sint32;

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;

#define SDL_INIT_VIDEO 0x00000020u

#define SDL_WINDOW_SHOWN 0x00000004u
#define SDL_WINDOW_RESIZABLE 0x00000020u
#define SDL_WINDOWPOS_CENTERED_MASK 0x2FFF0000u
#define SDL_WINDOWPOS_CENTERED ((int)SDL_WINDOWPOS_CENTERED_MASK)

#define SDL_RENDERER_SOFTWARE 0x00000001u
#define SDL_RENDERER_ACCELERATED 0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u

#define SDL_QUIT 0x100
#define SDL_WINDOWEVENT 0x200
#define SDL_WINDOWEVENT_CLOSE 14
#define SDL_WINDOWEVENT_MOVED 4
#define SDL_WINDOWEVENT_RESIZED 5

typedef struct SDL_WindowEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 event;
    Uint8 padding1;
    Uint8 padding2;
    Uint8 padding3;
    Sint32 data1;
    Sint32 data2;
} SDL_WindowEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    Uint8 padding[56];
} SDL_Event;

int SDL_Init(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_Delay(Uint32 ms);
Uint32 SDL_GetTicks(void);

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);

void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
void SDL_SetWindowPosition(SDL_Window *window, int x, int y);
void SDL_SetWindowSize(SDL_Window *window, int w, int h);

int SDL_PollEvent(SDL_Event *event);
int SDL_GetNumVideoDisplays(void);
int SDL_GetDisplayBounds(int displayIndex, SDL_Rect *rect);

int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                   const SDL_Rect *srcrect, const SDL_Rect *dstrect);
void SDL_RenderPresent(SDL_Renderer *renderer);
void SDL_DestroyTexture(SDL_Texture *texture);

#endif
