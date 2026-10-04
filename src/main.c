#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "config.h"
#include "config_registry.h"
#include "geometry.h"
#include "monitors.h"
#include "renderer.h"
#include "window.h"

#define WINDOW_TITLE "Visual Window App"
#define MAX_MONITORS 16
#define STATE_POLL_MS 500
#define MOVE_DEBOUNCE_MS 300

static const char *state_dir(void) {
    const char *d = getenv("VW_STATE_DIR");
    if (d && *d) return d;
    static char buf[1024];
    const char *home = getenv("HOME");
    snprintf(buf, sizeof(buf), "%s/.local/state/visual-window",
             home ? home : "/tmp");
    return buf;
}

static bool init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
    int initted = IMG_Init(img_flags);
    if ((initted & img_flags) == 0) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return false;
    }
    return true;
}

static void shutdown_sdl(void) {
    IMG_Quit();
    SDL_Quit();
}

/* Read window rect from the config store and repair it against live displays. */
static Rect load_and_repair_rect(ConfigStore *cfg, Monitor *mons, int *nmon) {
    int count = monitors_enumerate(mons, MAX_MONITORS);
    *nmon = count;
    long long x, y, w, h;
    config_get_int(cfg, "window.x", -1, &x);
    config_get_int(cfg, "window.y", -1, &y);
    config_get_int(cfg, "window.width", 1280, &w);
    config_get_int(cfg, "window.height", 720, &h);
    Rect saved = {(int)x, (int)y, (int)w, (int)h};
    GeomResult repaired = geometry_repair(saved, mons, count);
    return repaired.rect;
}

static void persist_fingerprint_and_rect(ConfigStore *cfg, const Monitor *mons,
                                         int count, Rect rect,
                                         const char *reason) {
    char fp[GEOM_FP_MAX];
    geometry_fingerprint(mons, count, fp, sizeof(fp));
    config_set_local_string(cfg, "displays.fingerprint", fp);
    config_set_local_int(cfg, "window.x", rect.x);
    config_set_local_int(cfg, "window.y", rect.y);
    config_set_local_int(cfg, "window.width", rect.w);
    config_set_local_int(cfg, "window.height", rect.h);
    printf("display repair (%s): window -> x=%d y=%d %dx%d fp=%s\n",
           reason, rect.x, rect.y, rect.w, rect.h, fp);
}

/* Reload the background texture when the configured URI changes. Local file
 * paths are honoured; http(s) URIs are expected to be materialised by the
 * sync agent. A missing file never blocks the window. */
static bool background_current(const char *uri) {
    if (!uri) return true;
    if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0)
        return true;  /* agent-provided remote; texture management out of scope */
    struct stat st;
    return stat(uri, &st) == 0;
}

static void try_reload_background(SceneRenderer *scene, SDL_Renderer *r,
                                  ConfigStore *cfg, char *current_uri,
                                  size_t uri_cap) {
    const char *uri = NULL;
    config_get_string(cfg, "theme.background.uri", &uri);
    if (!uri) return;
    if (strcmp(uri, current_uri) == 0) return;
    if (!background_current(uri)) return;
    if (scene->background) renderer_destroy(scene);
    if (renderer_load_background(scene, r, uri))
        snprintf(current_uri, uri_cap, "%s", uri);
}

int main(void) {
    if (!init_sdl()) return 1;

    ConfigHooks hooks = {0};
    ConfigStore *cfg = config_open(state_dir(),
                                   (getenv("VW_DEVICE_ID") ? getenv("VW_DEVICE_ID") : "device-local"),
                                   &hooks);
    if (!cfg) {
        fprintf(stderr, "failed to open configuration state\n");
        shutdown_sdl();
        return 1;
    }

    Monitor mons[MAX_MONITORS];
    int nmon = 0;
    Rect rect = load_and_repair_rect(cfg, mons, &nmon);

    AppWindow app = {0};
    if (!window_init(&app, WINDOW_TITLE, rect)) {
        config_close(cfg);
        shutdown_sdl();
        return 1;
    }
    /* The created window (possibly centered by SDL) becomes the durable rect. */
    Rect actual = window_get_rect(&app);
    persist_fingerprint_and_rect(cfg, mons, nmon, actual, "startup");

    SceneRenderer scene = {0};
    const char *bg_uri = NULL;
    config_get_string(cfg, "theme.background.uri", &bg_uri);
    char current_uri[512] = {0};
    if (bg_uri && background_current(bg_uri)) {
        renderer_load_background(&scene, app.renderer, bg_uri);
        snprintf(current_uri, sizeof(current_uri), "%s", bg_uri);
    }

    bool running = true;
    Uint32 last_persist = 0;
    Uint32 last_poll = 0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (event.type == SDL_QUIT
                || (event.type == SDL_WINDOWEVENT
                    && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT
                       && (event.window.event == SDL_WINDOWEVENT_MOVED
                           || event.window.event
                              == SDL_WINDOWEVENT_SIZE_CHANGED
                           || event.window.event
                              == SDL_WINDOWEVENT_DISPLAY_CHANGED)) {
                last_persist = SDL_GetTicks();  /* debounce */
            } else if (event.type == SDL_WINDOWEVENT
                       && (event.window.event
                           == SDL_WINDOWEVENT_RESIZED)) {
                last_persist = SDL_GetTicks();
            }
        }

        Uint32 now = SDL_GetTicks();

        /* Persist debounced user moves/resizes as device-scoped values. */
        if (last_persist != 0 && now - last_persist > MOVE_DEBOUNCE_MS) {
            Rect cur = window_get_rect(&app);
            config_set_local_int(cfg, "window.x", cur.x);
            config_set_local_int(cfg, "window.y", cur.y);
            config_set_local_int(cfg, "window.width", cur.w);
            config_set_local_int(cfg, "window.height", cur.h);
            last_persist = 0;
        }

        /* Detect hotplug: recompute fingerprint and repair the window. */
        if (now - last_poll > STATE_POLL_MS) {
            last_poll = now;
            Monitor live[MAX_MONITORS];
            int nlive = monitors_enumerate(live, MAX_MONITORS);
            char new_fp[GEOM_FP_MAX], old_fp[GEOM_FP_MAX] = "";
            geometry_fingerprint(live, nlive, new_fp, sizeof(new_fp));
            const char *old = NULL;
            config_get_string(cfg, "displays.fingerprint", &old);
            if (old) snprintf(old_fp, sizeof(old_fp), "%s", old);
            if (strcmp(new_fp, old_fp) != 0) {
                Rect cur = window_get_rect(&app);
                GeomResult r = geometry_repair(cur, live, nlive);
                window_apply_rect(&app, r.rect);
                persist_fingerprint_and_rect(cfg, live, nlive, r.rect,
                                             "hotplug");
            }
            try_reload_background(&scene, app.renderer, cfg,
                                  current_uri, sizeof(current_uri));
        }

        renderer_draw_background(&scene, app.renderer, app.rect.w,
                                 app.rect.h);
        SDL_Delay(16);
    }

    Rect final_rect = window_get_rect(&app);
    config_set_local_int(cfg, "window.x", final_rect.x);
    config_set_local_int(cfg, "window.y", final_rect.y);
    config_set_local_int(cfg, "window.width", final_rect.w);
    config_set_local_int(cfg, "window.height", final_rect.h);

    renderer_destroy(&scene);
    window_destroy(&app);
    config_close(cfg);
    shutdown_sdl();
    return 0;
}
