#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t fusesMagic[4];
    uint8_t dataItemCount;
    uint16_t i2cDeviceIndexMask;
} FusesHeader;

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint8_t i2cDeviceIndex;
    uint8_t fuseIndex;
    uint8_t __align[2];
} FusesDataItem;

#define ITEM_COUNT (8)
#define WAIT_TIME (500)

int main(int argc, char *argv[]) {
    FusesHeader header = {
        .dataItemCount = ITEM_COUNT,
        .i2cDeviceIndexMask = 0b00000001
    };
    header.fusesMagic[0] = 'F';
    header.fusesMagic[1] = 'U';
    header.fusesMagic[2] = 'S';
    header.fusesMagic[3] = 'E';

    FusesDataItem items[ITEM_COUNT];
    for (int i = 0; i < ITEM_COUNT; ++i) {
        items[i].timestamp = i * WAIT_TIME;
        items[i].i2cDeviceIndex = 1;
        items[i].fuseIndex = i;
        items[i].__align[0] = 255;
        items[i].__align[1] = 255;
    }

    FILE *file = fopen("fuses.bin", "wb");
    if (file == NULL) {
        perror("fopen");
        return EXIT_FAILURE;
    }

    fwrite(&header, sizeof(FusesHeader), 1, file);
    fwrite(items, sizeof(FusesDataItem), ITEM_COUNT, file);

    fclose(file);

    return EXIT_SUCCESS;
}
