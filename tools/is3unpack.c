/* Command-line InstallShield 3 extractor: is3unpack archive.Z outdir [-l] */
#include "../runtime/is3.h"
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s archive.Z outdir [-l]\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz); if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); return 1; } fclose(f);
    int r = is3_extract_mem(buf, (size_t)sz, argv[2], argc > 3 && !strcmp(argv[3], "-l"));
    if (r < 0) { fprintf(stderr, "bad signature\n"); return 1; }
    return r ? 2 : 0;
}
