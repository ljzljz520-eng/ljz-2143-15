#ifndef WINDOW_H
#define WINDOW_H

#include <stdbool.h>
#include <SDL2/SDL.h>

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int width;
    int height;
} AppWindow;

/* x/y 传负数表示居中（SDL_WINDOWPOS_CENTERED）。 */
bool window_init(AppWindow *app, const char *title, int x, int y, int width, int height);
void window_get_geometry(const AppWindow *app, int *x, int *y, int *width, int *height);
void window_set_geometry(AppWindow *app, int x, int y, int width, int height);
void window_destroy(AppWindow *app);

#endif
