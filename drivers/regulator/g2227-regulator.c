// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define G2227_POWER_CONTROL	0x04
#define G2227_POWER_OFF		BIT(7)
#define G2227_ENABLE		0x05
#define G2227_ID			0x20

static const unsigned int g2227_dcdc1_voltages[] = {
	3000000, 3100000, 3200000, 3300000,
};

static const unsigned int g2227_ldo_voltages[] = {
	800000, 850000, 900000, 950000, 1000000, 1100000, 1200000, 1300000,
	1500000, 1600000, 1800000, 1900000, 2500000, 2600000, 3000000, 3100000,
};

static const struct regulator_ops g2227_linear_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
};

static const struct regulator_ops g2227_table_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
};

static int g2227_feedback_voltage(struct regulator_dev *rdev)
{
	const struct regulation_constraints *constraints = rdev->constraints;

	if (constraints->min_uV <= 0 || constraints->min_uV != constraints->max_uV)
		return -EINVAL;
	return constraints->min_uV;
}

static const struct regulator_ops g2227_fixed_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_voltage = g2227_feedback_voltage,
};

#define G2227_COMMON(_name, _id, _bit) \
	.name = #_name, .of_match = #_name, .regulators_node = "regulators", \
	.id = _id, .owner = THIS_MODULE, .type = REGULATOR_VOLTAGE, \
	.enable_reg = G2227_ENABLE, .enable_mask = BIT(_bit)

#define G2227_BUCK(_name, _id, _bit, _reg) { \
	G2227_COMMON(_name, _id, _bit), \
	.ops = &g2227_linear_ops, .n_voltages = 32, \
	.min_uV = 800000, .uV_step = 12500, \
	.vsel_reg = _reg, .vsel_mask = GENMASK(4, 0), \
}

static const struct regulator_desc g2227_regulators[] = {
	{
		G2227_COMMON(dcdc1, 0, 7),
		.ops = &g2227_table_ops,
		.n_voltages = ARRAY_SIZE(g2227_dcdc1_voltages),
		.volt_table = g2227_dcdc1_voltages,
		.vsel_reg = 0x13,
		.vsel_mask = GENMASK(7, 6),
	},
	G2227_BUCK(dcdc2, 1, 6, 0x10),
	G2227_BUCK(dcdc3, 2, 5, 0x11),
	{
		G2227_COMMON(dcdc4, 3, 4),
		.ops = &g2227_fixed_ops,
		.n_voltages = 1,
	},
	G2227_BUCK(dcdc5, 4, 3, 0x12),
	G2227_BUCK(dcdc6, 5, 2, 0x13),
	{
		G2227_COMMON(ldo2, 6, 1),
		.ops = &g2227_table_ops,
		.n_voltages = ARRAY_SIZE(g2227_ldo_voltages),
		.volt_table = g2227_ldo_voltages,
		.vsel_reg = 0x14,
		.vsel_mask = GENMASK(7, 4),
	},
	{
		G2227_COMMON(ldo3, 7, 0),
		.ops = &g2227_table_ops,
		.n_voltages = ARRAY_SIZE(g2227_ldo_voltages),
		.volt_table = g2227_ldo_voltages,
		.vsel_reg = 0x14,
		.vsel_mask = GENMASK(3, 0),
	},
};

static int g2227_power_off(struct sys_off_data *data)
{
	struct regmap *regmap = data->cb_data;
	int ret;

	ret = regmap_set_bits(regmap, G2227_POWER_CONTROL, G2227_POWER_OFF);
	if (ret)
		dev_err(data->dev, "power-off request failed: %d\n", ret);
	return NOTIFY_DONE;
}

static const struct regmap_config g2227_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = G2227_ID,
};

static int g2227_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int id;
	int i, ret;

	regmap = devm_regmap_init_i2c(client, &g2227_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "cannot map registers\n");
	ret = regmap_read(regmap, G2227_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read identification\n");
	if ((id & GENMASK(7, 3)) != 0xd8)
		return dev_err_probe(dev, -ENODEV, "unexpected identification\n");
	config.regmap = regmap;
	for (i = 0; i < ARRAY_SIZE(g2227_regulators); i++) {
		rdev = devm_regulator_register(dev, &g2227_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "cannot register %s\n",
					     g2227_regulators[i].name);
	}
	if (device_property_read_bool(dev, "system-power-controller")) {
		ret = devm_register_sys_off_handler(dev,
						    SYS_OFF_MODE_POWER_OFF_PREPARE,
						    SYS_OFF_PRIO_DEFAULT,
						    g2227_power_off, regmap);
		if (ret)
			return dev_err_probe(dev, ret, "cannot register power-off handler\n");
	}
	return 0;
}

static const struct of_device_id g2227_of_match[] = {
	{ .compatible = "gmt,g2227" },
	{ }
};
MODULE_DEVICE_TABLE(of, g2227_of_match);

static struct i2c_driver g2227_driver = {
	.probe = g2227_probe,
	.driver = {
		.name = "g2227-regulator",
		.of_match_table = g2227_of_match,
	},
};
module_i2c_driver(g2227_driver);

MODULE_DESCRIPTION("GMT G2227 regulator driver");
MODULE_AUTHOR("Nguyen Quang Minh <oss@minhng99.eu>");
MODULE_LICENSE("GPL");
