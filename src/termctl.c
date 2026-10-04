/* termctl —— C 终端命令行客户端（无 SDL 依赖，便于无头环境与自动化测试）。
 *
 * 用法: termctl --dir D --server URL --device ID --user U [--schema N] <cmd> [args]
 *   init                      初始化本地恢复记录（内置兜底默认）
 *   show                      校验窗口可见性后打印生效配置（JSON）
 *   get FIELD                 打印字段值（JSON）
 *   set FIELD VALUE           本地修改字段（记入 journal）
 *   move X Y                  移动窗口
 *   resize W H                调整窗口尺寸
 *   monitors JSON             设置本机显示器拓扑 [{x,y,w,h},...] 并校验窗口
 *   validate                  重新校验窗口可见性
 *   sync                      与服务器同步（推送 journal + 拉取增量/快照）
 *   corrupt [--bak]           测试辅助：截断状态文件模拟写一半断电
 *   upload-diagnostics        上传损坏配置诊断副本
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cjson.h"
#include "config.h"
#include "state_file.h"
#include "sync_client.h"

static void usage(void) {
    fprintf(stderr,
        "usage: termctl --dir D --server URL --device ID --user U [--schema N] <cmd> [args]\n"
        "cmds: init|show|get|set|move|resize|monitors|validate|sync|corrupt|upload-diagnostics\n");
}

static int ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return 0;
    return mkdir(dir, 0755);
}

/* 加载或（首次）初始化状态 */
static int load_or_init(AppState *st, const char *dir, const char *device,
                        const char *user, int schema) {
    state_init(st, device, user, schema);
    char err[512] = {0};
    int rc = state_load(st, dir, err, sizeof err);
    if (rc != 0)
        fprintf(stderr, "note: %s\n", err);
    return rc;
}

/* 校验窗口可见性；有钳制则保存并记录 */
static void validate_window(AppState *st, const char *dir) {
    char note[256] = {0};
    if (cfg_clamp_window(st->doc, st->monitors, note, sizeof note)) {
        fprintf(stderr, "note: %s\n", note);
        state_log_recovery(dir, "%s", note);
        /* 钳制结果作为本机字段变更入 journal，让服务器存档最新可恢复位置 */
        JVal *win = j_get_path(st->doc, "machine.window");
        state_local_set(st, "machine.window.x", j_obj_get(win, "x"));
        state_local_set(st, "machine.window.y", j_obj_get(win, "y"));
        state_local_set(st, "machine.window.width", j_obj_get(win, "width"));
        state_local_set(st, "machine.window.height", j_obj_get(win, "height"));
        char err[256] = {0};
        state_save(st, dir, err, sizeof err);
    }
}

int main(int argc, char **argv) {
    const char *dir = ".";
    const char *server = "http://127.0.0.1:8000";
    const char *device = "dev-unknown";
    const char *user = "user-unknown";
    int schema = CFG_SCHEMA_VERSION;

    int i = 1;
    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        const char *opt = argv[i];
        if (i + 1 >= argc) { usage(); return 1; }
        if (strcmp(opt, "--dir") == 0) dir = argv[++i];
        else if (strcmp(opt, "--server") == 0) server = argv[++i];
        else if (strcmp(opt, "--device") == 0) device = argv[++i];
        else if (strcmp(opt, "--user") == 0) user = argv[++i];
        else if (strcmp(opt, "--schema") == 0) schema = atoi(argv[++i]);
        else { usage(); return 1; }
        i++;
    }
    if (i >= argc) { usage(); return 1; }
    const char *cmd = argv[i++];
    ensure_dir(dir);

    AppState st;
    int load_rc = load_or_init(&st, dir, device, user, schema);
    char err[512] = {0};
    int rc = 0;

    if (strcmp(cmd, "init") == 0) {
        if (state_save(&st, dir, err, sizeof err) != 0) {
            fprintf(stderr, "save failed: %s\n", err);
            rc = 1;
        } else {
            printf("{\"ok\":true,\"recovered\":%d}\n", st.recovered);
        }
    } else if (strcmp(cmd, "show") == 0) {
        validate_window(&st, dir);
        char *out = j_write(st.doc);
        puts(out ? out : "{}");
        free(out);
    } else if (strcmp(cmd, "get") == 0) {
        if (i >= argc) { usage(); rc = 1; goto done; }
        JVal *v = j_get_path(st.doc, argv[i]);
        char *out = v ? j_write(v) : strdup("null");
        puts(out);
        free(out);
    } else if (strcmp(cmd, "set") == 0) {
        if (i + 1 >= argc) { usage(); rc = 1; goto done; }
        const char *jerr = NULL;
        JVal *v = j_parse(argv[i + 1], &jerr);
        if (!v) v = j_str(argv[i + 1]); /* 非 JSON 按字符串处理 */
        state_local_set(&st, argv[i], v);
        j_free(v);
        if (state_save(&st, dir, err, sizeof err) != 0) {
            fprintf(stderr, "save failed: %s\n", err);
            rc = 1;
        } else {
            printf("{\"ok\":true,\"journal\":%d}\n", st.journal->len);
        }
    } else if (strcmp(cmd, "move") == 0 || strcmp(cmd, "resize") == 0) {
        if (i + 1 >= argc) { usage(); rc = 1; goto done; }
        double a = atof(argv[i]), b = atof(argv[i + 1]);
        JVal va = {.type = J_NUM, .num = a}, vb = {.type = J_NUM, .num = b};
        if (strcmp(cmd, "move") == 0) {
            state_local_set(&st, "machine.window.x", &va);
            state_local_set(&st, "machine.window.y", &vb);
        } else {
            state_local_set(&st, "machine.window.width", &va);
            state_local_set(&st, "machine.window.height", &vb);
        }
        if (state_save(&st, dir, err, sizeof err) != 0) {
            fprintf(stderr, "save failed: %s\n", err);
            rc = 1;
        } else {
            printf("{\"ok\":true,\"journal\":%d}\n", st.journal->len);
        }
    } else if (strcmp(cmd, "monitors") == 0) {
        if (i >= argc) { usage(); rc = 1; goto done; }
        const char *jerr = NULL;
        JVal *mons = j_parse(argv[i], &jerr);
        if (!mons || mons->type != J_ARR) {
            fprintf(stderr, "bad monitors json: %s\n", jerr);
            rc = 1;
            goto done;
        }
        j_free(st.monitors);
        st.monitors = mons;
        validate_window(&st, dir);
        if (state_save(&st, dir, err, sizeof err) != 0) {
            fprintf(stderr, "save failed: %s\n", err);
            rc = 1;
        } else {
            printf("{\"ok\":true}\n");
        }
    } else if (strcmp(cmd, "validate") == 0) {
        validate_window(&st, dir);
        JVal *win = j_get_path(st.doc, "machine.window");
        char *out = win ? j_write(win) : strdup("{}");
        puts(out);
        free(out);
    } else if (strcmp(cmd, "sync") == 0) {
        char log[2048];
        int src = sync_now(&st, server, log, sizeof log);
        fprintf(stderr, "%s", log);
        if (src == SYNC_OK) {
            if (state_save(&st, dir, err, sizeof err) != 0) {
                fprintf(stderr, "save failed: %s\n", err);
                rc = 1;
            } else {
                printf("{\"ok\":true,\"server_seq\":%ld,\"journal\":%d,\"recovered\":%d}\n",
                       st.server_seq, st.journal->len, st.recovered);
            }
        } else if (src == SYNC_OFFLINE) {
            printf("{\"ok\":false,\"offline\":true,\"journal\":%d}\n", st.journal->len);
            rc = 2;
        } else {
            fprintf(stderr, "sync protocol error\n");
            rc = 1;
        }
    } else if (strcmp(cmd, "corrupt") == 0) {
        /* 测试辅助：截断状态文件，模拟写一半断电 */
        int also_bak = (i < argc && strcmp(argv[i], "--bak") == 0);
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, STATE_FILE_NAME);
        FILE *f = fopen(path, "r+b");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            ftruncate(fileno(f), size / 2);
            fclose(f);
        }
        if (also_bak) {
            snprintf(path, sizeof path, "%s/%s", dir, STATE_BACKUP_NAME);
            f = fopen(path, "r+b");
            if (f) {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                ftruncate(fileno(f), size / 2);
                fclose(f);
            }
        }
        printf("{\"ok\":true}\n");
    } else if (strcmp(cmd, "upload-diagnostics") == 0) {
        char log[512];
        int n = sync_upload_diagnostics(&st, dir, server, log, sizeof log);
        fprintf(stderr, "%s\n", log);
        printf("{\"ok\":%s,\"uploaded\":%d}\n", n >= 0 ? "true" : "false", n);
        if (n < 0) rc = 2;
    } else {
        usage();
        rc = 1;
    }

done:
    (void)load_rc;
    state_free(&st);
    return rc;
}
