#ifndef __FUSES_H__
#define __FUSES_H__

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "i2c.h"

enum FusesErrorType {
    // info
    FUSES_ERROR_NO_ERROR,

    // warnings
    FUSES_WARNING_ALREADY_PLAYING,
    FUSES_WARNING_ALREADY_PAUSED,
    FUSES_WARNING_JUMPED_BEYOND_END,

    // errors
    // fuses
    FUSES_ERROR_INVALID_MAGIC_NUMBER,
    // i2c
    FUSES_I2C_ERROR,
    FUSES_ERROR_I2C_INITIALIZATION_FAILED,
    // other
    FUSES_ERROR_MEMORY_ALLOCATION_FAILED
};

enum FusesErrorLevel {
    FUSES_ERROR_LEVEL_INFO = 0,
    FUSES_ERROR_LEVEL_WARNING = 1,
    FUSES_ERROR_LEVEL_ERROR = 2
};

typedef struct {
    enum FusesErrorType type;
    enum FusesErrorLevel level;
    I2cError *i2cError;
} FusesError;

typedef struct {
    void *rawData;
    size_t rawDataSize;
    char *busName;
    size_t busNameLength;
    uint16_t fuseDuration;
    uint32_t timeResolution;
} FusesConfiguration;

typedef void* FusesObject;

FusesObject * fusesInit(FusesConfiguration *configuration);
void fusesDestroy(FusesObject *self);

bool fusesPlay(FusesObject *self, pthread_barrier_t *barrier);
bool fusesPause(FusesObject *self, pthread_barrier_t *barrier);
void fusesStop(FusesObject *self, pthread_barrier_t *barrier);
void fusesJump(FusesObject *self, pthread_barrier_t *barrier, uint32_t milliseconds);

bool fusesGetIsPlaying(FusesObject *self);
bool fusesGetIsPaused(FusesObject *self);
uint32_t fusesGetCurrentTime(FusesObject *self);
uint32_t fusesGetTotalDuration(FusesObject *self);

FusesError * fusesGetError(FusesObject *self);
char * fusesGetErrorString(FusesError *error);

#endif // __FUSES_H__
