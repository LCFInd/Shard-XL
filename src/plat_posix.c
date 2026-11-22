/* SPDX-License-Identifier: GPL-3.0-only */
/* The platform calls for the Linux test builds (and the Pi, later): WAV only, no folder picker. */
#include "xl.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int plat_decode(const char *path, float **inter, int *frames, int *rate, int *channels)
{
    (void)path; (void)inter; (void)frames; (void)rate; (void)channels;
    return -1;
}
int plat_choose_folder(char *out, int outlen) { (void)out; (void)outlen; return 0; }
void plat_mkdir(const char *path) { mkdir(path, 0755); }
int plat_list_dir(const char *path, char names[][256], int *isdir, int max)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) && n < max) {
        char full[2048];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) != 0) continue;
        snprintf(names[n], 256, "%s", e->d_name);
        isdir[n] = S_ISDIR(st.st_mode);
        n++;
    }
    closedir(d);
    return n;
}
double plat_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
