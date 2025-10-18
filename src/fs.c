#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

void create_dir(char* dir, int mode) {
    struct stat st = {0};
    if (stat(dir, &st) == -1) {
        if (mkdir(dir, mode) == -1) {
            perror("mkdir");
            exit(1);
        }
    }
}

int pandora_fs_init() {
    char* dirs[] = {
        "cache",
        "store",
        "tmp",
        "manifests",
        "locks",
        NULL
    };
    char* home = strdup(getenv("HOME"));
    create_dir(strcat(home, "/.pandora/"), 0755);
    for (int i = 0; dirs[i] != NULL; i++) {
        char* dir = strdup(home);
        dir = strcat(dir, dirs[i]);
        create_dir(dir, 0755);
        free(dir);
    }
    free(home);
    return 0;
}