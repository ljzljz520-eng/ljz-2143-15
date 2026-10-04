#include "window.h"

#include <stdio.h>

bool window_init(AppWindow *app, const char *title, int x, int y, int width, int height) {
    if (app == NULL) {
        fprintf(stderr, "window_init: app is NULL\n");
        return false;
    }

    app->window = NULL;
    app->renderer = NULL;
    app->width = width;
    app->height = height;

    int pos_x = (x >= 0) ? x : SDL_WINDOWPOS_CENTERED;
    int pos_y = (y >= 0) ? y : SDL_WINDOWPOS_CENTERED;

    app->window = SDL_CreateWindow(
        title,
        pos_x,
        pos_y,
        width,
        height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (app->window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    app->renderer = SDL_CreateRenderer(
        app->window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (app->renderer == NULL) {
        // Xvfb often has no hardware acceleration; software fallback is required.
        app->renderer = SDL_CreateRenderer(app->window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (app->renderer == NULL) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        window_destroy(app);
        return false;
    }

    return true;
}

void window_get_geometry(const AppWindow *app, int *x, int *y, int *width, int *height) {
    if (app == NULL || app->window == NULL) {
        return;
    }
    if (x != NULL && y != NULL) {
        SDL_GetWindowPosition(app->window, x, y);
    }
    if (width != NULL && height != NULL) {
        SDL_GetWindowSize(app->window, width, height);
    }
}

void window_set_geometry(AppWindow *app, int x, int y, int width, int height) {
    if (app == NULL || app->window == NULL) {
        return;
    }
    SDL_SetWindowPosition(app->window, x, y);
    SDL_SetWindowSize(app->window, width, height);
    app->width = width;
    app->height = height;
}

void window_destroy(AppWindow *app) {
    if (app == NULL) {
        return;
    }

    if (app->renderer != NULL) {
        SDL_DestroyRenderer(app->renderer);
        app->renderer = NULL;
    }

    if (app->window != NULL) {
        SDL_DestroyWindow(app->window);
        app->window = NULL;
    }
}
