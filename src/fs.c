#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "fs.h"
#include "err.h"

error_t fs_init(void) {
    struct stat st = {0};

    char* target_dirs[] = {
        "/pandora",
        "/pandora/store",
        "/pandora/vir",
        "/pandora/profiles",
        "/pandora/profiles/default",
        "/pandora/manifests",
        "/pandora/cache",
        "/pandora/tmp",
        NULL
    };
    
    for (int i = 0; target_dirs[i] != NULL; i++) {
        char* home_dir = strdup(getenv("HOME"));

        char* target_dir = strcat(home_dir, target_dirs[i]);
        if (stat(target_dir, &st) == -1) {
            printf("Creating dir %s\n", target_dir);
            if (mkdir(target_dir, 0755) == -1) {
                fprintf(stderr, "Failed to create dir %s\n", target_dir);
                exit(1);
            }
        }
    }

    return ERR_OK;
}