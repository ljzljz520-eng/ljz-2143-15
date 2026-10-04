#ifndef WINDOW_H
#define WINDOW_H

#include <stdbool.h>
#include <SDL2/SDL.h>

#include "geometry.h"

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    Rect rect;
} AppWindow;

/* Create a window at the given (already topology-repaired) rectangle.
 * Negative x/y are interpreted by SDL as centering on the primary display. */
bool window_init(AppWindow *app, const char *title, Rect rect);

/* Reposition/resize an existing window (after a hotplug repair). Returns
 * false if the SDL call fails; the window remains operable regardless. */
bool window_apply_rect(AppWindow *app, Rect rect);

Rect window_get_rect(const AppWindow *app);

void window_destroy(AppWindow *app);

#endif /* WINDOW_H */
