#include <stdio.h>
#include <stdlib.h>

#include "fuses.h"

int main(int argc, char *argv[]) {
    FusesConfiguration config = {
        .rawData = NULL,  // TODO
        .rawDataSize = 0,  // TODO
        .busName = "/dev/i2c-1",
        .busNameLength = 11,
        .fuseDuration = 200,
        .timeResolution = 10
    };

    FusesObject fuses = fusesInit(&config);

    return EXIT_SUCCESS;
}
