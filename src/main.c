#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "fs.h"
#include "err.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Missing arguments");
        exit(1);
    }
    if (strcmp(argv[1], "init") == 0) {
        return (int)fs_init();
    }
}