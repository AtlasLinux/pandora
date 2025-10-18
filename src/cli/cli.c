#include "cli/cli.h"
#include <stdio.h>
#include <stdlib.h>

error_t cli_help(void) {
    fprintf(stderr, 
        "usage: pandora <command>\n"
        "\n"
        "Commands:\n"
        "\tinit\t" "Initialises pandora\n"
        "\thelp\t" "Displays this message then quits\n"
    );
    exit(0);
}