// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2016-2019 Mellanox Technologies. All rights reserved */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <net/switchdev.h>
#include <linux/types.h>

#include "core.h"
#include "core_env.h"
#include "i2c.h"

static const char mlxsw_m_driver_name[] = "mlxsw_minimal";

#define MLXSW_M_FWREV_MINOR	2000
#define MLXSW_M_FWREV_SUBMINOR	1886

struct mlxsw_m_port;
struct mlxsw_m_area;

struct mlxsw_m {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	u8 base_mac[ETH_ALEN];
	struct mlxsw_m_area *main;
	struct mlxsw_m_area **linecards;
	u8 max_ports;
	u8 max_modules_per_slot;
};

struct mlxsw_m_area {
	struct mlxsw_m *mlxsw_m;
	struct mlxsw_m_port **ports;
	int *module_to_port;
	u8 max_ports;
};

struct mlxsw_m_port {
	struct net_device *dev;
	struct mlxsw_m_area *mlxsw_m_area;
	u8 slot_index;
	u8 local_port;
	u8 module;
};

static int mlxsw_m_port_dummy_open_stop(struct net_device *dev)
{
	return 0;
}

static const struct net_device_ops mlxsw_m_port_netdev_ops = {
	.ndo_open		= mlxsw_m_port_dummy_open_stop,
	.ndo_stop		= mlxsw_m_port_dummy_open_stop,
};

static void mlxsw_m_module_get_drvinfo(struct net_device *dev,
				       struct ethtool_drvinfo *drvinfo)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(dev);
	struct mlxsw_m_area *mlxsw_m_area = mlxsw_m_port->mlxsw_m_area;
	struct mlxsw_m *mlxsw_m = mlxsw_m_area->mlxsw_m;

	strlcpy(drvinfo->driver, mlxsw_m->bus_info->device_kind,
		sizeof(drvinfo->driver));
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		 "%d.%d.%d",
		 mlxsw_m->bus_info->fw_rev.major,
		 mlxsw_m->bus_info->fw_rev.minor,
		 mlxsw_m->bus_info->fw_rev.subminor);
	strlcpy(drvinfo->bus_info, mlxsw_m->bus_info->device_name,
		sizeof(drvinfo->bus_info));
}

static int mlxsw_m_get_module_info(struct net_device *netdev,
				   struct ethtool_modinfo *modinfo)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_m_area *mlxsw_m_area = mlxsw_m_port->mlxsw_m_area;
	struct mlxsw_core *core = mlxsw_m_area->mlxsw_m->core;

	return mlxsw_env_get_module_info(core, mlxsw_m_port->slot_index,
					 mlxsw_m_port->module, modinfo);
}

static int
mlxsw_m_get_module_eeprom(struct net_device *netdev, struct ethtool_eeprom *ee,
			  u8 *data)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_m_area *mlxsw_m_area = mlxsw_m_port->mlxsw_m_area;
	struct mlxsw_core *core = mlxsw_m_area->mlxsw_m->core;

	return mlxsw_env_get_module_eeprom(netdev, core, mlxsw_m_port->slot_index,
					   mlxsw_m_port->module, ee, data);
}

static const struct ethtool_ops mlxsw_m_port_ethtool_ops = {
	.get_drvinfo		= mlxsw_m_module_get_drvinfo,
	.get_module_info	= mlxsw_m_get_module_info,
	.get_module_eeprom	= mlxsw_m_get_module_eeprom,
};

static int
mlxsw_m_port_dev_addr_get(struct mlxsw_m_port *mlxsw_m_port)
{
	struct mlxsw_m_area *mlxsw_m_area = mlxsw_m_port->mlxsw_m_area;
	struct mlxsw_m *mlxsw_m = mlxsw_m_area->mlxsw_m;
	struct net_device *dev = mlxsw_m_port->dev;
	char ppad_pl[MLXSW_REG_PPAD_LEN];
	int err;

	mlxsw_reg_ppad_pack(ppad_pl, false, 0);
	err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(ppad), ppad_pl);
	if (err)
		return err;
	mlxsw_reg_ppad_mac_memcpy_from(ppad_pl, dev->dev_addr);
	/* The last byte value in base mac address is guaranteed
	 * to be such it does not overflow when adding local_port
	 * value.
	 */
	dev->dev_addr[ETH_ALEN - 1] = mlxsw_m_port->module + 1;
	/* Add MAC address offset for slot. */
	if (mlxsw_m_port->slot_index)
		dev->dev_addr[ETH_ALEN - 1] += (mlxsw_m_port->slot_index - 1) *
					       mlxsw_m->max_modules_per_slot;
	return 0;
}

static void mlxsw_m_port_switchdev_init(struct mlxsw_m_port *mlxsw_m_port)
{
}

static void mlxsw_m_port_switchdev_fini(struct mlxsw_m_port *mlxsw_m_port)
{
}

static int mlxsw_m_fw_rev_validate(struct mlxsw_m *mlxsw_m)
{
	const struct mlxsw_fw_rev *rev = &mlxsw_m->bus_info->fw_rev;

	dev_info(mlxsw_m->bus_info->dev, "The firmware version %d.%d.%d\n",
		 rev->major, rev->minor, rev->subminor);
	/* Validate driver & FW are compatible */
	if ((rev->minor > MLXSW_M_FWREV_MINOR) ||
	    (rev->minor == MLXSW_M_FWREV_MINOR &&
	     rev->subminor >= MLXSW_M_FWREV_SUBMINOR))
		return 0;

	dev_info(mlxsw_m->bus_info->dev, "The firmware version %d.%d.%d is incompatible with the driver (required >= %d.%d.%d)\n",
		 rev->major, rev->minor, rev->subminor, rev->major,
		 MLXSW_M_FWREV_MINOR, MLXSW_M_FWREV_SUBMINOR);

	return -EINVAL;
}

static int
mlxsw_m_port_create(struct mlxsw_m_area *mlxsw_m_area, u8 slot_index,
		    u8 local_port, u8 module)
{
	struct mlxsw_m *mlxsw_m = mlxsw_m_area->mlxsw_m;
	struct mlxsw_m_port *mlxsw_m_port;
	struct net_device *dev;
	int err;

	err = mlxsw_core_port_init(mlxsw_m->core, local_port);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Port %d: Failed to init core port\n",
			local_port);
		return err;
	}

	dev = alloc_etherdev(sizeof(struct mlxsw_m_port));
	if (!dev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}

	SET_NETDEV_DEV(dev, mlxsw_m->bus_info->dev);
	mlxsw_m_port = netdev_priv(dev);
	mlxsw_m_port->dev = dev;
	mlxsw_m_port->mlxsw_m_area = mlxsw_m_area;
	mlxsw_m_port->slot_index = slot_index;
	mlxsw_m_port->local_port = module;
	mlxsw_m_port->module = module;

	dev->netdev_ops = &mlxsw_m_port_netdev_ops;
	dev->ethtool_ops = &mlxsw_m_port_ethtool_ops;

	err = mlxsw_m_port_dev_addr_get(mlxsw_m_port);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Port %d: Unable to get port mac address\n",
			mlxsw_m_port->local_port);
		goto err_dev_addr_get;
	}

	netif_carrier_off(dev);
	mlxsw_m_port_switchdev_init(mlxsw_m_port);
	mlxsw_m_area->ports[local_port] = mlxsw_m_port;
	err = register_netdev(dev);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Port %d: Failed to register netdev\n",
			mlxsw_m_port->local_port);
		goto err_register_netdev;
	}

	return 0;

err_register_netdev:
	mlxsw_m_area->ports[local_port] = NULL;
	mlxsw_m_port_switchdev_fini(mlxsw_m_port);
	free_netdev(dev);
err_dev_addr_get:
err_alloc_etherdev:
	mlxsw_core_port_fini(mlxsw_m->core, local_port);
	return err;
}

static void mlxsw_m_port_remove(struct mlxsw_m_area *mlxsw_m_area, u8 local_port)
{
	struct mlxsw_m_port *mlxsw_m_port = mlxsw_m_area->ports[local_port];
	struct mlxsw_m *mlxsw_m = mlxsw_m_area->mlxsw_m;

	mlxsw_core_port_clear(mlxsw_m->core, local_port, mlxsw_m);
	unregister_netdev(mlxsw_m_port->dev); /* This calls ndo_stop */
	mlxsw_m_area->ports[local_port] = NULL;
	mlxsw_m_port_switchdev_fini(mlxsw_m_port);
	free_netdev(mlxsw_m_port->dev);
	mlxsw_core_port_fini(mlxsw_m->core, local_port);
}

static int mlxsw_m_ports_create(struct mlxsw_m_area *mlxsw_m_area, u8 slot_index)
{
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	int i, err;

	mlxsw_reg_mgpir_pack(mgpir_pl, slot_index);
	err = mlxsw_reg_query(mlxsw_m_area->mlxsw_m->core, MLXSW_REG(mgpir),
			      mgpir_pl);
	if (err)
		return err;

	if (slot_index)
		mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
				       &mlxsw_m_area->max_ports, NULL, NULL);
	else
		mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
				       &mlxsw_m_area->max_ports, NULL,
				       &mlxsw_m_area->mlxsw_m->max_modules_per_slot);

	if (!mlxsw_m_area->max_ports)
		return 0;

	mlxsw_m_area->ports = kcalloc(mlxsw_m_area->max_ports,
				      sizeof(*mlxsw_m_area->ports), GFP_KERNEL);
	if (!mlxsw_m_area->ports)
		return -ENOMEM;

	mlxsw_m_area->module_to_port = kmalloc_array(mlxsw_m_area->max_ports,
						     sizeof(int), GFP_KERNEL);
	if (!mlxsw_m_area->module_to_port) {
		err = -ENOMEM;
		goto err_module_to_port_alloc;
	}

	/* Create port objects for each valid entry */
	for (i = 0; i < mlxsw_m_area->max_ports; i++) {
		mlxsw_m_area->module_to_port[i] = i;
		err = mlxsw_m_port_create(mlxsw_m_area, slot_index,
					  mlxsw_m_area->module_to_port[i], i);
		if (err)
			goto err_module_to_port_create;
	}

	return 0;

err_module_to_port_create:
	for (i--; i >= 0; i--)
		mlxsw_m_port_remove(mlxsw_m_area,
				    mlxsw_m_area->module_to_port[i]);
	kfree(mlxsw_m_area->module_to_port);
err_module_to_port_alloc:
	kfree(mlxsw_m_area->ports);
	return err;
}

static void mlxsw_m_ports_remove(struct mlxsw_m_area *mlxsw_m_area)
{
	int i;

	for (i = 0; i < mlxsw_m_area->max_ports; i++)
		mlxsw_m_port_remove(mlxsw_m_area, mlxsw_m_area->module_to_port[i]);

	kfree(mlxsw_m_area->module_to_port);
	kfree(mlxsw_m_area->ports);
}

static void mlxsw_m_sys_event_handler(struct mlxsw_core *mlxsw_core)
{
	struct mlxsw_m *mlxsw_m = mlxsw_core_driver_priv(mlxsw_core);
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);
	char mddq_pl[MLXSW_REG_MDDQ_LEN];
	int i, err;

	/* Handle line cards, for which active status has been changed. */
	for (i = 1; i <= linecards->count; i++) {
		mlxsw_reg_mddq_pack(mddq_pl, MLXSW_REG_MDDQ_QUERY_TYPE_SLOT_INFO,
				    false, i);
		err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(mddq), mddq_pl);
		if (err)
			dev_err(mlxsw_m->bus_info->dev, "Fail to query MDDQ register for slot %d\n",
				i);

		mlxsw_linecard_status_process(mlxsw_m->core, mddq_pl);
	}
}

static void
mlxsw_m_got_ready(struct mlxsw_core *mlxsw_core, u8 slot_index,
		  const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_m *mlxsw_m = priv;
	struct mlxsw_m_area *mlxsw_m_area = mlxsw_m->linecards[slot_index - 1];
	int err;

	err = mlxsw_m_ports_create(mlxsw_m_area, slot_index);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to create ports for line card at slot %d\n",
			slot_index);
	}
}

static void
mlxsw_m_got_unready(struct mlxsw_core *mlxsw_core, u8 slot_index,
		    const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_m *mlxsw_m = priv;
	struct mlxsw_m_area *mlxsw_m_area = mlxsw_m->linecards[slot_index - 1];

	mlxsw_m_ports_remove(mlxsw_m_area);
}

static struct mlxsw_linecards_event_ops mlxsw_m_event_ops = {
	.got_ready = mlxsw_m_got_ready,
	.got_unready = mlxsw_m_got_unready,
};

static int mlxsw_m_linecards_register(struct mlxsw_m *mlxsw_m)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_m->core);
	int err;

	if (!linecards || !linecards->count)
		return 0;

	mlxsw_m->linecards = kcalloc(linecards->count, sizeof(*mlxsw_m->linecards),
				     GFP_KERNEL);
	if (!mlxsw_m->linecards)
		return -ENOMEM;

	err = mlxsw_linecards_event_ops_register(mlxsw_m->core,
						 &mlxsw_m_event_ops,
						 mlxsw_m);
	if (err)
		goto err_linecards_event_ops_register;

	return 0;

err_linecards_event_ops_register:
	kfree(mlxsw_m->linecards);
	return err;
}

static void mlxsw_m_linecards_unregister(struct mlxsw_m *mlxsw_m)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_m->core);

	if (!linecards || !linecards->count)
		return;

	mlxsw_linecards_event_ops_unregister(mlxsw_m->core,
					     &mlxsw_m_event_ops, mlxsw_m);
	kfree(mlxsw_m->linecards);
}

static int mlxsw_m_init(struct mlxsw_core *mlxsw_core,
			const struct mlxsw_bus_info *mlxsw_bus_info)
{
	struct mlxsw_m *mlxsw_m = mlxsw_core_driver_priv(mlxsw_core);
	int err;

	mlxsw_m->main = kzalloc(sizeof(*mlxsw_m->main), GFP_KERNEL);
	if (!mlxsw_m->main)
		return -ENOMEM;

	mlxsw_m->core = mlxsw_core;
	mlxsw_m->bus_info = mlxsw_bus_info;
	mlxsw_m->main->mlxsw_m = mlxsw_m;

	err = mlxsw_m_fw_rev_validate(mlxsw_m);
	if (err)
		return err;

	err = mlxsw_m_ports_create(mlxsw_m->main, 0);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to create ports\n");
		return err;
	}

	err = mlxsw_m_linecards_register(mlxsw_m);
	if (err)
		goto err_linecards_register;

	return 0;

err_linecards_register:
	mlxsw_m_ports_remove(mlxsw_m->main);
	return err;
}

static void mlxsw_m_fini(struct mlxsw_core *mlxsw_core)
{
	struct mlxsw_m *mlxsw_m = mlxsw_core_driver_priv(mlxsw_core);

	mlxsw_m_linecards_unregister(mlxsw_m);
	mlxsw_m_ports_remove(mlxsw_m->main);
}

static const struct mlxsw_config_profile mlxsw_m_config_profile;

static struct mlxsw_driver mlxsw_m_driver = {
	.kind			= mlxsw_m_driver_name,
	.priv_size		= sizeof(struct mlxsw_m),
	.init			= mlxsw_m_init,
	.fini			= mlxsw_m_fini,
	.sys_event_handler	= mlxsw_m_sys_event_handler,
	.profile		= &mlxsw_m_config_profile,
	.res_query_enabled	= true,
};

static const struct i2c_device_id mlxsw_m_i2c_id[] = {
	{ "mlxsw_minimal", 0},
	{ },
};

static struct i2c_driver mlxsw_m_i2c_driver = {
	.driver.name = "mlxsw_minimal",
	.class = I2C_CLASS_HWMON,
	.id_table = mlxsw_m_i2c_id,
};

static int __init mlxsw_m_module_init(void)
{
	int err;

	err = mlxsw_core_driver_register(&mlxsw_m_driver);
	if (err)
		return err;

	err = mlxsw_i2c_driver_register(&mlxsw_m_i2c_driver);
	if (err)
		goto err_i2c_driver_register;

	return 0;

err_i2c_driver_register:
	mlxsw_core_driver_unregister(&mlxsw_m_driver);

	return err;
}

static void __exit mlxsw_m_module_exit(void)
{
	mlxsw_i2c_driver_unregister(&mlxsw_m_i2c_driver);
	mlxsw_core_driver_unregister(&mlxsw_m_driver);
}

module_init(mlxsw_m_module_init);
module_exit(mlxsw_m_module_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Vadim Pasternak <vadimp@mellanox.com>");
MODULE_DESCRIPTION("Mellanox minimal driver");
MODULE_DEVICE_TABLE(i2c, mlxsw_m_i2c_id);
