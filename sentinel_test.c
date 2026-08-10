#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "echo \"%s\"", argv[1]);
    system(cmd);
    return 0;
}
