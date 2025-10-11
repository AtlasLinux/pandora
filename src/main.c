#include "acl.h"
#include <stdio.h>

int main(int argc, char** argv) {
    AclBlock* root = acl_parse_file(argv[1]);
    acl_print(root, stdout);
}