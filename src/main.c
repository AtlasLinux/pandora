#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

typedef struct package_t {
    char* name;
    char* version;
    char* author;
    char* signature;
    struct package_t* deps;
    size_t num_deps
} Package;

void download(char* address, char* location) {
    char buffer[256];
    snprintf(buffer, 256, "curl %s %s -o %s", strtok(address, "/"), strtok(NULL, "/"), location);
    system(buffer);
}

void install(char* pkg) {
    download("raw.githubusercontent.com/atlaslinux/pandora/main/packages.db", "/tmp/pandora/packages.db");
}

int main(int argc, char** argv) {
    if (argc != 3)
        return 1;

    switch (argv[1][1]) {
        case 'S':
            install(argv[2]);
    }
}