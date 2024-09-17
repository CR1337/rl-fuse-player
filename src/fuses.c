#include "fuses.h"

#include <unistd.h>
#include <time.h>

typedef uint8_t Bool8;

#define FUSES_MAGIC (uint8_t[4]){'F', 'U', 'S', 'E'}
#define MAGIC_SIZE (4)

#define MAX_I2C_DEVICE_COUNT (16)
#define MAX_FUSE_COUNT_PER_DEVICE (16)
#define MAX_FUSE_COUNT (MAX_I2C_DEVICE_COUNT * MAX_FUSE_COUNT_PER_DEVICE)

#define MICROSECONDS_PER_MILLISECOND (1000)
#define MILLISECONDS_PER_SECOND (1000)
#define MILLISECONDS_PER_NANOSECOND (1000000)

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

typedef struct {
    I2cDevice *i2cDevices;
    uint8_t i2cDeviceCount;
    FusesDataItem *data;
    uint8_t dataItemCount;
    uint8_t totalDuration;
    uint32_t timeResolution;
    uint16_t fuseDuration;

    pthread_t *thread;
    pthread_barrier_t *externalBarrier;
    pthread_barrier_t *internalBarrier;
    pthread_mutex_t *actionLock;
    FusesError *error;

    pthread_t *subThreads;
    pthread_mutex_t *subThreadLocks;
    pthread_cond_t *subThreadConditions;
    Bool8 *subThreadFlags;

    uint32_t jumpTarget;
    uint32_t currentTime;

    uint32_t startTimestamp;
    uint32_t pauseStartedTimestamp;
    uint32_t timePaused;
    uint8_t nextFuseIndex;

    Bool8 useExternalBarrier;
    Bool8 isPlaying;
    Bool8 isPaused;
    Bool8 playFlag;
    Bool8 pauseFlag;
    Bool8 stopFlag;
    Bool8 haltFlag;
    Bool8 jumpFlag;
} _FusesObject;

typedef struct {
    _FusesObject *fusesObject;
    uint8_t dataItemIndex;
} SubThreadArg;

#define BASE_DEVICE_ADDRESS (0b1100000)
#define FUSE_REGISTER_BASE_ADDRESS (0x14)
#define FUSES_PER_REGISTER (4)

const uint8_t fuseRegisterMasks[4] = {
    0b00000011,
    0b00001100,
    0b00110000,
    0b11000000
};

#define INTERNAL_BARRIER_COUNT (2)

void _resetError(_FusesObject *_self) {
    _self->error->type = FUSES_ERROR_NO_ERROR;
    _self->error->level = FUSES_ERROR_LEVEL_INFO;
    _self->error->i2cError = NULL;
}

void _lightFuse(_FusesObject *_self, uint8_t dataItemIndex) {
    I2cDevice *device = _self->i2cDevices[_self->data[dataItemIndex].i2cDeviceIndex];
    uint8_t registerAddress = FUSE_REGISTER_BASE_ADDRESS + _self->data[dataItemIndex].fuseIndex / FUSES_PER_REGISTER;
    uint8_t registerMask = fuseRegisterMasks[_self->data[dataItemIndex].fuseIndex % FUSES_PER_REGISTER];

    uint8_t value = i2cReadByte(device, registerAddress);
    value &= ~registerMask;
    value |= registerMask;
    i2cWriteByte(device, registerAddress, value);
    printf("DEBUG: lit fuse %d\n", dataItemIndex);
}

void _extinguishFuse(_FusesObject *_self, uint8_t dataItemIndex) {
    I2cDevice *device = _self->i2cDevices[_self->data[dataItemIndex].i2cDeviceIndex];
    uint8_t registerAddress = FUSE_REGISTER_BASE_ADDRESS + _self->data[dataItemIndex].fuseIndex / FUSES_PER_REGISTER;
    uint8_t registerMask = fuseRegisterMasks[_self->data[dataItemIndex].fuseIndex % FUSES_PER_REGISTER];

    uint8_t value = i2cReadByte(device, registerAddress);
    value &= ~registerMask;
    i2cWriteByte(device, registerAddress, value);
    printf("DEBUG: unlit fuse %d\n", dataItemIndex);
}

void * _subThreadHandler(void *arg) {
    SubThreadArg *subThreadArg = (SubThreadArg*)arg;
    _FusesObject *_self = subThreadArg->fusesObject;
    uint8_t dataItemIndex = subThreadArg->dataItemIndex;

    pthread_mutex_lock(&_self->subThreadLocks[dataItemIndex]);
    while (!_self->subThreadFlags[dataItemIndex]) {
        pthread_cond_wait(
            &_self->subThreadConditions[dataItemIndex], 
            &_self->subThreadLocks[dataItemIndex]
        );
    }
    pthread_mutex_unlock(&_self->subThreadLocks[dataItemIndex]);

    if (_self->haltFlag) {
        return NULL;
    }

    _lightFuse(_self, dataItemIndex);
    usleep(_self->fuseDuration * MICROSECONDS_PER_MILLISECOND);
    _extinguishFuse(_self, dataItemIndex);

    return NULL;
}

void _scheduleLightFuse(_FusesObject *_self, uint8_t fuseIndex) {
    pthread_mutex_lock(&_self->subThreadLocks[fuseIndex]);
    _self->subThreadFlags[fuseIndex] = true;
    pthread_cond_signal(&_self->subThreadConditions[fuseIndex]);
    pthread_mutex_unlock(&_self->subThreadLocks[fuseIndex]);
}

void _waitForBarriers(_FusesObject *_self) {
    pthread_barrier_wait(_self->internalBarrier);
    if (_self->externalBarrier != NULL) {
        pthread_barrier_wait(_self->externalBarrier);
        _self->externalBarrier = NULL;
    }
}

void _play(_FusesObject *_self) {
    _self->playFlag = false;
    _self->isPlaying = true;
    _self->isPaused = false;

    uint32_t dt = _getCurrentTime() - _self->pauseStartedTimestamp;
    _self->startTimestamp += dt;
}

void _pause(_FusesObject *_self) {
    _self->pauseFlag = false;
    _self->isPlaying = false;
    _self->isPaused = true;

    _self->pauseStartedTimestamp = _getCurrentTime();
}

void _stop(_FusesObject *_self) {
    _self->stopFlag = false;
    _self->isPlaying = false;
    _self->isPaused = false;

    // _self->timePaused = 0;
    // _self->currentTime = 0;

    _self->startTimestamp = _getCurrentTime();
    _self->pauseStartedTimestamp = _self->startTimestamp;
    _self->nextFuseIndex = 0;
}

uint8_t _searchNextFuseIndex(_FusesObject *_self) {
    for (uint8_t i = 0; i < _self->dataItemCount; ++i) {
        if (_self->data[i].timestamp >= _self->jumpTarget) {
            return i;
        }
    }
}

void _jump(_FusesObject *_self) {
    _self->jumpFlag = false;
    // _self->currentTime = _self->jumpTarget;
    // if (_self->currentTime > _self->totalDuration) {
    //     _self->currentTime = _self->totalDuration;
    // }

    _self->startTimestamp = _getCurrentTime() - _self->jumpTarget;
    _self->nextFuseIndex = _searchNextFuseIndex(_self);
}

uint32_t _getCurrentTime() {
    struct timespec currentTime;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    return currentTime.tv_sec * MILLISECONDS_PER_SECOND 
        + currentTime.tv_nsec / MILLISECONDS_PER_NANOSECOND;
}

void _tick(_FusesObject *_self) {
    while (
        _self->data[_self->nextFuseIndex].timestamp 
            <= _getCurrentTime() - _self->startTimestamp
    ) {
        _scheduleLightFuse(_self, _self->nextFuseIndex);
        ++(_self->nextFuseIndex);
        if (_self->nextFuseIndex == _self->dataItemCount) {
            _stop(_self);
            break;
        }
    }
}

void * _mainloop(void *self) {
    _FusesObject *_self = (_FusesObject*)self;
    _self->isPaused = false;
    _self->startTimestamp = _getCurrentTime();
    _self->pauseStartedTimestamp = _self->startTimestamp;
    _self->nextFuseIndex = 0;
    // _self->timePaused = 0;

    while (!_self->haltFlag) {
        if (_self->playFlag) {
            _play(_self);
            _waitForBarriers(_self);
        } else if (_self->pauseFlag) {
            _pause(_self);
            _waitForBarriers(_self);
        } else if (_self->stopFlag) {
            _stop(_self);
            _waitForBarriers(_self);
        } else if (_self->jumpFlag) {
            _jump(_self);
            _waitForBarriers(_self);
        }

        usleep(_self->timeResolution * MICROSECONDS_PER_MILLISECOND);  // TODO: replace usleep
        if (_self->isPaused) continue;

        _tick(_self);
    }

    return NULL;
}

FusesObject * fusesInit(FusesConfiguration *configuration) {
    _FusesObject *_self = (_FusesObject*)calloc(1, sizeof(_FusesObject));
    if (_self == NULL) { return NULL; }

    _self->error = (FusesError*)calloc(1, sizeof(FusesError));
    if (_self->error == NULL) {
        free(_self);
        return NULL;
    }
    _resetError(_self);

    FusesHeader *header = (FusesHeader*)configuration->rawData;
    if (memcmp(header->fusesMagic, FUSES_MAGIC, MAGIC_SIZE) != 0) {
        _self->error->type = FUSES_ERROR_INVALID_MAGIC_NUMBER;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }

    _self->dataItemCount = header->dataItemCount;
    _self->fuseDuration = configuration->fuseDuration;
    _self->timeResolution = configuration->timeResolution;

    _self->i2cDevices = (I2cDevice*)calloc(MAX_I2C_DEVICE_COUNT, sizeof(I2cDevice));
    if (_self->i2cDevices == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }

    for (int i = 0; i < MAX_I2C_DEVICE_COUNT; ++i) {
        if (!(header->i2cDeviceIndexMask & (1 << i))) continue; 
        uint8_t deviceAddress = BASE_DEVICE_ADDRESS | i;
        I2cDevice *device = i2cInit(
            configuration->busName, configuration->busNameLength, deviceAddress
        );
        if (device == NULL) {
            _self->error->type = FUSES_ERROR_I2C_INITIALIZATION_FAILED;
            _self->error->level = FUSES_ERROR_LEVEL_ERROR;
            return (FusesObject*)_self;
        }
        if (i2cGetError(device)->level == I2C_ERROR_LEVEL_ERROR) {
            _self->error->type = FUSES_I2C_ERROR;
            _self->error->level = FUSES_ERROR_LEVEL_ERROR;
            _self->error->i2cError = i2cGetError(device);
            return (FusesObject*)_self;
        }
        if (!i2cTest(device)) {
            _self->error->type = FUSES_I2C_ERROR;
            _self->error->level = FUSES_ERROR_LEVEL_ERROR;
            _self->error->i2cError = i2cGetError(device);
            return (FusesObject*)_self;
        }
        _self->i2cDevices[_self->i2cDeviceCount++] = device;
    }

    _self->data = (FusesDataItem*)(configuration->rawData + sizeof(FusesHeader));
    _self->totalDuration = _self->data[_self->dataItemCount - 1].timestamp + _self->fuseDuration;

    _self->thread = (pthread_t*)calloc(1, sizeof(pthread_t));
    if (_self->thread == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }

    _self->internalBarrier = (pthread_barrier_t*)calloc(1, sizeof(pthread_barrier_t));
    if (_self->internalBarrier == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }

    _self->actionLock = (pthread_mutex_t*)calloc(1, sizeof(pthread_mutex_t));
    if (_self->actionLock == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }
    pthread_mutex_init(_self->actionLock, NULL);

    
    if ((_self->subThreads = (pthread_t*)calloc(_self->dataItemCount, sizeof(pthread_t))) == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }
    if ((_self->subThreadLocks = (pthread_mutex_t*)calloc(_self->dataItemCount, sizeof(pthread_mutex_t))) == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }
    if ((_self->subThreadConditions = (pthread_cond_t*)calloc(_self->dataItemCount, sizeof(pthread_cond_t))) == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }
    if ((_self->subThreadFlags = (Bool8*)calloc(_self->dataItemCount, sizeof(Bool8))) == NULL) {
        _self->error->type = FUSES_ERROR_MEMORY_ALLOCATION_FAILED;
        _self->error->level = FUSES_ERROR_LEVEL_ERROR;
        return (FusesObject*)_self;
    }
    for (int i = 0; i < _self->dataItemCount; ++i) {
        pthread_mutex_init(&_self->subThreadLocks[i], NULL);
        pthread_cond_init(&_self->subThreadConditions[i], NULL);
        _self->subThreadFlags[i] = false;
        pthread_create(
            &_self->subThreads[i], NULL, _subThreadHandler, 
            (void*)&(SubThreadArg){.fusesObject = _self, .dataItemIndex = i}
        );
    }

    _self->jumpTarget = 0;
    _self->currentTime = 0;
    _self->startTimestamp = 0;
    _self->pauseStartedTimestamp = 0;
    // _self->timePaused = 0;

    _self->useExternalBarrier = false;
    _self->isPlaying = false;
    _self->isPaused = false;
    _self->playFlag = false;
    _self->pauseFlag = false;
    _self->stopFlag = false;
    _self->haltFlag = false;
    _self->jumpFlag = false;

    pthread_create(_self->thread, NULL, _mainloop, (void*)_self);
    return (FusesObject*)_self;
}

void fusesDestroy(FusesObject *self) {
    _FusesObject *_self = (_FusesObject*)self;
    
    free(_self);
}

/**
 * @brief This function locks the action lock and checks if the action is allowed.
*/
bool _lockAction(
    _FusesObject *_self, pthread_barrier_t *barrier, bool predicate
) {
    // This lock prevents any other action from being processed while
    // the current action is being processed.
    pthread_mutex_lock(_self->actionLock);

    // If the predicate is false the action is not allowed.
    if (!predicate) {
        pthread_mutex_unlock(_self->actionLock);
        return false;
    }

    // If an external barrier is used, wait for it.
    _self->externalBarrier = barrier;
    pthread_barrier_init(
        _self->internalBarrier, NULL, INTERNAL_BARRIER_COUNT
    );

    return true;
}

/**
 * @brief This function unlocks the action lock and waits for the fuses thread to process the action.
*/
void _unlockAction(_FusesObject *_self) {
    // wait for the fuses thread to process the action
    pthread_barrier_wait(_self->internalBarrier);
    pthread_barrier_destroy(_self->internalBarrier);

    pthread_mutex_unlock(_self->actionLock);
}

bool fusesPlay(FusesObject *self, pthread_barrier_t *barrier) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    if (!_lockAction(
        _self, barrier, 
        !_self->isPlaying 
    )) {
        _self->error->type = FUSES_WARNING_ALREADY_PLAYING;
        _self->error->level = FUSES_ERROR_LEVEL_WARNING;
        return false;
    }

    _self->playFlag = true;

    _unlockAction(_self);
    return true;
}

bool fusesPause(FusesObject *self, pthread_barrier_t *barrier) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    if (!_lockAction(
        _self, barrier, 
        _self->isPlaying
    )) {
        _self->error->type = FUSES_WARNING_ALREADY_PAUSED;
        _self->error->level = FUSES_ERROR_LEVEL_WARNING;
        return false;
    }

    _self->pauseFlag = true;

    _unlockAction(_self);
    return true;
}

void fusesStop(FusesObject *self, pthread_barrier_t *barrier) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    _lockAction(_self, barrier, true);

    _self->stopFlag = true;

    _unlockAction(_self);
}

void fusesJump(FusesObject *self, pthread_barrier_t *barrier, uint32_t milliseconds) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    _lockAction(_self, barrier, true);

    _self->jumpFlag = true;
    if (milliseconds > _self->totalDuration) {
        _self->error->type = FUSES_WARNING_JUMPED_BEYOND_END;
        _self->error->level = FUSES_ERROR_LEVEL_WARNING;
    }
    _self->jumpTarget = milliseconds;

    _unlockAction(_self);
}

bool fusesGetIsPlaying(FusesObject *self) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    return _self->isPlaying;
}

bool fusesGetIsPaused(FusesObject *self) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    return _self->isPaused;
}

uint64_t fusesGetCurrentTime(FusesObject *self) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    return _self->currentTime;
}

uint32_t fusesGetTotalDuration(FusesObject *self) {
    _FusesObject *_self = (_FusesObject*)self;
    _resetError(_self);
    return _self->totalDuration;
}

FusesError * fusesGetError(FusesObject *self) {
    _FusesObject *_self = (_FusesObject*)self;
    return _self->error;
}

char * fusesGetErrorString(FusesError *error) {
    switch (error->type) {
        // TODO: add all error types

        // info
        case FUSES_ERROR_NO_ERROR:
            return "No error";

        // warnings
        case FUSES_WARNING_ALREADY_PLAYING:
            return "Fuses are already playing";

        case FUSES_WARNING_ALREADY_PAUSED:
            return "Fuses are already paused";

        case FUSES_WARNING_JUMPED_BEYOND_END:
            return "Jumoed beyond end of fuses";


        // erorrs
        // fuses
        case FUSES_ERROR_INVALID_MAGIC_NUMBER:
            return "FUSE magic is invalid";

        // i2c
        case FUSES_I2C_ERROR:
            return i2cGetErrorString(error->i2cError);

        case FUSES_ERROR_I2C_INITIALIZATION_FAILED:
            return "Initialization ot the i2c device failed";

        // other
        case FUSES_ERROR_MEMORY_ALLOCATION_FAILED:
            return "Memory allocation failed";

        default:
            return "Unknown error";
    }
}
