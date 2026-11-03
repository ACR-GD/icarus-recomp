// Launcher installer: rebuild the game data folder from the player's disc images.
#pragma once
struct install_progress {
    volatile float frac;     // 0..1
    char msg[256];           // current step
    char err[512];           // set on failure
    char warn[256];          // set on success with caveats (e.g. no music)
    volatile int done, ok;
};
int install_game(const char *cd1, const char *cd2, const char *datadir, struct install_progress *p);
int install_is_complete(const char *datadir);
