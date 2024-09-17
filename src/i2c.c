#include "i2c.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <stdlib.h>


typedef uint8_t Bool8;

#define DEFAULT_BUS_NAME ("/dev/i2c-1")

#define WRITE_REQUEST_SIZE (2)
#define READ_REQUEST_SIZE (1)
#define READ_SIZE (1)
#define IO_ERROR (-1)
#define FIRST_I2C_MSB (0b0001)
#define LAST_I2C_MSB (0b1110)

typedef struct {
    char *busName;
    uint8_t deviceAddress;
    Bool8 busNameSetByUser;
    I2cError *error;
} _I2cDevice;

int _openBus(char *busName, uint8_t deviceAddress, I2cError *error) {
    int fileDescriptor = open(busName, O_RDWR);
    if (fileDescriptor == IO_ERROR) {
        error->type = I2C_ERROR_IO_ERROR;
        error->level = I2C_ERROR_LEVEL_ERROR;
        error->ioErrno = errno;
        return IO_ERROR;
    }
    if (ioctl(fileDescriptor, I2C_SLAVE, deviceAddress) == IO_ERROR) {
        error->type = I2C_ERROR_IO_ERROR;
        error->level = I2C_ERROR_LEVEL_ERROR;
        error->ioErrno = errno;
        close(fileDescriptor);
        return IO_ERROR;
    }
    return fileDescriptor;
}

void _closeBus(int fileDescriptor) {
    close(fileDescriptor);
}

void _resetError(_I2cDevice *_self) {
    _self->error->type = I2C_ERROR_NO_ERROR;
    _self->error->level = I2C_ERROR_LEVEL_INFO;
    _self->error->ioErrno = 0;
}

char * _busName(char *busName, size_t busNameLength, bool *busNameSetByUser, I2cError *error) {
    char *result;
    if (busName == NULL) {
        *busNameSetByUser = false;
        result = DEFAULT_BUS_NAME;
    } else {
        *busNameSetByUser = true;
        result = (char*)malloc(busNameLength + 1);
        if (result == NULL) {
            error->type = I2C_ERROR_MEMORY_ALLOCATION_FAILED;
            error->level = I2C_ERROR_LEVEL_ERROR;
            return NULL;
        }
        memcpy(result, busName, busNameLength);
        result[busNameLength] = '\0';
    }
}

I2cDevice * i2cInit(char *busName, size_t busNameLength, uint8_t deviceAddress) {
    _I2cDevice *device = (_I2cDevice*)malloc(sizeof(_I2cDevice));
    if (device == NULL) { return NULL; }

    device->error = (I2cError*)malloc(sizeof(I2cError));
    if (device->error == NULL) {
        free(device);
        return NULL;
    }

    device->error->type = I2C_ERROR_NO_ERROR;
    device->error->level = I2C_ERROR_LEVEL_INFO;
    device->error->ioErrno = 0;

    device->busName = _busName(busName, busNameLength, &device->busNameSetByUser, device->error);
    if (device->error->type != I2C_ERROR_NO_ERROR) {
        return (I2cDevice*)device;
    }
    
    device->deviceAddress = deviceAddress;

    return (I2cDevice*)device;
}

void i2cDestroy(I2cDevice *self) {
    _I2cDevice *_self = (_I2cDevice*)self;
    if (_self->busNameSetByUser) {
        free(_self->busName);
    }
    free(_self->error);
    free(_self);
}

I2cError i2cScan(char *busName, size_t busNameLength, uint8_t *addresses, size_t *length) {
    bool busNameSetByUser;
    I2cError error;
    error.level = I2C_ERROR_LEVEL_INFO;
    error.type = I2C_ERROR_NO_ERROR;
    error.ioErrno = 0;

    char *busName = _busName(busName, busNameLength, &busNameSetByUser, &error);
    if (error.level == I2C_ERROR_LEVEL_ERROR) {
        return error;
    }

    *length = 0;

    for (uint8_t msb = FIRST_I2C_MSB; msb <= LAST_I2C_MSB; ++msb) {
        for (uint8_t lsb = 0b0000; lsb <= 0b1111; ++lsb) {
            uint8_t address = (msb << 4) | lsb;
            int fileDescriptor = _openBus(busName, address, &error);
            if (fileDescriptor == IO_ERROR) { continue; }
            _closeBus(fileDescriptor);
            addresses[(*length)++] = address;
        }
    }

    if (busNameSetByUser) {
        free(busName);
    }

    return error;
}

bool i2cTest(I2cDevice *self) {
    _I2cDevice *_self = (_I2cDevice*)self;
    int fileDescriptor = _openBus(_self->busName, _self->deviceAddress, _self->error);
    if (fileDescriptor == IO_ERROR) { return false; }
    _closeBus(fileDescriptor);
    return true;
}

void i2cWriteByte(I2cDevice *self, uint8_t registerAddress, uint8_t value) {
    _I2cDevice *_self = (_I2cDevice*)self;
    int fileDescriptor = _openBus(_self->busName, _self->deviceAddress, _self->error);
    if (fileDescriptor == IO_ERROR) { return; }
    uint8_t buffer[2] = { registerAddress, value };
    if (write(fileDescriptor, buffer, WRITE_REQUEST_SIZE) == IO_ERROR) {
        _self->error->type = I2C_ERROR_IO_ERROR;
        _self->error->level = I2C_ERROR_LEVEL_ERROR;
        _self->error->ioErrno = errno;
        _closeBus(fileDescriptor);
        return;
    }
    _closeBus(fileDescriptor);
}

uint8_t i2cReadByte(I2cDevice *self, uint8_t registerAddress) {
    _I2cDevice *_self = (_I2cDevice*)self;
    int fileDescriptor = _openBus(_self->busName, _self->deviceAddress, _self->error);
    if (fileDescriptor == IO_ERROR) { return 0; }
    if (write(fileDescriptor, &registerAddress, READ_REQUEST_SIZE) == IO_ERROR) {
        _self->error->type = I2C_ERROR_IO_ERROR;
        _self->error->level = I2C_ERROR_LEVEL_ERROR;
        _self->error->ioErrno = errno;
        _closeBus(fileDescriptor);
        return 0;
    }
    uint8_t value = 0;
    if (read(fileDescriptor, &value, READ_SIZE) == IO_ERROR) {
        _self->error->type = I2C_ERROR_IO_ERROR;
        _self->error->level = I2C_ERROR_LEVEL_ERROR;
        _self->error->ioErrno = errno;
        _closeBus(fileDescriptor);
        return 0;
    }
    _closeBus(fileDescriptor);
    return value;
}

I2cError * i2cGetError(I2cDevice *self) {
    _I2cDevice *_self = (_I2cDevice*)self;
    return _self->error;
}

char * i2cGetErrorString(I2cError *error) {
    switch (error->type) {
        case I2C_ERROR_NO_ERROR:
            return "No error";
        case I2C_ERROR_IO_ERROR:
            return strerror(error->ioErrno);
        case I2C_ERROR_MEMORY_ALLOCATION_FAILED:
            return "Memory allocation failed";
        default:
            return "Unknown error";
    }
}
