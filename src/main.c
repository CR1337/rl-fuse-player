#include <stdio.h>
#include <stdlib.h>

#include "fuses.h"

int main(int argc, char *argv[]) {
    char *filename = argv[1];
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        perror("fopen");
        return EXIT_FAILURE;
    }
    size_t fileSize;
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    FusesConfiguration config = {
        .rawData = malloc(fileSize),
        .rawDataSize = fileSize,
        .busName = "/dev/i2c-1",
        .busNameLength = 11,
        .fuseDuration = 200,
        .timeResolution = 10
    };

    fread(config.rawData, fileSize, 1, file);

    FusesObject fuses = fusesInit(&config);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);

    fusesPlay(fuses, &barrier);
    pthread_barrier_wait(&barrier);

    while (fusesGetIsPlaying(&fuses));

    return EXIT_SUCCESS;
}
