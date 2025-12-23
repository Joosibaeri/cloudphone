#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("CloudPhone GUI stub\n");
    printf("GTK not available or dev headers missing.\n");
    printf("To enable full GUI build, install: pkgconf libgtk-3-dev\n");
    printf("Usage: run ./main or install GTK and rebuild gui.c to get full GUI.\n");
    return 0;
}
