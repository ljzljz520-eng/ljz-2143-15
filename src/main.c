/* Visual Window App —— C 终端 GUI。
 *
 * 窗口位置/尺寸（机器相关，设备私有）与背景（用户级，可跨设备同步）
 * 接入状态同步系统：
 *   启动  —— 读取本地恢复记录（损坏自动隔离诊断副本并回退备份）
 *   运行  —— 移动/缩放实时写入恢复记录（防抖），周期性后台同步
 *   恢复  —— 按当前显示器拓扑钳制窗口，保证恢复后窗口可见可操作
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "cjson.h"
#include "config.h"
#include "renderer.h"
#include "state_file.h"
#include "sync_client.h"
#include "window.h"

#define WINDOW_TITLE "Visual Window App"
#define SYNC_INTERVAL_MS 5000
#define SAVE_DEBOUNCE_MS 500

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

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* 从 SDL 显示器拓扑刷新本机显示器列表（机器相关，不参与同步）。 */
static void refresh_monitors(AppState *st) {
    JVal *arr = j_new(J_ARR);
    int n = SDL_GetNumVideoDisplays();
    for (int i = 0; i < n; i++) {
        SDL_Rect r;
        if (SDL_GetDisplayBounds(i, &r) == 0) {
            JVal *m = j_new(J_OBJ);
            j_obj_set(m, "x", j_num((double)r.x));
            j_obj_set(m, "y", j_num((double)r.y));
            j_obj_set(m, "w", j_num((double)r.w));
            j_obj_set(m, "h", j_num((double)r.h));
            j_arr_push(arr, m);
        }
    }
    j_free(st->monitors);
    st->monitors = arr;
}

static void doc_geometry(const AppState *st, int *x, int *y, int *w, int *h) {
    JVal *win = j_get_path(st->doc, "machine.window");
    *x = (int)j_as_num(j_obj_get(win, "x"), 100);
    *y = (int)j_as_num(j_obj_get(win, "y"), 100);
    *w = (int)j_as_num(j_obj_get(win, "width"), 1024);
    *h = (int)j_as_num(j_obj_get(win, "height"), 768);
}

/* 钳制窗口到当前显示器拓扑；有调整时同步到 SDL 窗口并落盘。 */
static void clamp_and_apply(AppState *st, AppWindow *app, const char *state_dir) {
    char note[256] = {0};
    if (cfg_clamp_window(st->doc, st->monitors, note, sizeof note) == 0) {
        return;
    }
    fprintf(stderr, "state: %s\n", note);
    state_log_recovery(state_dir, "%s", note);
    int gx, gy, gw, gh;
    doc_geometry(st, &gx, &gy, &gw, &gh);
    window_set_geometry(app, gx, gy, gw, gh);
    JVal *win = j_get_path(st->doc, "machine.window");
    state_local_set(st, "machine.window.x", j_obj_get(win, "x"));
    state_local_set(st, "machine.window.y", j_obj_get(win, "y"));
    state_local_set(st, "machine.window.width", j_obj_get(win, "width"));
    state_local_set(st, "machine.window.height", j_obj_get(win, "height"));
}

int main(void) {
    const char *state_dir = env_or("APP_STATE_DIR", ".");
    const char *server = env_or("APP_SERVER", "http://127.0.0.1:8000");
    char host[128] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0) {
        snprintf(host, sizeof(host), "dev-unknown");
    }
    const char *device = env_or("APP_DEVICE_ID", host);
    const char *user_env = getenv("USER");
    const char *user = env_or("APP_USER", user_env != NULL ? user_env : "user-unknown");

    AppState st;
    state_init(&st, device, user, CFG_SCHEMA_VERSION);
    char serr[512] = {0};
    int load_rc = state_load(&st, state_dir, serr, sizeof serr);
    if (load_rc != 0) {
        fprintf(stderr, "state: %s\n", serr);
    }

    if (!init_sdl()) {
        state_free(&st);
        return 1;
    }

    refresh_monitors(&st);
    clamp_and_apply(&st, NULL, state_dir);

    int gx, gy, gw, gh;
    doc_geometry(&st, &gx, &gy, &gw, &gh);

    AppWindow app = {0};
    if (!window_init(&app, WINDOW_TITLE, gx, gy, gw, gh)) {
        shutdown_sdl();
        state_free(&st);
        return 1;
    }

    char current_bg[512];
    snprintf(current_bg, sizeof current_bg, "%s",
             j_as_str(j_get_path(st.doc, "prefs.background"), "assets/background.png"));

    SceneRenderer scene = {0};
    if (!renderer_load_background(&scene, app.renderer, current_bg)) {
        window_destroy(&app);
        shutdown_sdl();
        state_free(&st);
        return 1;
    }

    Uint32 last_sync = SDL_GetTicks();
    Uint32 last_save = 0;
    bool dirty = false;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    running = false;
                } else if (event.window.event == SDL_WINDOWEVENT_MOVED) {
                    JVal vx = { .type = J_NUM, .num = (double)event.window.data1 };
                    JVal vy = { .type = J_NUM, .num = (double)event.window.data2 };
                    state_local_set(&st, "machine.window.x", &vx);
                    state_local_set(&st, "machine.window.y", &vy);
                    dirty = true;
                } else if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    app.width = event.window.data1;
                    app.height = event.window.data2;
                    JVal vw = { .type = J_NUM, .num = (double)event.window.data1 };
                    JVal vh = { .type = J_NUM, .num = (double)event.window.data2 };
                    state_local_set(&st, "machine.window.width", &vw);
                    state_local_set(&st, "machine.window.height", &vh);
                    dirty = true;
                }
            }
        }

        Uint32 now = SDL_GetTicks();
        if (dirty && now - last_save >= SAVE_DEBOUNCE_MS) {
            char err[256] = {0};
            if (state_save(&st, state_dir, err, sizeof err) != 0) {
                fprintf(stderr, "state save failed: %s\n", err);
            }
            last_save = now;
            dirty = false;
        }

        if (now - last_sync >= SYNC_INTERVAL_MS) {
            refresh_monitors(&st);
            clamp_and_apply(&st, &app, state_dir);
            char log[1024];
            if (sync_now(&st, server, log, sizeof log) == SYNC_OK) {
                /* 后台换图：热重载背景纹理 */
                const char *nbg = j_as_str(j_get_path(st.doc, "prefs.background"), current_bg);
                if (strcmp(nbg, current_bg) != 0 &&
                    renderer_load_background(&scene, app.renderer, nbg)) {
                    snprintf(current_bg, sizeof current_bg, "%s", nbg);
                }
                dirty = true; /* 持久化同步后的状态 */
            }
            last_sync = now;
        }

        renderer_draw_background(&scene, app.renderer, app.width, app.height);
        SDL_Delay(16);
    }

    {
        char err[256] = {0};
        if (state_save(&st, state_dir, err, sizeof err) != 0) {
            fprintf(stderr, "state save failed: %s\n", err);
        }
    }
    renderer_destroy(&scene);
    window_destroy(&app);
    shutdown_sdl();
    state_free(&st);
    return 0;
}
