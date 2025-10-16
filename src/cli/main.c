#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "fs/fs.h"
#include "util/err.h"
#include "cli/cli.h"
#include "net/download.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Missing arguments");
        exit(1);
    }
    if (strcmp(argv[1], "init") == 0) {
        return (int)fs_init();
    } else if (strcmp(argv[1], "help") == 0) {
        return (int)cli_help();
    } else if (strcmp(argv[1], "fetch") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Missing arguments");
            exit(1);
        }
        return (int)fetch_package(argv[2], argv[3]);
    }
}