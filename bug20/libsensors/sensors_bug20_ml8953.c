/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#define LOG_TAG "sensors"
#define SENSORS_SERVICE_NAME "sensors"

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/input.h>
#include <sys/select.h>

#include <hardware/sensors.h>
#include <cutils/native_handle.h>
#include <cutils/sockets.h>
#include <cutils/properties.h>

/******************************************************************************/
#define ID_BASE SENSORS_HANDLE_BASE
#define ID_ACCELERATION (ID_BASE+0)

#define LSG                     (980.0f)
#define CONVERT                 (GRAVITY_EARTH / LSG)
#define SENSORS_ACCELERATION    (1 << ID_ACCELERATION)
#define INPUT_DEVICE               "/dev/input/event3"
#define SUPPORTED_SENSORS       (SENSORS_ACCELERATION)
#define EVENT_MASK_ACCEL_ALL    ( (1 << ABS_X) | (1 << ABS_Y) | (1 << ABS_Z))
#define DEFAULT_THRESHOLD 100

#define ACCELERATION_X (1 << ABS_X)
#define ACCELERATION_Y (1 << ABS_Y)
#define ACCELERATION_Z (1 << ABS_Z)
#define SENSORS_ACCELERATION_ALL (ACCELERATION_X | ACCELERATION_Y | \
        ACCELERATION_Z)
#define SEC_TO_NSEC 1000000000LL
#define USEC_TO_NSEC 1000
#define CONTROL_READ 0
#define CONTROL_WRITE 1
#define WAKE_SOURCE 0x1a

uint32_t active_sensors;
int sensor_fd = -1;
int event_fd = -1;
int control_fd[2] = { -1, -1 };
sensors_data_t sensors;

static int
write_int(char const* path, int value)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0)
    {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    }
    else
    {
        if (already_warned == 0)
        {
            LOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

/*
 * the following is the list of all supported sensors
 */
static const struct sensor_t bug20_sensor_list[] =
{
    {
        .name = "ML8953 Accelerometer",
        .vendor = "Bug Labs, Inc.",
        .version = 1,
        .handle = ID_ACCELERATION,
        .type = SENSOR_TYPE_ACCELEROMETER,
        .maxRange = (GRAVITY_EARTH * 2.3f),
        .resolution = (GRAVITY_EARTH * 2.3f) / 128.0f,
        .power = 3.0f,
        .reserved = {},
    },
};

static uint32_t sensors_get_list(struct sensors_module_t *module,
                                 struct sensor_t const** list)
{
    *list = bug20_sensor_list;
    return 1;
}

/** Close the sensors device */
static int
close_sensors(struct hw_device_t *dev)
{
    struct sensors_data_device_t *device_data =
                    (struct sensors_data_device_t *)dev;
    if (device_data) {
        if (event_fd > 0)
            close(event_fd);
        free(device_data);
    }
    return 0;
}

int open_sensors_phy(struct sensors_control_device_t *dev)
{
    char devname[PATH_MAX];
    char *filename;
    int fd;
    int res;
    uint8_t bits[4];
    DIR *dir;
    struct dirent *de;

    // TODO: Determine from sysfs which device node to open.
    fd = open(INPUT_DEVICE, O_RDONLY);

    if (fd < 0)
    {
    	LOGE("Couldn't open accelerometer device %s, error = %d", INPUT_DEVICE, fd);
        return -1;
    }

    LOGD("Opened accelerometer device.");
    return fd;
}

static native_handle_t *control_open_data_source(struct sensors_control_device_t *dev)
{
	LOGD("control_open_data_source");
    native_handle_t *hd;

    if (event_fd != -1) {
        LOGE("Sensor open and not yet closed\n");
        return NULL;
    }

    if (control_fd[0] == -1 && control_fd[1] == -1) {
        if (socketpair(AF_LOCAL, SOCK_STREAM, 0, control_fd) < 0 )
        {
            LOGE("could not create thread control socket pair: %s",
                 strerror(errno));
            return NULL;
        }
    }

    sensor_fd = open_sensors_phy(dev);

    LOGD("Open sensor %d\n", sensor_fd);
    hd = native_handle_create(1, 0);
    hd->data[0] = sensor_fd;

    return hd;
}

static int control_activate(struct sensors_control_device_t *dev,
                            int handle, int enabled)
{
	LOGD("control_activate");
    uint32_t mask = (1 << handle);
    uint32_t sensors;
    uint32_t new_sensors, active, changed;

    sensors = enabled ? mask : 0;
    active = active_sensors;
    new_sensors = (active & ~mask) | (sensors & mask);
    changed = active ^ new_sensors;
    if (!changed)
        return 0;

    active_sensors = new_sensors;

    if (!enabled)
        LOGD("Deactivate sensor\n");
    else
        LOGD("Activate sensor\n");

    return 0;
}

static int control_set_delay(struct sensors_control_device_t *dev, int32_t ms)
{
    LOGD("Control set delay %d ms is not supported and fix to 400\n", ms);
    return 0;
}

static int control_wake(struct sensors_control_device_t *dev)
{
    LOGD("Control wake\n");
    //char ch = WAKE_SOURCE;
    //write(control_fd[CONTROL_WRITE], &ch, sizeof(char));
    return 0;
}

static int control_close(struct hw_device_t *dev)
{
    struct sensors_control_device_t *device_control = (void *) dev;
    close(control_fd[CONTROL_WRITE]);
    close(control_fd[CONTROL_READ]);
    close(sensor_fd);
    sensor_fd = -1; control_fd[0] = -1; control_fd[1] = -1;
    free(device_control);
    return 0;
}

int sensors_open(struct sensors_data_device_t *dev, native_handle_t* hd)
{
    event_fd = dup(hd->data[0]);
    sensors.vector.status = SENSOR_STATUS_ACCURACY_HIGH;
    LOGD("Open sensor\n");
  /*  write_int("/sys/bus/spi/drivers/lis302dl/spi3.1/threshold",
              DEFAULT_THRESHOLD);
    write_int("/sys/bus/spi/drivers/lis302dl/spi3.0/threshold",
              DEFAULT_THRESHOLD);*/
    native_handle_close(hd);
    native_handle_delete(hd);

    return 0;
}

int sensors_close(struct sensors_data_device_t *dev)
{
    if (event_fd > 0) {
        close(event_fd);
        event_fd = -1;
        LOGD("Close sensor\n");
    }
    return 0;
}

int sensors_poll(struct sensors_data_device_t *dev, sensors_data_t* values)
{
	LOGD("sensor poll");
    int fd = event_fd;
    fd_set rfds;
    struct input_event ev;
    int ret;
    uint32_t new_sensors = 0;
    unsigned char ax = 0, ay = 0, az = 0;

    while (1)
    {
        ret = read(fd, &ev, sizeof(ev));
        if (ret < (int)sizeof(ev))
            break;

        if (ev.code == ABS_X && ax == 0) {
        	sensors.acceleration.x = ev.value;
        	//abs(ev.value * CONVERT);
			ax = 1;
        }
		   if (ev.code == ABS_Y && ay == 0) {
			sensors.acceleration.y = ev.value;
			ay = 1;
		}
		if (ev.code == ABS_Z && az == 0) {
			sensors.acceleration.z = ev.value;
			az = 1;
       	}

		if (ax == 1 && ay == 1 && az == 1) {
			int64_t t = ev.time.tv_sec * SEC_TO_NSEC + ev.time.tv_usec *
						USEC_TO_NSEC;
			new_sensors = 0;
			sensors.time = t;
			LOGD("%s: sensor event %f, %f, %f\n", __FUNCTION__,
			   sensors.acceleration.x, sensors.acceleration.y,
			   sensors.acceleration.z);
			*values = sensors;
			values->sensor = ID_ACCELERATION;
			return ID_ACCELERATION;
		}
    }
    return 0;
}

/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a sensors device using name */
static int open_sensors(const struct hw_module_t* module, char const* name,
                        struct hw_device_t** device)
{
    int status = -EINVAL;

    if (!strcmp(name, SENSORS_HARDWARE_CONTROL))
    {
        struct sensors_control_device_t *device_control =
                        malloc(sizeof(struct sensors_control_device_t));
        memset(device_control, 0, sizeof(*device_control));

        device_control->common.tag = HARDWARE_DEVICE_TAG;
        device_control->common.version = 0;
        device_control->common.module = (struct hw_module_t*)module;
        device_control->common.close = control_close;
        device_control->open_data_source = control_open_data_source;
        device_control->activate = control_activate;
        device_control->set_delay = control_set_delay;
        device_control->wake = control_wake;
        sensor_fd = -1;
        *device = &device_control->common;
        status = 0;
     } else if (!strcmp(name, SENSORS_HARDWARE_DATA)) {
        struct sensors_data_device_t *device_data =
                        malloc(sizeof(struct sensors_data_device_t));
        memset(device_data, 0, sizeof(*device_data));

        device_data->common.tag = HARDWARE_DEVICE_TAG;
        device_data->common.version = 0;
        device_data->common.module = (struct hw_module_t*)module;
        device_data->common.close = close_sensors;
        device_data->data_open = sensors_open;
        device_data->data_close = sensors_close;
        device_data->poll = sensors_poll;
        event_fd = -1;
        *device = &device_data->common;
        status = 0;
    }
    return status;
}

static struct hw_module_methods_t sensors_module_methods =
{
    .open =  open_sensors,
};

/*
 * The Sensors Hardware Module
 */
const struct sensors_module_t HAL_MODULE_INFO_SYM =
{
    .common = {
        .tag           = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id            = SENSORS_HARDWARE_MODULE_ID,
        .name          = "ML8953 Accelerometer",
        .author        = "Bug Labs, Inc.",
        .methods       = &sensors_module_methods,
    },
    .get_sensors_list = sensors_get_list
};
