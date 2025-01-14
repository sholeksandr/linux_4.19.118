// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2016-2018 Mellanox Technologies. All rights reserved
 * Copyright (c) 2016 Ivan Vecera <cera@cera.cz>
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/err.h>
#include <linux/sfp.h>

#include "core.h"
#include "core_env.h"

#define MLXSW_THERMAL_POLL_INT	1000	/* ms */
#define MLXSW_THERMAL_SLOW_POLL_INT	20000	/* ms */
#define MLXSW_THERMAL_ASIC_TEMP_NORM	75000	/* 75C */
#define MLXSW_THERMAL_ASIC_TEMP_HIGH	85000	/* 85C */
#define MLXSW_THERMAL_ASIC_TEMP_HOT	105000	/* 105C */
#define MLXSW_THERMAL_MODULE_TEMP_NORM	60000	/* 60C */
#define MLXSW_THERMAL_MODULE_TEMP_HIGH	70000	/* 70C */
#define MLXSW_THERMAL_MODULE_TEMP_HOT	80000	/* 80C */
#define MLXSW_THERMAL_HYSTERESIS_TEMP	5000	/* 5C */
#define MLXSW_THERMAL_MODULE_TEMP_SHIFT	(MLXSW_THERMAL_HYSTERESIS_TEMP * 2)
#define MLXSW_THERMAL_ZONE_MAX_NAME	16
#define MLXSW_THERMAL_TEMP_SCORE_MAX	GENMASK(31, 0)
#define MLXSW_THERMAL_MAX_STATE	10
#define MLXSW_THERMAL_MAX_DUTY	255
/* Minimum and maximum fan allowed speed in percent: from 20% to 100%. Values
 * MLXSW_THERMAL_MAX_STATE + x, where x is between 2 and 10 are used for
 * setting fan speed dynamic minimum. For example, if value is set to 14 (40%)
 * cooling levels vector will be set to 4, 4, 4, 4, 4, 5, 6, 7, 8, 9, 10 to
 * introduce PWM speed in percent: 40, 40, 40, 40, 40, 50, 60. 70, 80, 90, 100.
 */
#define MLXSW_THERMAL_SPEED_MIN		(MLXSW_THERMAL_MAX_STATE + 2)
#define MLXSW_THERMAL_SPEED_MAX		(MLXSW_THERMAL_MAX_STATE * 2)
#define MLXSW_THERMAL_SPEED_MIN_LEVEL	2		/* 20% */

/* External cooling devices, allowed for binding to mlxsw thermal zones. */
static char * const mlxsw_thermal_external_allowed_cdev[] = {
	"mlxreg_fan",
};

enum mlxsw_thermal_trips {
	MLXSW_THERMAL_TEMP_TRIP_NORM,
	MLXSW_THERMAL_TEMP_TRIP_HIGH,
	MLXSW_THERMAL_TEMP_TRIP_HOT,
};

struct mlxsw_thermal_trip {
	int	type;
	int	temp;
	int	hyst;
	int	min_state;
	int	max_state;
};

static const struct mlxsw_thermal_trip default_thermal_trips[] = {
	{	/* In range - 0-40% PWM */
		.type		= THERMAL_TRIP_ACTIVE,
		.temp		= MLXSW_THERMAL_ASIC_TEMP_NORM,
		.hyst		= MLXSW_THERMAL_HYSTERESIS_TEMP,
		.min_state	= 0,
		.max_state	= (4 * MLXSW_THERMAL_MAX_STATE) / 10,
	},
	{
		/* In range - 40-100% PWM */
		.type		= THERMAL_TRIP_ACTIVE,
		.temp		= MLXSW_THERMAL_ASIC_TEMP_HIGH,
		.hyst		= MLXSW_THERMAL_HYSTERESIS_TEMP,
		.min_state	= (4 * MLXSW_THERMAL_MAX_STATE) / 10,
		.max_state	= MLXSW_THERMAL_MAX_STATE,
	},
	{	/* Warning */
		.type		= THERMAL_TRIP_HOT,
		.temp		= MLXSW_THERMAL_ASIC_TEMP_HOT,
		.min_state	= MLXSW_THERMAL_MAX_STATE,
		.max_state	= MLXSW_THERMAL_MAX_STATE,
	},
};

#define MLXSW_THERMAL_NUM_TRIPS	ARRAY_SIZE(default_thermal_trips)

/* Make sure all trips are writable */
#define MLXSW_THERMAL_TRIP_MASK	(BIT(MLXSW_THERMAL_NUM_TRIPS) - 1)

struct mlxsw_thermal;
struct mlxsw_thermal_area;

struct mlxsw_thermal_module {
	struct mlxsw_thermal *parent;
	struct mlxsw_thermal_area *area;
	struct thermal_zone_device *tzdev;
	struct mlxsw_thermal_trip trips[MLXSW_THERMAL_NUM_TRIPS];
	enum thermal_device_mode mode;
	int module; /* Module or gearbox number */
	u8 slot_index;
};

struct mlxsw_thermal_area {
	struct mlxsw_thermal_module *tz_module_arr;
	u8 tz_module_num;
	struct mlxsw_thermal_module *tz_gearbox_arr;
	u8 tz_gearbox_num;
	u8 slot_index;
	u16 *gearbox_sensor_map;
};

struct mlxsw_thermal {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	struct thermal_zone_device *tzdev;
	int polling_delay;
	struct thermal_cooling_device *cdevs[MLXSW_MFCR_PWMS_MAX];
	u8 cooling_levels[MLXSW_THERMAL_MAX_STATE + 1];
	struct mlxsw_thermal_trip trips[MLXSW_THERMAL_NUM_TRIPS];
	enum thermal_device_mode mode;
	struct mlxsw_thermal_area *main;
	struct mlxsw_thermal_area **linecards;
	unsigned int tz_highest_score;
	struct thermal_zone_device *tz_highest_dev;
	bool initializing; /* Driver is in initialization stage */
};

static inline u8 mlxsw_state_to_duty(int state)
{
	return DIV_ROUND_CLOSEST(state * MLXSW_THERMAL_MAX_DUTY,
				 MLXSW_THERMAL_MAX_STATE);
}

static inline int mlxsw_duty_to_state(u8 duty)
{
	return DIV_ROUND_CLOSEST(duty * MLXSW_THERMAL_MAX_STATE,
				 MLXSW_THERMAL_MAX_DUTY);
}

static int mlxsw_get_cooling_device_idx(struct mlxsw_thermal *thermal,
					struct thermal_cooling_device *cdev)
{
	int i;

	for (i = 0; i < MLXSW_MFCR_PWMS_MAX; i++)
		if (thermal->cdevs[i] == cdev)
			return i;

	/* Allow mlxsw thermal zone binding to an external cooling device */
	for (i = 0; i < ARRAY_SIZE(mlxsw_thermal_external_allowed_cdev); i++) {
		if (strnstr(cdev->type, mlxsw_thermal_external_allowed_cdev[i],
			    sizeof(cdev->type)))
			return 0;
	}

	return -ENODEV;
}

static void
mlxsw_thermal_module_trips_reset(struct mlxsw_thermal_module *tz)
{
	tz->trips[MLXSW_THERMAL_TEMP_TRIP_NORM].temp = MLXSW_THERMAL_MODULE_TEMP_NORM;
	tz->trips[MLXSW_THERMAL_TEMP_TRIP_HIGH].temp = MLXSW_THERMAL_MODULE_TEMP_HIGH;
	tz->trips[MLXSW_THERMAL_TEMP_TRIP_HOT].temp = MLXSW_THERMAL_MODULE_TEMP_HOT;
}

static int
mlxsw_thermal_module_trips_update(struct device *dev, struct mlxsw_core *core,
				  struct mlxsw_thermal_module *tz)
{
	int crit_temp, emerg_temp;
	int err;

	err = mlxsw_env_module_temp_thresholds_get(core, tz->slot_index,
						   tz->module,
						   SFP_TEMP_HIGH_WARN,
						   &crit_temp);
	if (err)
		return err;

	err = mlxsw_env_module_temp_thresholds_get(core, tz->slot_index,
						   tz->module,
						   SFP_TEMP_HIGH_ALARM,
						   &emerg_temp);
	if (err)
		return err;

	if (crit_temp > emerg_temp) {
		dev_warn(dev, "%s : Critical threshold %d is above emergency threshold %d\n",
			 tz->tzdev->type, crit_temp, emerg_temp);
		return 0;
	}

	/* According to the system thermal requirements, the thermal zones are
	 * defined with four trip points. The critical and emergency
	 * temperature thresholds, provided by QSFP module are set as "active"
	 * and "hot" trip points, "normal" and "critical" trip points are
	 * derived from "active" and "hot" by subtracting or adding double
	 * hysteresis value.
	 */
	if (crit_temp >= MLXSW_THERMAL_MODULE_TEMP_SHIFT)
		tz->trips[MLXSW_THERMAL_TEMP_TRIP_NORM].temp = crit_temp -
					MLXSW_THERMAL_MODULE_TEMP_SHIFT;
	else
		tz->trips[MLXSW_THERMAL_TEMP_TRIP_NORM].temp = crit_temp;
	tz->trips[MLXSW_THERMAL_TEMP_TRIP_HIGH].temp = crit_temp;
	tz->trips[MLXSW_THERMAL_TEMP_TRIP_HOT].temp = emerg_temp;

	return 0;
}

static void mlxsw_thermal_tz_score_update(struct mlxsw_thermal *thermal,
					  struct thermal_zone_device *tzdev,
					  struct mlxsw_thermal_trip *trips,
					  int temp)
{
	struct mlxsw_thermal_trip *trip = trips;
	unsigned int score, delta, i, shift = 1;

	/* Calculate thermal zone score, if temperature is above the critical
	 * threshold score is set to MLXSW_THERMAL_TEMP_SCORE_MAX.
	 */
	score = MLXSW_THERMAL_TEMP_SCORE_MAX;
	for (i = MLXSW_THERMAL_TEMP_TRIP_NORM; i < MLXSW_THERMAL_NUM_TRIPS;
	     i++, trip++) {
		if (temp < trip->temp) {
			delta = DIV_ROUND_CLOSEST(temp, trip->temp - temp);
			score = delta * shift;
			break;
		}
		shift *= 256;
	}

	if (score > thermal->tz_highest_score) {
		thermal->tz_highest_score = score;
		thermal->tz_highest_dev = tzdev;
	}
}

static int mlxsw_thermal_bind(struct thermal_zone_device *tzdev,
			      struct thermal_cooling_device *cdev)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;
	struct device *dev = thermal->bus_info->dev;
	int i, err;

	/* If the cooling device is one of ours bind it */
	if (mlxsw_get_cooling_device_idx(thermal, cdev) < 0)
		return 0;

	for (i = 0; i < MLXSW_THERMAL_NUM_TRIPS; i++) {
		const struct mlxsw_thermal_trip *trip = &thermal->trips[i];

		err = thermal_zone_bind_cooling_device(tzdev, i, cdev,
						       trip->max_state,
						       trip->min_state,
						       THERMAL_WEIGHT_DEFAULT);
		if (err < 0) {
			dev_err(dev, "Failed to bind cooling device to trip %d\n", i);
			return err;
		}
	}
	return 0;
}

static int mlxsw_thermal_unbind(struct thermal_zone_device *tzdev,
				struct thermal_cooling_device *cdev)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;
	struct device *dev = thermal->bus_info->dev;
	int i;
	int err;

	/* If the cooling device is our one unbind it */
	if (mlxsw_get_cooling_device_idx(thermal, cdev) < 0)
		return 0;

	for (i = 0; i < MLXSW_THERMAL_NUM_TRIPS; i++) {
		err = thermal_zone_unbind_cooling_device(tzdev, i, cdev);
		if (err < 0) {
			dev_err(dev, "Failed to unbind cooling device\n");
			return err;
		}
	}
	return 0;
}

static int mlxsw_thermal_get_mode(struct thermal_zone_device *tzdev,
				  enum thermal_device_mode *mode)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	*mode = thermal->mode;

	return 0;
}

static int mlxsw_thermal_set_mode(struct thermal_zone_device *tzdev,
				  enum thermal_device_mode mode)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	mutex_lock(&tzdev->lock);

	if (mode == THERMAL_DEVICE_ENABLED)
		tzdev->polling_delay = thermal->polling_delay;
	else
		tzdev->polling_delay = 0;

	mutex_unlock(&tzdev->lock);

	thermal->mode = mode;
	thermal_zone_device_update(tzdev, THERMAL_EVENT_UNSPECIFIED);

	return 0;
}

static int mlxsw_thermal_get_temp(struct thermal_zone_device *tzdev,
				  int *p_temp)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;
	struct device *dev = thermal->bus_info->dev;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int temp;
	int err;

	/* Do not read temperature in initialization stage. */
	if (thermal->initializing) {
		*p_temp = 0;
		return 0;
	}

	mlxsw_reg_mtmp_pack(mtmp_pl, 0, 0, false, false);

	err = mlxsw_reg_query(thermal->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(dev, "Failed to query temp sensor\n");
		return err;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, &temp, NULL, NULL);
	if (temp > 0)
		mlxsw_thermal_tz_score_update(thermal, tzdev, thermal->trips,
					      temp);

	*p_temp = temp;
	return 0;
}

static int mlxsw_thermal_get_trip_type(struct thermal_zone_device *tzdev,
				       int trip,
				       enum thermal_trip_type *p_type)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	*p_type = thermal->trips[trip].type;
	return 0;
}

static int mlxsw_thermal_get_trip_temp(struct thermal_zone_device *tzdev,
				       int trip, int *p_temp)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	*p_temp = thermal->trips[trip].temp;
	return 0;
}

static int mlxsw_thermal_set_trip_temp(struct thermal_zone_device *tzdev,
				       int trip, int temp)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	thermal->trips[trip].temp = temp;
	return 0;
}

static int mlxsw_thermal_get_trip_hyst(struct thermal_zone_device *tzdev,
				       int trip, int *p_hyst)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	*p_hyst = thermal->trips[trip].hyst;
	return 0;
}

static int mlxsw_thermal_set_trip_hyst(struct thermal_zone_device *tzdev,
				       int trip, int hyst)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	thermal->trips[trip].hyst = hyst;
	return 0;
}

static int mlxsw_thermal_trend_get(struct thermal_zone_device *tzdev,
				   int trip, enum thermal_trend *trend)
{
	struct mlxsw_thermal *thermal = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	if (tzdev == thermal->tz_highest_dev)
		return 1;

	*trend = THERMAL_TREND_STABLE;
	return 0;
}

static struct thermal_zone_params mlxsw_thermal_params = {
	.no_hwmon = true,
};

static struct thermal_zone_device_ops mlxsw_thermal_ops = {
	.bind = mlxsw_thermal_bind,
	.unbind = mlxsw_thermal_unbind,
	.get_mode = mlxsw_thermal_get_mode,
	.set_mode = mlxsw_thermal_set_mode,
	.get_temp = mlxsw_thermal_get_temp,
	.get_trip_type	= mlxsw_thermal_get_trip_type,
	.get_trip_temp	= mlxsw_thermal_get_trip_temp,
	.set_trip_temp	= mlxsw_thermal_set_trip_temp,
	.get_trip_hyst	= mlxsw_thermal_get_trip_hyst,
	.set_trip_hyst	= mlxsw_thermal_set_trip_hyst,
	.get_trend	= mlxsw_thermal_trend_get,
};

static int mlxsw_thermal_module_bind(struct thermal_zone_device *tzdev,
				     struct thermal_cooling_device *cdev)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;
	struct mlxsw_thermal *thermal = tz->parent;
	int i, j, err;

	/* If the cooling device is one of ours bind it */
	if (mlxsw_get_cooling_device_idx(thermal, cdev) < 0)
		return 0;

	for (i = 0; i < MLXSW_THERMAL_NUM_TRIPS; i++) {
		const struct mlxsw_thermal_trip *trip = &tz->trips[i];

		err = thermal_zone_bind_cooling_device(tzdev, i, cdev,
						       trip->max_state,
						       trip->min_state,
						       THERMAL_WEIGHT_DEFAULT);
		if (err < 0)
			goto err_thermal_zone_bind_cooling_device;
	}
	return 0;

err_thermal_zone_bind_cooling_device:
	for (j = i - 1; j >= 0; j--)
		thermal_zone_unbind_cooling_device(tzdev, j, cdev);
	return err;
}

static int mlxsw_thermal_module_unbind(struct thermal_zone_device *tzdev,
				       struct thermal_cooling_device *cdev)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;
	struct mlxsw_thermal *thermal = tz->parent;
	int i;
	int err;

	/* If the cooling device is one of ours unbind it */
	if (mlxsw_get_cooling_device_idx(thermal, cdev) < 0)
		return 0;

	for (i = 0; i < MLXSW_THERMAL_NUM_TRIPS; i++) {
		err = thermal_zone_unbind_cooling_device(tzdev, i, cdev);
		WARN_ON(err);
	}
	return err;
}

static int mlxsw_thermal_module_mode_get(struct thermal_zone_device *tzdev,
					 enum thermal_device_mode *mode)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;

	*mode = tz->mode;

	return 0;
}

static int mlxsw_thermal_module_mode_set(struct thermal_zone_device *tzdev,
					 enum thermal_device_mode mode)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;
	struct mlxsw_thermal *thermal = tz->parent;

	mutex_lock(&tzdev->lock);

	if (mode == THERMAL_DEVICE_ENABLED)
		tzdev->polling_delay = thermal->polling_delay;
	else
		tzdev->polling_delay = 0;

	mutex_unlock(&tzdev->lock);

	tz->mode = mode;
	thermal_zone_device_update(tzdev, THERMAL_EVENT_UNSPECIFIED);

	return 0;
}

static int mlxsw_thermal_module_temp_get(struct thermal_zone_device *tzdev,
					 int *p_temp)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;
	struct mlxsw_thermal *thermal = tz->parent;
	struct device *dev = thermal->bus_info->dev;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int temp;
	int err;

	/* Do not read temperature in initialization stage. */
	if (thermal->initializing) {
		*p_temp = 0;
		return 0;
	}

	/* Read module temperature. */
	mlxsw_reg_mtmp_pack(mtmp_pl, tz->slot_index,
			    MLXSW_REG_MTMP_MODULE_INDEX_MIN + tz->module,
			    false, false);
	err = mlxsw_reg_query(thermal->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		/* Do not return error - in case of broken module's sensor
		 * it will cause error message flooding.
		 */
		temp = 0;
		*p_temp = (int) temp;
		return 0;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, &temp, NULL, NULL);
	*p_temp = temp;

	if (!temp)
		return 0;

	/* Update trip points. */
	err = mlxsw_thermal_module_trips_update(dev, thermal->core, tz);
	if (!err && temp > 0)
		mlxsw_thermal_tz_score_update(thermal, tzdev, tz->trips, temp);

	return 0;
}

static int
mlxsw_thermal_module_trip_type_get(struct thermal_zone_device *tzdev, int trip,
				   enum thermal_trip_type *p_type)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	*p_type = tz->trips[trip].type;
	return 0;
}

static int
mlxsw_thermal_module_trip_temp_get(struct thermal_zone_device *tzdev,
				   int trip, int *p_temp)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	*p_temp = tz->trips[trip].temp;
	return 0;
}

static int
mlxsw_thermal_module_trip_temp_set(struct thermal_zone_device *tzdev,
				   int trip, int temp)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	tz->trips[trip].temp = temp;
	return 0;
}

static int
mlxsw_thermal_module_trip_hyst_get(struct thermal_zone_device *tzdev, int trip,
				   int *p_hyst)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;

	*p_hyst = tz->trips[trip].hyst;
	return 0;
}

static int
mlxsw_thermal_module_trip_hyst_set(struct thermal_zone_device *tzdev, int trip,
				   int hyst)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;

	tz->trips[trip].hyst = hyst;
	return 0;
}

static int mlxsw_thermal_module_trend_get(struct thermal_zone_device *tzdev,
					  int trip, enum thermal_trend *trend)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;
	struct mlxsw_thermal *thermal = tz->parent;

	if (trip < 0 || trip >= MLXSW_THERMAL_NUM_TRIPS)
		return -EINVAL;

	if (tzdev == thermal->tz_highest_dev)
		return 1;

	*trend = THERMAL_TREND_STABLE;
	return 0;
}

static struct thermal_zone_device_ops mlxsw_thermal_module_ops = {
	.bind		= mlxsw_thermal_module_bind,
	.unbind		= mlxsw_thermal_module_unbind,
	.get_mode	= mlxsw_thermal_module_mode_get,
	.set_mode	= mlxsw_thermal_module_mode_set,
	.get_temp	= mlxsw_thermal_module_temp_get,
	.get_trip_type	= mlxsw_thermal_module_trip_type_get,
	.get_trip_temp	= mlxsw_thermal_module_trip_temp_get,
	.set_trip_temp	= mlxsw_thermal_module_trip_temp_set,
	.get_trip_hyst	= mlxsw_thermal_module_trip_hyst_get,
	.set_trip_hyst	= mlxsw_thermal_module_trip_hyst_set,
	.get_trend	= mlxsw_thermal_module_trend_get,
};

static int mlxsw_thermal_gearbox_temp_get(struct thermal_zone_device *tzdev,
					  int *p_temp)
{
	struct mlxsw_thermal_module *tz = tzdev->devdata;
	struct mlxsw_thermal *thermal = tz->parent;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	u16 index;
	int temp;
	int err;

	/* Do not read temperature in initialization stage. */
	if (thermal->initializing) {
		*p_temp = 0;
		return 0;
	}

	index = tz->area->gearbox_sensor_map[tz->module];
	mlxsw_reg_mtmp_pack(mtmp_pl, tz->slot_index, index, false, false);

	err = mlxsw_reg_query(thermal->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err)
		return err;

	mlxsw_reg_mtmp_unpack(mtmp_pl, &temp, NULL, NULL);
	if (temp > 0)
		mlxsw_thermal_tz_score_update(thermal, tzdev, tz->trips, temp);

	*p_temp = temp;
	return 0;
}

static struct thermal_zone_device_ops mlxsw_thermal_gearbox_ops = {
	.bind		= mlxsw_thermal_module_bind,
	.unbind		= mlxsw_thermal_module_unbind,
	.get_mode	= mlxsw_thermal_module_mode_get,
	.set_mode	= mlxsw_thermal_module_mode_set,
	.get_temp	= mlxsw_thermal_gearbox_temp_get,
	.get_trip_type	= mlxsw_thermal_module_trip_type_get,
	.get_trip_temp	= mlxsw_thermal_module_trip_temp_get,
	.set_trip_temp	= mlxsw_thermal_module_trip_temp_set,
	.get_trip_hyst	= mlxsw_thermal_module_trip_hyst_get,
	.set_trip_hyst	= mlxsw_thermal_module_trip_hyst_set,
	.get_trend	= mlxsw_thermal_module_trend_get,
};

static int mlxsw_thermal_get_max_state(struct thermal_cooling_device *cdev,
				       unsigned long *p_state)
{
	*p_state = MLXSW_THERMAL_MAX_STATE;
	return 0;
}

static int mlxsw_thermal_get_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long *p_state)

{
	struct mlxsw_thermal *thermal = cdev->devdata;
	struct device *dev = thermal->bus_info->dev;
	char mfsc_pl[MLXSW_REG_MFSC_LEN];
	int err, idx;
	u8 duty;

	idx = mlxsw_get_cooling_device_idx(thermal, cdev);
	if (idx < 0)
		return idx;

	mlxsw_reg_mfsc_pack(mfsc_pl, idx, 0);
	err = mlxsw_reg_query(thermal->core, MLXSW_REG(mfsc), mfsc_pl);
	if (err) {
		dev_err(dev, "Failed to query PWM duty\n");
		return err;
	}

	duty = mlxsw_reg_mfsc_pwm_duty_cycle_get(mfsc_pl);
	*p_state = mlxsw_duty_to_state(duty);
	return 0;
}

static int mlxsw_thermal_set_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long state)

{
	struct mlxsw_thermal *thermal = cdev->devdata;
	struct device *dev = thermal->bus_info->dev;
	char mfsc_pl[MLXSW_REG_MFSC_LEN];
	unsigned long cur_state, i;
	int idx;
	u8 duty;
	int err;

	idx = mlxsw_get_cooling_device_idx(thermal, cdev);
	if (idx < 0)
		return idx;

	/* Verify if this request is for changing allowed fan dynamical
	 * minimum. If it is - update cooling levels accordingly and update
	 * state, if current state is below the newly requested minimum state.
	 * For example, if current state is 5, and minimal state is to be
	 * changed from 4 to 6, thermal->cooling_levels[0 to 5] will be changed
	 * all from 4 to 6. And state 5 (thermal->cooling_levels[4]) should be
	 * overwritten.
	 */
	if (state >= MLXSW_THERMAL_SPEED_MIN &&
	    state <= MLXSW_THERMAL_SPEED_MAX) {
		state -= MLXSW_THERMAL_MAX_STATE;
		for (i = 0; i <= MLXSW_THERMAL_MAX_STATE; i++)
			thermal->cooling_levels[i] = max(state, i);

		mlxsw_reg_mfsc_pack(mfsc_pl, idx, 0);
		err = mlxsw_reg_query(thermal->core, MLXSW_REG(mfsc), mfsc_pl);
		if (err)
			return err;

		duty = mlxsw_reg_mfsc_pwm_duty_cycle_get(mfsc_pl);
		cur_state = mlxsw_duty_to_state(duty);

		/* If current fan state is lower than requested dynamical
		 * minimum, increase fan speed up to dynamical minimum.
		 */
		if (state < cur_state)
			return 0;

		state = cur_state;
	}

	if (state > MLXSW_THERMAL_MAX_STATE)
		return -EINVAL;

	/* Normalize the state to the valid speed range. */
	state = thermal->cooling_levels[state];
	mlxsw_reg_mfsc_pack(mfsc_pl, idx, mlxsw_state_to_duty(state));
	err = mlxsw_reg_write(thermal->core, MLXSW_REG(mfsc), mfsc_pl);
	if (err) {
		dev_err(dev, "Failed to write PWM duty\n");
		return err;
	}
	return 0;
}

static const struct thermal_cooling_device_ops mlxsw_cooling_ops = {
	.get_max_state	= mlxsw_thermal_get_max_state,
	.get_cur_state	= mlxsw_thermal_get_cur_state,
	.set_cur_state	= mlxsw_thermal_set_cur_state,
};

static int
mlxsw_thermal_module_tz_init(struct mlxsw_thermal_module *module_tz)
{
	char tz_name[MLXSW_THERMAL_ZONE_MAX_NAME];
	int err;

	if (module_tz->slot_index)
		snprintf(tz_name, sizeof(tz_name), "mlxsw-lc%d-module%d",
			 module_tz->slot_index, module_tz->module + 1);
	else
		snprintf(tz_name, sizeof(tz_name), "mlxsw-module%d",
			 module_tz->module + 1);
	module_tz->tzdev = thermal_zone_device_register(tz_name,
							MLXSW_THERMAL_NUM_TRIPS,
							MLXSW_THERMAL_TRIP_MASK,
							module_tz,
							&mlxsw_thermal_module_ops,
							&mlxsw_thermal_params,
							0, 0);
	if (IS_ERR(module_tz->tzdev)) {
		err = PTR_ERR(module_tz->tzdev);
		return err;
	}

	module_tz->mode = THERMAL_DEVICE_ENABLED;
	return 0;
}

static void mlxsw_thermal_module_tz_fini(struct thermal_zone_device *tzdev)
{
	thermal_zone_device_unregister(tzdev);
}

static int
mlxsw_thermal_module_init(struct device *dev, struct mlxsw_core *core,
			  struct mlxsw_thermal *thermal,
			  struct mlxsw_thermal_area *area, u8 module)
{
	struct mlxsw_thermal_module *module_tz;

	module_tz = &area->tz_module_arr[module];
	/* Skip if parent is already set (case of port split). */
	if (module_tz->parent)
		return 0;
	module_tz->module = module;
	module_tz->area = area;
	module_tz->slot_index = area->slot_index;
	module_tz->parent = thermal;
	memcpy(module_tz->trips, default_thermal_trips,
	       sizeof(thermal->trips));
	/* Initialize all trip point. */
	mlxsw_thermal_module_trips_reset(module_tz);

	return 0;
}

static void mlxsw_thermal_module_fini(struct mlxsw_thermal_module *module_tz)
{
	if (module_tz && module_tz->tzdev) {
		mlxsw_thermal_module_tz_fini(module_tz->tzdev);
		module_tz->tzdev = NULL;
		module_tz->parent = NULL;
	}
}

static int
mlxsw_thermal_modules_init(struct device *dev, struct mlxsw_core *core,
			   struct mlxsw_thermal *thermal,
			   struct mlxsw_thermal_area *area)
{
	struct mlxsw_thermal_module *module_tz;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	int i, err;

	if (!mlxsw_core_res_query_enabled(core))
		return 0;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
			       &area->tz_module_num, NULL, NULL);

	area->tz_module_arr = kcalloc(area->tz_module_num,
				      sizeof(*area->tz_module_arr),
				      GFP_KERNEL);
	if (!area->tz_module_arr)
		return -ENOMEM;

	for (i = 0; i < area->tz_module_num; i++) {
		err = mlxsw_thermal_module_init(dev, core, thermal, area, i);
		if (err)
			goto err_thermal_module_init;
	}

	for (i = 0; i < area->tz_module_num; i++) {
		module_tz = &area->tz_module_arr[i];
		if (!module_tz->parent)
			continue;
		err = mlxsw_thermal_module_tz_init(module_tz);
		if (err)
			goto err_thermal_module_tz_init;
	}

	return 0;

err_thermal_module_tz_init:
err_thermal_module_init:
	for (i = area->tz_module_num - 1; i >= 0; i--)
		mlxsw_thermal_module_fini(&area->tz_module_arr[i]);
	kfree(area->tz_module_arr);
	return err;
}

static void
mlxsw_thermal_modules_fini(struct mlxsw_thermal *thermal,
			   struct mlxsw_thermal_area *area)
{
	int i;

	if (!mlxsw_core_res_query_enabled(thermal->core))
		return;

	for (i = area->tz_module_num - 1; i >= 0; i--)
		mlxsw_thermal_module_fini(&area->tz_module_arr[i]);
	kfree(area->tz_module_arr);
}

static int
mlxsw_thermal_gearbox_tz_init(struct mlxsw_thermal_module *gearbox_tz)
{
	char tz_name[MLXSW_THERMAL_ZONE_MAX_NAME];

	if (gearbox_tz->slot_index)
		snprintf(tz_name, sizeof(tz_name), "mlxsw-lc%d-gearbox%d",
			 gearbox_tz->slot_index, gearbox_tz->module + 1);
	else
		snprintf(tz_name, sizeof(tz_name), "mlxsw-gearbox%d",
			 gearbox_tz->module + 1);
	gearbox_tz->tzdev = thermal_zone_device_register(tz_name,
						MLXSW_THERMAL_NUM_TRIPS,
						MLXSW_THERMAL_TRIP_MASK,
						gearbox_tz,
						&mlxsw_thermal_gearbox_ops,
						&mlxsw_thermal_params, 0, 0);
	if (IS_ERR(gearbox_tz->tzdev))
		return PTR_ERR(gearbox_tz->tzdev);

	gearbox_tz->mode = THERMAL_DEVICE_ENABLED;
	return 0;
}

static void
mlxsw_thermal_gearbox_tz_fini(struct mlxsw_thermal_module *gearbox_tz)
{
	thermal_zone_device_unregister(gearbox_tz->tzdev);
}

static int
mlxsw_thermal_gearboxes_main_init(struct device *dev, struct mlxsw_core *core,
				  struct mlxsw_thermal_area *area)
{
	enum mlxsw_reg_mgpir_device_type device_type;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	int i = 0, err;

	if (!mlxsw_core_res_query_enabled(core))
		return 0;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, &area->tz_gearbox_num, &device_type,
			       NULL, NULL, NULL, NULL);
	if (device_type != MLXSW_REG_MGPIR_DEVICE_TYPE_GEARBOX_DIE)
		area->tz_gearbox_num = 0;

	/* Skip gearbox sensor array allocation, if no gearboxes are available. */
	if (!area->tz_gearbox_num)
		return 0;

	area->tz_gearbox_arr = kcalloc(area->tz_gearbox_num,
				       sizeof(*area->tz_gearbox_arr),
				       GFP_KERNEL);
	if (!area->tz_gearbox_arr)
		return -ENOMEM;

	area->gearbox_sensor_map = kmalloc_array(area->tz_gearbox_num,
						 sizeof(u16), GFP_KERNEL);
	if (!area->gearbox_sensor_map)
		goto mlxsw_thermal_gearbox_sensor_map;

	/* Fill out gearbox sensor mapping array. */
	for (i = 0; i < area->tz_gearbox_num; i++)
		area->gearbox_sensor_map[i] = MLXSW_REG_MTMP_GBOX_INDEX_MIN + i;

	return 0;

mlxsw_thermal_gearbox_sensor_map:
	kfree(area->tz_gearbox_arr);
	return err;
}

static void
mlxsw_thermal_gearboxes_main_fini(struct mlxsw_thermal_area *area)
{
	kfree(area->gearbox_sensor_map);
	kfree(area->tz_gearbox_arr);
}

static int
mlxsw_thermal_gearboxes_init(struct device *dev, struct mlxsw_core *core,
			     struct mlxsw_thermal *thermal,
			     struct mlxsw_thermal_area *area)
{
	struct mlxsw_thermal_module *gearbox_tz;
	int i, err;

	if (!area->tz_gearbox_num)
		return 0;

	for (i = 0; i < area->tz_gearbox_num; i++) {
		gearbox_tz = &area->tz_gearbox_arr[i];
		memcpy(gearbox_tz->trips, default_thermal_trips,
		       sizeof(thermal->trips));
		gearbox_tz->module = i;
		gearbox_tz->parent = thermal;
		gearbox_tz->area = area;
		gearbox_tz->slot_index = area->slot_index;
		err = mlxsw_thermal_gearbox_tz_init(gearbox_tz);
		if (err)
			goto err_thermal_gearbox_tz_init;
	}

	return 0;

err_thermal_gearbox_tz_init:
	for (i--; i >= 0; i--)
		mlxsw_thermal_gearbox_tz_fini(&area->tz_gearbox_arr[i]);
	return err;
}

static void
mlxsw_thermal_gearboxes_fini(struct mlxsw_thermal *thermal,
			     struct mlxsw_thermal_area *area)
{
	int i;

	for (i = area->tz_gearbox_num - 1; i >= 0; i--)
		mlxsw_thermal_gearbox_tz_fini(&area->tz_gearbox_arr[i]);
}

static void
mlxsw_thermal_got_active(struct mlxsw_core *mlxsw_core, u8 slot_index,
			 const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_thermal *thermal = priv;
	struct mlxsw_thermal_area *area = thermal->linecards[slot_index];
	struct mlxsw_env_gearbox_sensors_map map;
	int err;

	err = mlxsw_thermal_modules_init(thermal->bus_info->dev, thermal->core,
					 thermal, area);
	if (err)
		goto err_thermal_linecard_modules_init;

	map.sensor_bit_map = area->gearbox_sensor_map;
	err = mlxsw_env_sensor_map_create(thermal->core, thermal->bus_info,
					  linecard->slot_index, &map);
	if (err)
		goto err_thermal_linecard_env_sensor_map_create;

	err = mlxsw_thermal_gearboxes_init(thermal->bus_info->dev, thermal->core,
					   thermal, area);
	if (err)
		goto err_thermal_linecard_gearboxes_init;

	return;

err_thermal_linecard_gearboxes_init:
	mlxsw_env_sensor_map_destroy(thermal->bus_info,
				     area->gearbox_sensor_map);
err_thermal_linecard_env_sensor_map_create:
	mlxsw_thermal_modules_fini(thermal, area);
err_thermal_linecard_modules_init:
	devm_kfree(thermal->bus_info->dev, area);
}

static void mlxsw_thermal_got_inactive(struct mlxsw_core *mlxsw_core, u8 slot_index,
				       const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_thermal *thermal = priv;
	struct mlxsw_thermal_area *area = thermal->linecards[slot_index];

	mlxsw_thermal_gearboxes_fini(thermal, area);
	mlxsw_env_sensor_map_destroy(thermal->bus_info,
				     area->gearbox_sensor_map);
	mlxsw_thermal_modules_fini(thermal, area);
	devm_kfree(thermal->bus_info->dev, area);
}

static struct mlxsw_linecards_event_ops mlxsw_thermal_event_ops = {
	.got_active = mlxsw_thermal_got_active,
	.got_inactive = mlxsw_thermal_got_inactive,
};

static int mlxsw_thermal_linecards_register(struct mlxsw_core *mlxsw_core,
					    struct mlxsw_thermal *thermal)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);
	int err;

	if (!linecards || !linecards->count)
		return 0;

	thermal->linecards = kcalloc(linecards->count, sizeof(*thermal->linecards),
				     GFP_KERNEL);
	if (!thermal->linecards)
		return -ENOMEM;

	err = mlxsw_linecards_event_ops_register(mlxsw_core,
						 &mlxsw_thermal_event_ops,
						 thermal);
	if (err)
		goto err_linecards_event_ops_register;

err_linecards_event_ops_register:
	kfree(thermal->linecards);
	return err;
}

static void mlxsw_thermal_linecards_unregister(struct mlxsw_thermal *thermal)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(thermal->core);

	if (!linecards || !linecards->count)
		return;

	mlxsw_linecards_event_ops_unregister(thermal->core,
					     &mlxsw_thermal_event_ops, thermal);
	kfree(thermal->linecards);
}

int mlxsw_thermal_init(struct mlxsw_core *core,
		       const struct mlxsw_bus_info *bus_info,
		       struct mlxsw_thermal **p_thermal)
{
	char mfcr_pl[MLXSW_REG_MFCR_LEN] = { 0 };
	enum mlxsw_reg_mfcr_pwm_frequency freq;
	struct device *dev = bus_info->dev;
	struct mlxsw_thermal *thermal;
	u16 tacho_active;
	u8 pwm_active;
	int err, i;

	thermal = devm_kzalloc(dev, sizeof(*thermal),
			       GFP_KERNEL);
	if (!thermal)
		return -ENOMEM;

	thermal->main = devm_kzalloc(dev, sizeof(*thermal->main), GFP_KERNEL);
	if (!thermal->main) {
		err = -ENOMEM;
		goto err_devm_kzalloc;
	}

	thermal->core = core;
	thermal->bus_info = bus_info;
	memcpy(thermal->trips, default_thermal_trips, sizeof(thermal->trips));
	thermal->initializing = true;

	err = mlxsw_reg_query(thermal->core, MLXSW_REG(mfcr), mfcr_pl);
	if (err) {
		dev_err(dev, "Failed to probe PWMs\n");
		goto err_reg_query;
	}
	mlxsw_reg_mfcr_unpack(mfcr_pl, &freq, &tacho_active, &pwm_active);

	for (i = 0; i < MLXSW_MFCR_TACHOS_MAX; i++) {
		if (tacho_active & BIT(i)) {
			char mfsl_pl[MLXSW_REG_MFSL_LEN];

			mlxsw_reg_mfsl_pack(mfsl_pl, i, 0, 0);

			/* We need to query the register to preserve maximum */
			err = mlxsw_reg_query(thermal->core, MLXSW_REG(mfsl),
					      mfsl_pl);
			if (err)
				goto err_reg_query;

			/* set the minimal RPMs to 0 */
			mlxsw_reg_mfsl_tach_min_set(mfsl_pl, 0);
			err = mlxsw_reg_write(thermal->core, MLXSW_REG(mfsl),
					      mfsl_pl);
			if (err)
				goto err_reg_write;
		}
	}
	for (i = 0; i < MLXSW_MFCR_PWMS_MAX; i++) {
		if (pwm_active & BIT(i)) {
			struct thermal_cooling_device *cdev;

			cdev = thermal_cooling_device_register("mlxsw_fan",
							       thermal,
							       &mlxsw_cooling_ops);
			if (IS_ERR(cdev)) {
				err = PTR_ERR(cdev);
				dev_err(dev, "Failed to register cooling device\n");
				goto err_thermal_cooling_device_register;
			}
			thermal->cdevs[i] = cdev;
		}
	}

	/* Initialize cooling levels per PWM state. */
	for (i = 0; i < MLXSW_THERMAL_MAX_STATE; i++)
		thermal->cooling_levels[i] = max(MLXSW_THERMAL_SPEED_MIN_LEVEL,
						 i);

	thermal->polling_delay = bus_info->low_frequency ?
				 MLXSW_THERMAL_SLOW_POLL_INT :
				 MLXSW_THERMAL_POLL_INT;

	thermal->tzdev = thermal_zone_device_register("mlxsw",
						      MLXSW_THERMAL_NUM_TRIPS,
						      MLXSW_THERMAL_TRIP_MASK,
						      thermal,
						      &mlxsw_thermal_ops,
						      &mlxsw_thermal_params, 0,
						      thermal->polling_delay);
	if (IS_ERR(thermal->tzdev)) {
		err = PTR_ERR(thermal->tzdev);
		dev_err(dev, "Failed to register thermal zone\n");
		goto err_thermal_zone_device_register;
	}

	err = mlxsw_thermal_modules_init(dev, core, thermal, thermal->main);
	if (err)
		goto err_thermal_modules_init;

	err = mlxsw_thermal_gearboxes_main_init(dev, core, thermal->main);
	if (err)
		goto err_thermal_gearboxes_main_init;

	err = mlxsw_thermal_gearboxes_init(dev, core, thermal, thermal->main);
	if (err)
		goto err_thermal_gearboxes_init;

	err = mlxsw_thermal_linecards_register(core, thermal);
	if (err)
		goto err_linecards_register;

	thermal->mode = THERMAL_DEVICE_ENABLED;
	thermal->initializing = false;
	*p_thermal = thermal;
	return 0;

err_linecards_register:
err_thermal_gearboxes_init:
	mlxsw_thermal_gearboxes_main_fini(thermal->main);
err_thermal_gearboxes_main_init:
	mlxsw_thermal_modules_fini(thermal, thermal->main);
err_thermal_modules_init:
	if (thermal->tzdev) {
		thermal_zone_device_unregister(thermal->tzdev);
		thermal->tzdev = NULL;
	}
err_thermal_zone_device_register:
err_thermal_cooling_device_register:
	for (i = 0; i < MLXSW_MFCR_PWMS_MAX; i++)
		if (thermal->cdevs[i])
			thermal_cooling_device_unregister(thermal->cdevs[i]);
err_reg_write:
err_reg_query:
	devm_kfree(dev, thermal->main);
err_devm_kzalloc:
	devm_kfree(dev, thermal);
	return err;
}

void mlxsw_thermal_fini(struct mlxsw_thermal *thermal)
{
	int i;

	mlxsw_thermal_linecards_unregister(thermal);
	mlxsw_thermal_gearboxes_fini(thermal, thermal->main);
	mlxsw_thermal_gearboxes_main_fini(thermal->main);
	mlxsw_thermal_modules_fini(thermal, thermal->main);
	if (thermal->tzdev) {
		thermal_zone_device_unregister(thermal->tzdev);
		thermal->tzdev = NULL;
	}

	for (i = 0; i < MLXSW_MFCR_PWMS_MAX; i++) {
		if (thermal->cdevs[i]) {
			thermal_cooling_device_unregister(thermal->cdevs[i]);
			thermal->cdevs[i] = NULL;
		}
	}

	devm_kfree(thermal->bus_info->dev, thermal->main);
	devm_kfree(thermal->bus_info->dev, thermal);
}
