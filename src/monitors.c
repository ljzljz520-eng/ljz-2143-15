#include "monitors.h"

#include <SDL2/SDL.h>

#include <stdio.h>

int monitors_enumerate(Monitor *out, int max) {
    if (!out || max <= 0) return 0;
    int n = SDL_GetNumVideoDisplays();
    if (n < 0) {
        fprintf(stderr, "SDL_GetNumVideoDisplays failed: %s\n",
                SDL_GetError());
        return 0;
    }
    int count = 0;
    for (int i = 0; i < n && count < max; ++i) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(i, &bounds) != 0) {
            fprintf(stderr, "SDL_GetDisplayBounds(%d) failed: %s\n",
                    i, SDL_GetError());
            continue;
        }
        static const char *names[16];
        static char storage[16][16];
        snprintf(storage[count], sizeof(storage[count]), "display-%d", i);
        names[count] = storage[count];
        out[count].id = names[count];
        out[count].x = bounds.x;
        out[count].y = bounds.y;
        out[count].w = bounds.w;
        out[count].h = bounds.h;
        out[count].primary = (i == 0);
        count++;
    }
    return count;
}
