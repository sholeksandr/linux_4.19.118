// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2015-2018 Mellanox Technologies. All rights reserved */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/sfp.h>

#include "core.h"
#include "core_env.h"

#define MLXSW_HWMON_SENSORS_MAX_COUNT 64
#define MLXSW_HWMON_MODULES_MAX_COUNT 64
#define MLXSW_HWMON_GEARBOXES_MAX_COUNT 32

#define MLXSW_HWMON_ATTR_PER_SENSOR 3
#define MLXSW_HWMON_ATTR_PER_MODULE 7
#define MLXSW_HWMON_ATTR_PER_GEARBOX 4
#define MLXSW_HWMON_DEV_NAME_LEN_MAX 16

#define MLXSW_HWMON_ATTR_COUNT (MLXSW_HWMON_SENSORS_MAX_COUNT * MLXSW_HWMON_ATTR_PER_SENSOR + \
				MLXSW_HWMON_MODULES_MAX_COUNT * MLXSW_HWMON_ATTR_PER_MODULE + \
				MLXSW_HWMON_GEARBOXES_MAX_COUNT * MLXSW_HWMON_ATTR_PER_GEARBOX + \
				MLXSW_MFCR_TACHOS_MAX + MLXSW_MFCR_PWMS_MAX)

struct mlxsw_hwmon_attr {
	struct device_attribute dev_attr;
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev;
	unsigned int type_index;
	char name[32];
};

static int
mlxsw_hwmon_get_attr_index(int index, int count, u16 *gearbox_sensor_map)
{
	if (index >= count)
		return gearbox_sensor_map[index % count];

	return index;
}

struct mlxsw_hwmon_dev {
	char name[MLXSW_HWMON_DEV_NAME_LEN_MAX];
	struct mlxsw_hwmon *hwmon;
	struct device *hwmon_dev;
	struct attribute_group group;
	const struct attribute_group *groups[2];
	struct attribute *attrs[MLXSW_HWMON_ATTR_COUNT + 1];
	struct mlxsw_hwmon_attr hwmon_attrs[MLXSW_HWMON_ATTR_COUNT];
	unsigned int attrs_count;
	u8 sensor_count;
	u8 module_sensor_max;
	u16 *gearbox_sensor_map;
	u8 slot_index;
};

struct mlxsw_hwmon {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	struct mlxsw_hwmon_dev *main;
	struct mlxsw_hwmon_dev **linecards;
};

static ssize_t mlxsw_hwmon_temp_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int temp, index;
	int err;

	index = mlxsw_hwmon_get_attr_index(mlxsw_hwmon_attr->type_index,
					   mlxsw_hwmon_dev->module_sensor_max,
					   mlxsw_hwmon_dev->gearbox_sensor_map);
	mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index, index, false,
			    false);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query temp sensor\n");
		return err;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, &temp, NULL, NULL);
	return sprintf(buf, "%d\n", temp);
}

static ssize_t mlxsw_hwmon_temp_max_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int temp_max, index;
	int err;

	index = mlxsw_hwmon_get_attr_index(mlxsw_hwmon_attr->type_index,
					   mlxsw_hwmon_dev->module_sensor_max,
					   mlxsw_hwmon_dev->gearbox_sensor_map);
	mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index, index, false,
			    false);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query temp sensor\n");
		return err;
	}
	mlxsw_reg_mtmp_unpack(mtmp_pl, NULL, &temp_max, NULL);
	return sprintf(buf, "%d\n", temp_max);
}

static ssize_t mlxsw_hwmon_temp_rst_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	unsigned long val;
	int index;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;
	if (val != 1)
		return -EINVAL;

	index = mlxsw_hwmon_get_attr_index(mlxsw_hwmon_attr->type_index,
					   mlxsw_hwmon_dev->module_sensor_max,
					   mlxsw_hwmon_dev->gearbox_sensor_map);

	mlxsw_reg_mtmp_slot_index_set(mtmp_pl, mlxsw_hwmon_dev->slot_index);
	mlxsw_reg_mtmp_sensor_index_set(mtmp_pl, index);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err)
		return err;
	mlxsw_reg_mtmp_mte_set(mtmp_pl, true);
	mlxsw_reg_mtmp_mtr_set(mtmp_pl, true);
	err = mlxsw_reg_write(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to reset temp sensor history\n");
		return err;
	}
	return len;
}

static ssize_t mlxsw_hwmon_fan_rpm_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfsm_pl[MLXSW_REG_MFSM_LEN];
	int err;

	mlxsw_reg_mfsm_pack(mfsm_pl, mlxsw_hwmon_attr->type_index);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mfsm), mfsm_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query fan\n");
		return err;
	}
	return sprintf(buf, "%u\n", mlxsw_reg_mfsm_rpm_get(mfsm_pl));
}

static ssize_t mlxsw_hwmon_fan_fault_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char fore_pl[MLXSW_REG_FORE_LEN];
	bool fault;
	int err;

	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(fore), fore_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query fan\n");
		return err;
	}
	mlxsw_reg_fore_unpack(fore_pl, mlxsw_hwmon_attr->type_index, &fault);

	return sprintf(buf, "%u\n", fault);
}

static ssize_t mlxsw_hwmon_pwm_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfsc_pl[MLXSW_REG_MFSC_LEN];
	int err;

	mlxsw_reg_mfsc_pack(mfsc_pl, mlxsw_hwmon_attr->type_index, 0);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mfsc), mfsc_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to query PWM\n");
		return err;
	}
	return sprintf(buf, "%u\n",
		       mlxsw_reg_mfsc_pwm_duty_cycle_get(mfsc_pl));
}

static ssize_t mlxsw_hwmon_pwm_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfsc_pl[MLXSW_REG_MFSC_LEN];
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;
	if (val > 255)
		return -EINVAL;

	mlxsw_reg_mfsc_pack(mfsc_pl, mlxsw_hwmon_attr->type_index, val);
	err = mlxsw_reg_write(mlxsw_hwmon->core, MLXSW_REG(mfsc), mfsc_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to write PWM\n");
		return err;
	}
	return len;
}

static ssize_t mlxsw_hwmon_module_temp_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	u8 module;
	int temp;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index,
			    MLXSW_REG_MTMP_MODULE_INDEX_MIN + module, false,
			    false);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp), mtmp_pl);
	if (err)
		return err;
	mlxsw_reg_mtmp_unpack(mtmp_pl, &temp, NULL, NULL);

	return sprintf(buf, "%d\n", temp);
}

static ssize_t mlxsw_hwmon_module_temp_fault_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtbr_pl[MLXSW_REG_MTBR_LEN] = {0};
	u8 module, fault;
	u16 temp;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	mlxsw_reg_mtbr_pack(mtbr_pl, mlxsw_hwmon_dev->slot_index,
			    MLXSW_REG_MTBR_BASE_MODULE_INDEX + module, 1);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtbr), mtbr_pl);
	if (err) {
		dev_err(dev, "Failed to query module temperature sensor\n");
		return err;
	}

	mlxsw_reg_mtbr_temp_unpack(mtbr_pl, 0, &temp, NULL);

	/* Update status and temperature cache. */
	switch (temp) {
	case MLXSW_REG_MTBR_BAD_SENS_INFO:
		/* Untrusted cable is connected. Reading temperature from its
		 * sensor is faulty.
		 */
		fault = 1;
		break;
	case MLXSW_REG_MTBR_NO_CONN:
	case MLXSW_REG_MTBR_NO_TEMP_SENS:
	case MLXSW_REG_MTBR_INDEX_NA:
	default:
		fault = 0;
		break;
	}

	return sprintf(buf, "%u\n", fault);
}

static ssize_t
mlxsw_hwmon_module_temp_critical_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	u8 module;
	int temp;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	err = mlxsw_env_module_temp_thresholds_get(mlxsw_hwmon->core,
						   mlxsw_hwmon_dev->slot_index,
						   module, SFP_TEMP_HIGH_WARN,
						   &temp);
	if (err) {
		dev_err(dev, "Failed to query module temperature thresholds\n");
		return err;
	}

	return sprintf(buf, "%u\n", temp);
}

static ssize_t
mlxsw_hwmon_module_temp_emergency_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	u8 module;
	int temp;
	int err;

	module = mlxsw_hwmon_attr->type_index - mlxsw_hwmon_dev->sensor_count;
	err = mlxsw_env_module_temp_thresholds_get(mlxsw_hwmon->core,
						   mlxsw_hwmon_dev->slot_index,
						   module, SFP_TEMP_HIGH_ALARM,
						   &temp);
	if (err) {
		dev_err(dev, "Failed to query module temperature thresholds\n");
		return err;
	}

	return sprintf(buf, "%u\n", temp);
}

static ssize_t
mlxsw_hwmon_module_temp_label_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev;

	mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	if (strlen(mlxsw_hwmon_dev->name))
		return sprintf(buf, "%s front panel %03u\n", mlxsw_hwmon_dev->name,
			       mlxsw_hwmon_attr->type_index);
	else
		return sprintf(buf, "front panel %03u\n",
			       mlxsw_hwmon_attr->type_index);
}

static ssize_t
mlxsw_hwmon_gbox_temp_label_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr =
			container_of(attr, struct mlxsw_hwmon_attr, dev_attr);
	struct mlxsw_hwmon_dev *mlxsw_hwmon_dev = mlxsw_hwmon_attr->mlxsw_hwmon_dev;
	int index = mlxsw_hwmon_attr->type_index -
		    mlxsw_hwmon_dev->module_sensor_max + 1;

	if (strlen(mlxsw_hwmon_dev->name))
		return sprintf(buf, "%s gearbox %03u\n", mlxsw_hwmon_dev->name, index);
	else
		return sprintf(buf, "gearbox %03u\n", index);
}

enum mlxsw_hwmon_attr_type {
	MLXSW_HWMON_ATTR_TYPE_TEMP,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MAX,
	MLXSW_HWMON_ATTR_TYPE_TEMP_RST,
	MLXSW_HWMON_ATTR_TYPE_FAN_RPM,
	MLXSW_HWMON_ATTR_TYPE_FAN_FAULT,
	MLXSW_HWMON_ATTR_TYPE_PWM,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_FAULT,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_CRIT,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_EMERG,
	MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_LABEL,
	MLXSW_HWMON_ATTR_TYPE_TEMP_GBOX_LABEL,
};

static void mlxsw_hwmon_attr_add(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev,
				 enum mlxsw_hwmon_attr_type attr_type,
				 unsigned int type_index, unsigned int num)
{
	struct mlxsw_hwmon_attr *mlxsw_hwmon_attr;
	unsigned int attr_index;

	attr_index = mlxsw_hwmon_dev->attrs_count;
	mlxsw_hwmon_attr = &mlxsw_hwmon_dev->hwmon_attrs[attr_index];

	switch (attr_type) {
	case MLXSW_HWMON_ATTR_TYPE_TEMP:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_temp_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_input", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MAX:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_temp_max_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_highest", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_RST:
		mlxsw_hwmon_attr->dev_attr.store = mlxsw_hwmon_temp_rst_store;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0200;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_reset_history", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_FAN_RPM:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_fan_rpm_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "fan%u_input", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_FAN_FAULT:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_fan_fault_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "fan%u_fault", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_PWM:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_pwm_show;
		mlxsw_hwmon_attr->dev_attr.store = mlxsw_hwmon_pwm_store;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0644;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "pwm%u", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE:
		mlxsw_hwmon_attr->dev_attr.show = mlxsw_hwmon_module_temp_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_input", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_FAULT:
		mlxsw_hwmon_attr->dev_attr.show =
					mlxsw_hwmon_module_temp_fault_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_fault", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_CRIT:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_module_temp_critical_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_crit", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_EMERG:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_module_temp_emergency_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_emergency", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_LABEL:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_module_temp_label_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_label", num + 1);
		break;
	case MLXSW_HWMON_ATTR_TYPE_TEMP_GBOX_LABEL:
		mlxsw_hwmon_attr->dev_attr.show =
			mlxsw_hwmon_gbox_temp_label_show;
		mlxsw_hwmon_attr->dev_attr.attr.mode = 0444;
		snprintf(mlxsw_hwmon_attr->name, sizeof(mlxsw_hwmon_attr->name),
			 "temp%u_label", num + 1);
		break;
	default:
		WARN_ON(1);
	}

	mlxsw_hwmon_attr->type_index = type_index;
	mlxsw_hwmon_attr->mlxsw_hwmon_dev = mlxsw_hwmon_dev;
	mlxsw_hwmon_attr->dev_attr.attr.name = mlxsw_hwmon_attr->name;
	sysfs_attr_init(&mlxsw_hwmon_attr->dev_attr.attr);

	mlxsw_hwmon_dev->attrs[attr_index] = &mlxsw_hwmon_attr->dev_attr.attr;
	mlxsw_hwmon_dev->attrs_count++;
}

static int mlxsw_hwmon_temp_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mtcap_pl[MLXSW_REG_MTCAP_LEN] = {0};
	int i;
	int err;

	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtcap), mtcap_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to get number of temp sensors\n");
		return err;
	}
	mlxsw_hwmon_dev->sensor_count = mlxsw_reg_mtcap_sensor_count_get(mtcap_pl);
	for (i = 0; i < mlxsw_hwmon_dev->sensor_count; i++) {
		char mtmp_pl[MLXSW_REG_MTMP_LEN] = {0};

		mlxsw_reg_mtmp_slot_index_set(mtmp_pl,
					      mlxsw_hwmon_dev->slot_index);
		mlxsw_reg_mtmp_sensor_index_set(mtmp_pl, i);
		err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mtmp),
				      mtmp_pl);
		if (err)
			return err;
		mlxsw_reg_mtmp_mte_set(mtmp_pl, true);
		mlxsw_reg_mtmp_mtr_set(mtmp_pl, true);
		err = mlxsw_reg_write(mlxsw_hwmon->core,
				      MLXSW_REG(mtmp), mtmp_pl);
		if (err) {
			dev_err(mlxsw_hwmon->bus_info->dev, "Failed to setup temp sensor number %d\n",
				i);
			return err;
		}
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP, i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MAX, i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_RST, i, i);
	}
	return 0;
}

static int mlxsw_hwmon_fans_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mfcr_pl[MLXSW_REG_MFCR_LEN] = {0};
	enum mlxsw_reg_mfcr_pwm_frequency freq;
	unsigned int type_index;
	unsigned int num;
	u16 tacho_active;
	u8 pwm_active;
	int err;

	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mfcr), mfcr_pl);
	if (err) {
		dev_err(mlxsw_hwmon->bus_info->dev, "Failed to get to probe PWMs and Tachometers\n");
		return err;
	}
	mlxsw_reg_mfcr_unpack(mfcr_pl, &freq, &tacho_active, &pwm_active);
	num = 0;
	for (type_index = 0; type_index < MLXSW_MFCR_TACHOS_MAX; type_index++) {
		if (tacho_active & BIT(type_index)) {
			mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
					     MLXSW_HWMON_ATTR_TYPE_FAN_RPM,
					     type_index, num);
			mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
					     MLXSW_HWMON_ATTR_TYPE_FAN_FAULT,
					     type_index, num++);
		}
	}
	num = 0;
	for (type_index = 0; type_index < MLXSW_MFCR_PWMS_MAX; type_index++) {
		if (pwm_active & BIT(type_index))
			mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
					     MLXSW_HWMON_ATTR_TYPE_PWM,
					     type_index, num++);
	}
	return 0;
}

static int mlxsw_hwmon_module_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	u8 module_sensor_max;
	int i, err;

	if (!mlxsw_core_res_query_enabled(mlxsw_hwmon->core))
		return 0;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
			       &module_sensor_max, NULL, NULL);

	/* Add extra attributes for module temperature. Sensor index is
	 * assigned to sensor_count value, while all indexed before
	 * sensor_count are already utilized by the sensors connected through
	 * mtmp register by mlxsw_hwmon_temp_init().
	 */
	mlxsw_hwmon_dev->module_sensor_max = mlxsw_hwmon_dev->sensor_count +
					     module_sensor_max;
	for (i = mlxsw_hwmon_dev->sensor_count;
	     i < mlxsw_hwmon_dev->module_sensor_max; i++) {
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE, i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_FAULT,
				     i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_CRIT, i,
				     i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_EMERG,
				     i, i);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MODULE_LABEL,
				     i, i);
	}

	return 0;
}

static int
mlxsw_hwmon_gearbox_main_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev,
			      u8 *gbox_num)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	enum mlxsw_reg_mgpir_device_type device_type;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	int i, err;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(mlxsw_hwmon->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, gbox_num, &device_type, NULL, NULL,
			       NULL, NULL);
	if (device_type != MLXSW_REG_MGPIR_DEVICE_TYPE_GEARBOX_DIE)
		*gbox_num = 0;

	/* Skip gearbox sensor mapping array allocation, if no gearboxes are
	 * available.
	 */
	if (!*gbox_num)
		return 0;

	mlxsw_hwmon_dev->gearbox_sensor_map = kmalloc_array(*gbox_num,
							    sizeof(u16),
							    GFP_KERNEL);
	if (!mlxsw_hwmon_dev->gearbox_sensor_map)
		return -ENOMEM;

	/* Fill out gearbox sensor mapping array. */
	for (i = 0; i < *gbox_num; i++)
		mlxsw_hwmon_dev->gearbox_sensor_map[i] =
					MLXSW_REG_MTMP_GBOX_INDEX_MIN + i;

	return 0;
}

static void
mlxsw_hwmon_gearbox_main_fini(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev)
{
	kfree(mlxsw_hwmon_dev->gearbox_sensor_map);
}

static int
mlxsw_hwmon_gearbox_init(struct mlxsw_hwmon_dev *mlxsw_hwmon_dev, u8 gbox_num)
{
	struct mlxsw_hwmon *mlxsw_hwmon = mlxsw_hwmon_dev->hwmon;
	int index, max_index, sensor_index;
	char mtmp_pl[MLXSW_REG_MTMP_LEN];
	int i = 0, err;

	if (!gbox_num)
		return 0;

	index = mlxsw_hwmon_dev->module_sensor_max;
	max_index = mlxsw_hwmon_dev->module_sensor_max + gbox_num;
	while (index < max_index) {
		sensor_index = mlxsw_hwmon_dev->gearbox_sensor_map[i++];
		mlxsw_reg_mtmp_pack(mtmp_pl, mlxsw_hwmon_dev->slot_index,
				    sensor_index, true, true);
		err = mlxsw_reg_write(mlxsw_hwmon->core,
				      MLXSW_REG(mtmp), mtmp_pl);
		if (err) {
			dev_err(mlxsw_hwmon->bus_info->dev, "Failed to setup temp sensor number %d\n",
				sensor_index);
			return err;
		}
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP, index, index);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_MAX, index,
				     index);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_RST, index,
				     index);
		mlxsw_hwmon_attr_add(mlxsw_hwmon_dev,
				     MLXSW_HWMON_ATTR_TYPE_TEMP_GBOX_LABEL,
				     index, index);
		index++;
	}

	return 0;
}

static void
mlxsw_hwmon_got_active(struct mlxsw_core *mlxsw_core, u8 slot_index,
		       const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_hwmon *hwmon = priv;
	struct mlxsw_hwmon_dev *lc = hwmon->linecards[slot_index - 1];
	struct device *dev = hwmon->bus_info->dev;
	struct mlxsw_env_gearbox_sensors_map map;
	int err;

	err = mlxsw_hwmon_module_init(lc);
	if (err)
		goto err_hwmon_linecard_module_init;

	map.sensor_bit_map = lc->gearbox_sensor_map;
	err = mlxsw_env_sensor_map_create(hwmon->core,
					  hwmon->bus_info, slot_index,
					  &map);
	if (err)
		goto err_hwmon_linecard_env_sensor_map_create;

	err = mlxsw_hwmon_gearbox_init(lc, map.sensor_count);
	if (err)
		goto err_hwmon_linecard_gearbox_init;

	lc->groups[0] = &lc->group;
	lc->group.attrs = lc->attrs;
	lc->slot_index = slot_index;
	sprintf(lc->name, "%s#%02u", "linecard", slot_index);
	lc->hwmon_dev = hwmon_device_register_with_groups(dev, (const char *) lc->name,
							  lc, lc->groups);
	if (IS_ERR(lc->hwmon_dev)) {
		err = PTR_ERR(lc->hwmon_dev);
		goto err_hwmon_linecard_register;
	}

	return;

err_hwmon_linecard_register:
err_hwmon_linecard_gearbox_init:
	mlxsw_env_sensor_map_destroy(hwmon->bus_info,
				     lc->gearbox_sensor_map);
err_hwmon_linecard_env_sensor_map_create:
err_hwmon_linecard_module_init:
	return;
}

static void
mlxsw_hwmon_got_inactive(struct mlxsw_core *mlxsw_core, u8 slot_index,
			 const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_hwmon *hwmon = priv;
	struct mlxsw_hwmon_dev *lc = hwmon->linecards[slot_index - 1];

	if (lc->hwmon_dev)
		hwmon_device_unregister(lc->hwmon_dev);
	mlxsw_env_sensor_map_destroy(hwmon->bus_info,
				     lc->gearbox_sensor_map);
	hwmon->linecards[slot_index - 1] = NULL;
}

static struct mlxsw_linecards_event_ops mlxsw_hwmon_event_ops = {
	.got_active = mlxsw_hwmon_got_active,
	.got_inactive = mlxsw_hwmon_got_inactive,
};

static int mlxsw_hwmon_linecards_register(struct mlxsw_hwmon *hwmon)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(hwmon->core);
	int err;

	if (!linecards || !linecards->count)
		return 0;

	hwmon->linecards = kcalloc(linecards->count, sizeof(*hwmon->linecards),
				   GFP_KERNEL);
	if (!hwmon->linecards)
		return -ENOMEM;

	err = mlxsw_linecards_event_ops_register(hwmon->core,
						 &mlxsw_hwmon_event_ops,
						 hwmon);
	if (err)
		goto err_linecards_event_ops_register;

err_linecards_event_ops_register:
	kfree(hwmon->linecards);
	return err;
}

static void mlxsw_hwmon_linecards_unregister(struct mlxsw_hwmon *hwmon)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(hwmon->core);

	if (!linecards || !linecards->count)
		return;

	mlxsw_linecards_event_ops_unregister(hwmon->core,
					     &mlxsw_hwmon_event_ops, hwmon);
	kfree(hwmon->linecards);
}

int mlxsw_hwmon_init(struct mlxsw_core *mlxsw_core,
		     const struct mlxsw_bus_info *mlxsw_bus_info,
		     struct mlxsw_hwmon **p_hwmon)
{
	struct mlxsw_hwmon *mlxsw_hwmon;
	struct device *hwmon_dev;
	u8 gbox_num;
	int err;

	mlxsw_hwmon = kzalloc(sizeof(*mlxsw_hwmon), GFP_KERNEL);
	if (!mlxsw_hwmon)
		return -ENOMEM;
	mlxsw_hwmon->main = kzalloc(sizeof(*mlxsw_hwmon->main), GFP_KERNEL);
	if (!mlxsw_hwmon->main) {
		err = -ENOMEM;
		goto err_hwmon_main_init;
	}
	mlxsw_hwmon->core = mlxsw_core;
	mlxsw_hwmon->bus_info = mlxsw_bus_info;
	mlxsw_hwmon->main->hwmon = mlxsw_hwmon;

	err = mlxsw_hwmon_temp_init(mlxsw_hwmon->main);
	if (err)
		goto err_temp_init;

	err = mlxsw_hwmon_fans_init(mlxsw_hwmon->main);
	if (err)
		goto err_fans_init;

	err = mlxsw_hwmon_module_init(mlxsw_hwmon->main);
	if (err)
		goto err_temp_module_init;

	err = mlxsw_hwmon_gearbox_main_init(mlxsw_hwmon->main, &gbox_num);
	if (err)
		goto err_gearbox_main_init;

	err = mlxsw_hwmon_gearbox_init(mlxsw_hwmon->main, gbox_num);
	if (err)
		goto err_gearbox_init;

	mlxsw_hwmon->main->groups[0] = &mlxsw_hwmon->main->group;
	mlxsw_hwmon->main->group.attrs = mlxsw_hwmon->main->attrs;

	hwmon_dev = hwmon_device_register_with_groups(mlxsw_bus_info->dev,
						      "mlxsw", mlxsw_hwmon->main,
						      mlxsw_hwmon->main->groups);
	if (IS_ERR(hwmon_dev)) {
		err = PTR_ERR(hwmon_dev);
		goto err_hwmon_register;
	}

	err = mlxsw_hwmon_linecards_register(mlxsw_hwmon);
	if (err)
		goto err_linecards_register;

	mlxsw_hwmon->main->hwmon_dev = hwmon_dev;
	*p_hwmon = mlxsw_hwmon;
	return 0;

err_linecards_register:
	hwmon_device_unregister(mlxsw_hwmon->main->hwmon_dev);
err_hwmon_register:
err_gearbox_init:
	mlxsw_hwmon_gearbox_main_fini(mlxsw_hwmon->main);
err_gearbox_main_init:
err_temp_module_init:
err_fans_init:
err_temp_init:
	kfree(mlxsw_hwmon->main);
err_hwmon_main_init:
	kfree(mlxsw_hwmon);
	return err;
}

void mlxsw_hwmon_fini(struct mlxsw_hwmon *mlxsw_hwmon)
{
	mlxsw_hwmon_linecards_unregister(mlxsw_hwmon);
	hwmon_device_unregister(mlxsw_hwmon->main->hwmon_dev);
	mlxsw_hwmon_gearbox_main_fini(mlxsw_hwmon->main);
	kfree(mlxsw_hwmon->main);
	kfree(mlxsw_hwmon);
}
