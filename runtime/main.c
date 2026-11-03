// Entry point: map the PE image into guest memory, set up the Win32 environment and run the
// recompiled CRT entry (which calls WinMain).
#include "win32.h"
#include "launcher.h"
#include "../gen/image_info.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>   // maps main() to WinMain on Windows
#include <sys/stat.h>

static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s (run `make gen` first)", path);
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2)
        if (fread(MEM + hdr[0], 1, hdr[1], f) != hdr[1]) rt_fatal("short image");
    fclose(f);
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

// icarus.cfg (in the working folder): KEY = value lines become ICARUS_KEY defaults; real
// environment variables still win. Written with commented defaults on first run.
static const char CFG_DEFAULT[] =
    "# Icarus settings. Remove the # in front of a line to change it. Environment variables\n"
    "# ICARUS_<NAME> override this file.\n"
    "\n"
    "# Window size as a multiple of 640x480, and start in fullscreen (F11 / Alt+Enter toggle it)\n"
    "#SCALE = 2\n#FULLSCREEN = 0\n"
    "# sharp (crisp, even pixels at any size), nearest, smooth, integer\n"
    "#SCALE_MODE = sharp\n"
    "# Dialogue speed: 1 = original, 2 = twice as fast, 0 = whole lines at once\n"
    "#TEXT_SPEED = 2\n"
    "# Font: replacement (anti-aliased) or original\n"
    "#FONT = replacement\n"
    "# Use the game's cursor as the system cursor (no lag). 0 = draw it in the game frame\n"
    "#HWCURSOR = 1\n"
    "# Frame rate: by default the game follows the display refresh (as on 1998 hardware)\n"
    "#FPS = 60\n"
    "#MUTE = 0\n";
static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { f = fopen(path, "w"); if (f) { fputs(CFG_DEFAULT, f); fclose(f); } return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        char key[64], val[160]; int k = 0;
        for (char *q = p; q < eq && k < 63; q++) if (*q != ' ' && *q != '\t') key[k++] = (char)(*q >= 'a' && *q <= 'z' ? *q - 32 : *q);
        key[k] = 0;
        char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        snprintf(val, sizeof val, "%s", v);
        for (int n = (int)strlen(val); n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' '); n--) val[n - 1] = 0;
        char name[80]; snprintf(name, sizeof name, "ICARUS_%s", key);
        if (key[0] && !getenv(name)) SDL_setenv_unsafe(name, val, 0);
    }
    fclose(f);
}

static char img_path[1100];
// game data is usable: an installed folder (launcher / make data) and a memory image for it
static int data_ready(void) {
    if (!is_dir(game_dir)) return 0;
    char p[1100]; snprintf(p, sizeof p, "%s/icarus.img", game_dir);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); snprintf(img_path, sizeof img_path, "%s", p); }
    else {   // developer layout: data/ from `make data`, image from `make gen`
        snprintf(p, sizeof p, "%s/ICARUS.EXE", game_dir);
        FILE *e = fopen(p, "rb"); FILE *g = fopen("gen/image.bin", "rb");
        int ok = e && g;
        if (e) fclose(e); if (g) fclose(g);
        if (!ok) return 0;
        snprintf(img_path, sizeof img_path, "gen/image.bin");
    }
    snprintf(p, sizeof p, "%s/cd", game_dir);
    return is_dir(p);
}

int main(int argc, char **argv) {
    SDL_SetAppMetadata("Icarus - Recomp by Second Save", "1.0", "com.secondsave.icarus");
    SDL_Init(SDL_INIT_EVENTS);
    // Folders. Developer layout: ./data, ./save and ./icarus.cfg in the working folder (when
    // ./data exists). Otherwise everything lives in the per-user folder:
    //   macOS ~/Library/Application Support/Second Save/Icarus, Windows %APPDATA%\Second Save\Icarus,
    //   Linux ~/.local/share/Second Save/Icarus. ICARUS_DATA / ICARUS_SAVE / ICARUS_CONFIG override.
    int dev = !getenv("ICARUS_DATA") && is_dir("data");
    char *pref = dev ? 0 : SDL_GetPrefPath("Second Save", "Icarus");
    char cfg[1100];
    if (getenv("ICARUS_CONFIG")) snprintf(cfg, sizeof cfg, "%s", getenv("ICARUS_CONFIG"));
    else snprintf(cfg, sizeof cfg, "%sicarus.cfg", pref ? pref : "");
    load_config(cfg);
    rt_verbose = getenv("ICARUS_VERBOSE") != 0;
    w32_log_api = getenv("ICARUS_TRACE") != 0;
    if (getenv("ICARUS_DATA")) snprintf(game_dir, sizeof game_dir, "%s", getenv("ICARUS_DATA"));
    else snprintf(game_dir, sizeof game_dir, "%sdata", pref ? pref : "");
    if (getenv("ICARUS_SAVE")) snprintf(save_dir, sizeof save_dir, "%s", getenv("ICARUS_SAVE"));
    else snprintf(save_dir, sizeof save_dir, "%ssave", pref ? pref : "");
    snprintf(cd_dir, sizeof cd_dir, "%s/cd", game_dir);
    if (pref) SDL_free(pref);

    // Launcher: shown unless headless, --play, or LAUNCHER = 0 in the settings. It is also shown
    // when the game data is missing (that is where the discs are picked).
    int headless = getenv("ICARUS_HEADLESS") && atoi(getenv("ICARUS_HEADLESS"));
    int play_arg = 0; for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--play")) play_arg = 1;
    const char *lv = getenv("ICARUS_LAUNCHER");
    int launcher_off = lv && !strcmp(lv, "0");
    int ready = data_ready();
    if (!headless && !launcher_off && (!play_arg || !ready)) {
        if (launcher_run(game_dir, save_dir, cfg, data_ready) != LAUNCH_PLAY) { SDL_Quit(); return 0; }
        rt_verbose = getenv("ICARUS_VERBOSE") != 0;
        ready = data_ready();
    }
    if (!ready) rt_fatal("game data folder '%s' not found (run the launcher and install from your discs, or see README)", game_dir);

    MEM = calloc(1, MEM_SIZE + 16);
    load_image(getenv("ICARUS_IMAGE") ? getenv("ICARUS_IMAGE") : img_path);
    w32_init(argc, argv);
    audio_init();
    ESP = STACK_HI - 16;
    PUSH32(0);
    uint32_t args[1] = { 0 };
    guest_call(GUEST_ENTRY, 0, args);
    fprintf(stderr, "guest returned from entry\n");
    w32_shutdown();
    return 0;
}

void w32_shutdown(void) {
    audio_shutdown();
    video_shutdown();
    SDL_Quit();
}
void movie_play(const char *path) { LOG("movie_play %s (not implemented yet)\n", path); }
