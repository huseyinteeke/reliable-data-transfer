/* sender - BLG337E Project 1 (stub, to be implemented) */
#include <stdio.h>
#include "config.h"

int main(int argc, char *argv[])
{
    for (int i = 0; i < argc; i++)
        fprintf(stderr, "[sender] argv[%d] = %s\n", i, argv[i]);
    return 0;
}
