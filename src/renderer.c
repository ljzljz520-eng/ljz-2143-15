#include "renderer.h"

#include <stdio.h>

#include <SDL2/SDL_image.h>

bool renderer_load_background(
    SceneRenderer *scene,
    SDL_Renderer *renderer,
    const char *image_path
) {
    if (scene == NULL || renderer == NULL || image_path == NULL) {
        return false;
    }

    scene->background = IMG_LoadTexture(renderer, image_path);
    scene->background_failed = (scene->background == NULL);
    if (scene->background_failed) {
        // Not fatal: a missing/sync-pending background falls back to a colour.
        fprintf(
            stderr,
            "IMG_LoadTexture failed for %s: %s (using fallback colour)\n",
            image_path, IMG_GetError()
        );
    }
    return scene->background != NULL;
}

void renderer_draw_background(
    const SceneRenderer *scene,
    SDL_Renderer *renderer,
    int window_width,
    int window_height
) {
    SDL_Rect dst_rect = {
        .x = 0,
        .y = 0,
        .w = window_width,
        .h = window_height,
    };

    SDL_SetRenderDrawColor(renderer, 18, 24, 38, 255);
    SDL_RenderClear(renderer);

    if (scene != NULL && scene->background != NULL) {
        SDL_RenderCopy(renderer, scene->background, NULL, &dst_rect);
    }

    SDL_RenderPresent(renderer);
}

void renderer_destroy(SceneRenderer *scene) {
    if (scene == NULL) {
        return;
    }

    if (scene->background != NULL) {
        SDL_DestroyTexture(scene->background);
        scene->background = NULL;
    }
}
