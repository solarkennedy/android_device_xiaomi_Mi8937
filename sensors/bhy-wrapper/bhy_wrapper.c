/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * sensors.pepito_bhy.so - multihal sub-HAL that wraps the prebuilt BST BHy HAL
 * (sensors.native.so) and corrects the sensor list it advertises.
 *
 * The blob fills sensor_t straight from the hub's 16-byte sensor-info record:
 *   maxRange   = info.max_range   -> "4"  (g, not m/s^2), "2000" (dps), ...
 *   resolution = info.resolution  -> "16" (the sample BIT WIDTH, not a unit)
 * Stock Android 8.1 never looked at either. Android 11+ SensorService rounds
 * every accel/gyro/mag/pressure sample to a multiple of 0.125 * resolution
 * (SensorDeviceUtils::quantizeValue), so resolution=16 turned the raw
 * accelerometer into 2 m/s^2 steps (flat = 0, 0, 10.00), the gyro into
 * 2 rad/s steps and the magnetometer into 2 uT steps, and maxRange got rounded
 * to 0. The hub's fused outputs (rotation vector, linear accel, ...) are not
 * quantized by the framework, which is why they alone looked fine.
 *
 * The event path in the blob is correct (float scale = base * range / 32768),
 * so only the advertised list needs fixing. The values below are the blob's
 * own per-LSB scales, read out of its live sensor table on a PVG100
 * (hub dynamic ranges: accel 4 g, gyro 2000 dps, mag 4912).
 */

#define LOG_TAG "bhy_wrapper"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/sensors.h>
#include <log/log.h>

#define REAL_HAL "sensors.native.so"

#define FULL_SCALE 32768.0f
#define ACCEL_MAX (4.0f * GRAVITY_EARTH)            /* +-4 g in m/s^2 */
#define GYRO_MAX (2000.0f * 0.0174532925f)          /* +-2000 dps in rad/s */
#define MAG_MAX 4912.0f                             /* uT */
#define PRESSURE_MAX 1100.0f                        /* hPa (BMP280) */
#define PRESSURE_RES (1.0f / 12800.0f)              /* hub LSB = 1/128 Pa */
#define ORIENTATION_MAX 360.0f
#define UNIT_MAX 1.0f                               /* quaternion components */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct sensors_module_t* real;
static struct sensor_t* fixed_list;
static int fixed_count;

static struct sensors_module_t* load_real(void) {
    if (real) return real;

    void* handle = dlopen(REAL_HAL, RTLD_NOW);
    if (!handle) {
        ALOGE("dlopen(%s) failed: %s", REAL_HAL, dlerror());
        return NULL;
    }
    real = (struct sensors_module_t*)dlsym(handle, HAL_MODULE_INFO_SYM_AS_STR);
    if (!real) {
        ALOGE("%s has no %s", REAL_HAL, HAL_MODULE_INFO_SYM_AS_STR);
        dlclose(handle);
    }
    return real;
}

static void fix_sensor(struct sensor_t* s) {
    float max;

    switch (s->type) {
        case SENSOR_TYPE_ACCELEROMETER:
        case SENSOR_TYPE_GRAVITY:
        case SENSOR_TYPE_LINEAR_ACCELERATION:
            max = ACCEL_MAX;
            break;
        case SENSOR_TYPE_GYROSCOPE:
        case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
            max = GYRO_MAX;
            break;
        case SENSOR_TYPE_MAGNETIC_FIELD:
        case SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED:
            max = MAG_MAX;
            break;
        case SENSOR_TYPE_ORIENTATION:
            max = ORIENTATION_MAX;
            break;
        case SENSOR_TYPE_ROTATION_VECTOR:
        case SENSOR_TYPE_GAME_ROTATION_VECTOR:
        case SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR:
            /* Quaternions are Q14, not full-scale 32768. */
            s->maxRange = UNIT_MAX;
            s->resolution = 1.0f / 16384.0f;
            return;
        case SENSOR_TYPE_PRESSURE:
            s->maxRange = PRESSURE_MAX;
            s->resolution = PRESSURE_RES;
            return;
        default:
            /* Step/motion/custom sensors: the blob's values are harmless. */
            return;
    }
    s->maxRange = max;
    s->resolution = max / FULL_SCALE;
}

static int wrapper_get_sensors_list(struct sensors_module_t* module __unused,
                                    struct sensor_t const** list) {
    int count;

    pthread_mutex_lock(&lock);
    if (!fixed_list) {
        struct sensor_t const* real_list = NULL;

        if (!load_real()) {
            pthread_mutex_unlock(&lock);
            *list = NULL;
            return 0;
        }
        count = real->get_sensors_list(real, &real_list);
        if (count <= 0 || !real_list) {
            pthread_mutex_unlock(&lock);
            *list = NULL;
            return 0;
        }
        fixed_list = calloc(count, sizeof(*fixed_list));
        if (!fixed_list) {
            /* Better coarse sensors than none. */
            pthread_mutex_unlock(&lock);
            *list = real_list;
            return count;
        }
        memcpy(fixed_list, real_list, count * sizeof(*fixed_list));
        for (int i = 0; i < count; i++) fix_sensor(&fixed_list[i]);
        fixed_count = count;
        ALOGI("corrected range/resolution of %d BHy sensors", count);
    }
    *list = fixed_list;
    count = fixed_count;
    pthread_mutex_unlock(&lock);
    return count;
}

static int wrapper_set_operation_mode(unsigned int mode) {
    pthread_mutex_lock(&lock);
    struct sensors_module_t* m = load_real();
    pthread_mutex_unlock(&lock);

    if (!m) return -ENODEV;
    if (!m->set_operation_mode) return mode == SENSOR_HAL_NORMAL_MODE ? 0 : -EINVAL;
    return m->set_operation_mode(mode);
}

/* Hand multihal the blob's own device; only the list needs wrapping. */
static int wrapper_open(const struct hw_module_t* module __unused, const char* id,
                        struct hw_device_t** device) {
    pthread_mutex_lock(&lock);
    struct sensors_module_t* m = load_real();
    pthread_mutex_unlock(&lock);

    if (!m) return -ENODEV;
    return m->common.methods->open(&m->common, id, device);
}

static struct hw_module_methods_t wrapper_methods = {
    .open = wrapper_open,
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = SENSORS_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = SENSORS_HARDWARE_MODULE_ID,
        .name = "BHy HAL wrapper (pepito)",
        .author = "The LineageOS Project",
        .methods = &wrapper_methods,
    },
    .get_sensors_list = wrapper_get_sensors_list,
    .set_operation_mode = wrapper_set_operation_mode,
};
