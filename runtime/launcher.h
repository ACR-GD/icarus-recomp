// Launcher window (runtime/launcher.c).
#pragma once
enum { LAUNCH_QUIT = 0, LAUNCH_PLAY = 1 };
// Shows the launcher; returns LAUNCH_PLAY or LAUNCH_QUIT. data_ready() reports whether the game
// data is installed. Settings are written to cfg_path and exported as ICARUS_* variables on Play.
int launcher_run(const char *data_dir, const char *save_dir, const char *cfg_path, int (*data_ready)(void));
