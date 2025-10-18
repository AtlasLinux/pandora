#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "fs.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Not enough arguments");
        return 1;
    }
    if (strcmp(argv[1], "init") == 0) {
        fprintf(stderr, "Initialising: ");
        pandora_fs_init();
    }
    return 0;
}