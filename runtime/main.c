// Entry point: map the PE image into guest memory, set up the Win32 environment and run the
// recompiled CRT entry (which calls WinMain).
#include "win32.h"
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

int main(int argc, char **argv) {
    rt_verbose = getenv("ICARUS_VERBOSE") != 0;
    w32_log_api = getenv("ICARUS_TRACE") != 0;
    // data location: ICARUS_DATA, or ./data next to the current directory, or next to the binary
    const char *data = getenv("ICARUS_DATA");
    char buf[1024];
    if (!data) {
        if (is_dir("data")) data = "data";
        else {
            const char *base = SDL_GetBasePath();
            snprintf(buf, sizeof buf, "%sdata", base ? base : "");
            data = buf;
        }
    }
    snprintf(game_dir, sizeof game_dir, "%s", data);
    snprintf(cd_dir, sizeof cd_dir, "%s/cd", data);
    snprintf(save_dir, sizeof save_dir, "%s", getenv("ICARUS_SAVE") ? getenv("ICARUS_SAVE") : "save");
    if (!is_dir(game_dir)) rt_fatal("game data folder '%s' not found (see README: tools/prepare_data.sh)", game_dir);

    MEM = calloc(1, MEM_SIZE + 16);
    char img[1100]; snprintf(img, sizeof img, "%s/icarus.img", game_dir);
    FILE *probe = fopen(img, "rb"); if (probe) fclose(probe);
    load_image(getenv("ICARUS_IMAGE") ? getenv("ICARUS_IMAGE") : (probe ? img : "gen/image.bin"));
    SDL_SetAppMetadata("Icarus", "1.0", "icarus.recomp");
    SDL_Init(SDL_INIT_EVENTS);
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
