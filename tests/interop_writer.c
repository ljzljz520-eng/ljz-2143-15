/* Tiny harness to exercise the C ConfigStore against the Python store on a
 * shared directory: device-scoped writes only here. */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "geometry.h"

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    ConfigStore *s = config_open(argv[1], "dev-1", NULL);
    if (!s) return 3;

    if (strcmp(argv[2], "c-init") == 0) {
        Monitor mons[2] = {
            {"eDP", 0, 0, 1920, 1080, true},
            {"HDMI", 1920, 0, 1920, 1080, false},
        };
        char fp[GEOM_FP_MAX];
        geometry_fingerprint(mons, 2, fp, sizeof(fp));
        config_set_local_int(s, "window.x", 2100);
        config_set_local_int(s, "window.y", 60);
        config_set_local_int(s, "window.width", 1600);
        config_set_local_int(s, "window.height", 900);
        config_set_local_string(s, "displays.fingerprint", fp);
    } else if (strcmp(argv[2], "c-verify") == 0) {
        long long x = 0;
        config_get_int(s, "window.x", -1, &x);
        if (x != 2100) { fprintf(stderr, "lost window.x: %lld\n", x); return 4; }
        const char *accent = NULL;
        if (!config_get_string(s, "theme.accent", &accent)
            || strcmp(accent, "#cafe00") != 0) {
            fprintf(stderr, "C did not see Python synced accent\n");
            return 5;
        }
        const char *lang = NULL;
        if (!config_get_string(s, "ui.language", &lang)
            || strcmp(lang, "de") != 0) {
            fprintf(stderr, "C did not see Python session pref\n");
            return 6;
        }
    }
    config_close(s);
    return 0;
}
