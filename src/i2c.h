#ifndef __I2C_H__
#define __I2C_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum I2cErrorType {
    I2C_ERROR_NO_ERROR,
    I2C_ERROR_IO_ERROR,
    I2C_ERROR_MEMORY_ALLOCATION_FAILED
};

enum I2cErrorLevel {
    I2C_ERROR_LEVEL_INFO = 0,
    I2C_ERROR_LEVEL_WARNING = 1,
    I2C_ERROR_LEVEL_ERROR = 2
};

typedef struct {
    enum I2cErrorType type;
    enum I2cErrorLevel level;
    int ioErrno;
} I2cError;

typedef void* I2cDevice;

I2cDevice * i2cInit(char *busName, size_t busNameLength, uint8_t deviceAddress);
void i2cDestroy(I2cDevice *self);

I2cError i2cScan(char *busName, size_t busNameLength, uint8_t *addresses, size_t *length);
bool i2cTest(I2cDevice *self);
void i2cWriteByte(I2cDevice *self, uint8_t registerAddress, uint8_t value);
uint8_t i2cReadByte(I2cDevice *self, uint8_t registerAddress);

I2cError * i2cGetError(I2cDevice *self);
char * i2cGetErrorString(I2cError *error);

#endif
