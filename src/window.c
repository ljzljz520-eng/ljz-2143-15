#include "window.h"

#include <stdio.h>

bool window_init(AppWindow *app, const char *title, Rect rect) {
    if (app == NULL) {
        fprintf(stderr, "window_init: app is NULL\n");
        return false;
    }

    app->window = NULL;
    app->renderer = NULL;
    app->rect = rect;

    int x = rect.x < 0 ? SDL_WINDOWPOS_CENTERED : rect.x;
    int y = rect.y < 0 ? SDL_WINDOWPOS_CENTERED : rect.y;

    app->window = SDL_CreateWindow(
        title, x, y, rect.w, rect.h, SDL_WINDOW_SHOWN);
    if (app->window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    app->renderer = SDL_CreateRenderer(
        app->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (app->renderer == NULL) {
        // Xvfb often has no hardware acceleration; software fallback required.
        app->renderer = SDL_CreateRenderer(app->window, -1,
                                           SDL_RENDERER_SOFTWARE);
    }
    if (app->renderer == NULL) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        window_destroy(app);
        return false;
    }

    int wx, wy, ww, wh;
    SDL_GetWindowPosition(app->window, &wx, &wy);
    SDL_GetWindowSize(app->window, &ww, &wh);
    app->rect = (Rect){wx, wy, ww, wh};
    return true;
}

bool window_apply_rect(AppWindow *app, Rect rect) {
    if (!app || !app->window) return false;
    SDL_SetWindowPosition(app->window, rect.x, rect.y);
    SDL_SetWindowSize(app->window, rect.w, rect.h);
    app->rect = rect;
    return true;
}

Rect window_get_rect(const AppWindow *app) {
    if (!app || !app->window) return app ? app->rect : (Rect){0, 0, 0, 0};
    int x, y, w, h;
    SDL_GetWindowPosition(app->window, &x, &y);
    SDL_GetWindowSize(app->window, &w, &h);
    return (Rect){x, y, w, h};
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
