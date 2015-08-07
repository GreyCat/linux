/*
 * AXP20x PMIC Backup/RTC battery driver
 *
 * Copyright 2014-2015 Bruno Prémont <bonbons@linux-vserver.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under  the terms of the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/mfd/axp20x.h>

/* Fields of AXP20X_CHRG_BAK_CTRL */
#define AXP20X_BACKUP_ENABLE         (0x01 << 7)
#define AXP20X_BACKUP_VOLTAGE_MASK   (0x03 << 5)
#define AXP20X_BACKUP_VOLTAGE_3_1V   (0x00 << 5)
#define AXP20X_BACKUP_VOLTAGE_3_0V   (0x01 << 5)
#define AXP20X_BACKUP_VOLTAGE_3_6V   (0x02 << 5)
#define AXP20X_BACKUP_VOLTAGE_2_5V   (0x03 << 5)
#define AXP20X_BACKUP_CURRENT_MASK   0x03
#define AXP20X_BACKUP_CURRENT_50uA   0x00
#define AXP20X_BACKUP_CURRENT_100uA  0x01
#define AXP20X_BACKUP_CURRENT_200uA  0x02
#define AXP20X_BACKUP_CURRENT_400uA  0x03

struct axp20x_rtcbatt_power {
	struct regmap *regmap;
	struct power_supply *supply;
};

static int axp20x_rtcbatt_config(struct platform_device *pdev,
	struct device_node *np, struct regmap *regmap)
{
	int ret = 0, reg, new_reg = 0;
	u32 limit;

	ret = regmap_read(regmap, AXP20X_CHRG_BAK_CTRL, &reg);
	if (ret)
		return ret;

	ret = of_property_read_u32_array(np, "voltage", &limit, 1);
	if (ret != 0)
		goto err;
	switch (limit) {
	case 2500000:
		new_reg |= AXP20X_BACKUP_VOLTAGE_2_5V;
		break;
	case 3000000:
		new_reg |= AXP20X_BACKUP_VOLTAGE_3_0V;
		break;
	case 3100000:
		new_reg |= AXP20X_BACKUP_VOLTAGE_3_1V;
		break;
	case 3600000:
		new_reg |= AXP20X_BACKUP_VOLTAGE_3_6V;
		break;
	default:
		dev_warn(&pdev->dev, "Invalid backup/rtc DT voltage limit %uuV\n", limit);
		ret = -EINVAL;
		goto err;
	}

	ret = of_property_read_u32_array(np, "current", &limit, 1);
	if (ret != 0)
		goto err;
	switch (limit) {
	case 50:
		new_reg |= AXP20X_BACKUP_CURRENT_50uA;
		break;
	case 100:
		new_reg |= AXP20X_BACKUP_CURRENT_100uA;
		break;
	case 200:
		new_reg |= AXP20X_BACKUP_CURRENT_200uA;
		break;
	case 400:
		new_reg |= AXP20X_BACKUP_CURRENT_400uA;
		break;
	default:
		dev_warn(&pdev->dev, "Invalid backup/rtc DT current limit %uuA\n", limit);
		ret = -EINVAL;
		goto err;
	}
	new_reg |= AXP20X_BACKUP_ENABLE;

	ret = regmap_update_bits(regmap, AXP20X_CHRG_BAK_CTRL,
			AXP20X_BACKUP_ENABLE | AXP20X_BACKUP_VOLTAGE_MASK |
			AXP20X_BACKUP_CURRENT_MASK, new_reg);
	if (ret)
		dev_warn(&pdev->dev, "Failed to adjust rtc/backup battery settings: %d\n", ret);

err:
	return ret;
}

static int axp20x_rtcbatt_get_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct axp20x_rtcbatt_power *power = power_supply_get_drvdata(psy);
	int ret = 0, reg;

	ret = regmap_read(power->regmap, AXP20X_CHRG_BAK_CTRL, &reg);
	if (ret < 0)
		return ret;

	switch (psp)  {
	case POWER_SUPPLY_PROP_STATUS:
		if ((reg & AXP20X_BACKUP_ENABLE))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		switch ((reg & AXP20X_BACKUP_VOLTAGE_MASK)) {
		case AXP20X_BACKUP_VOLTAGE_2_5V:
			val->intval = 2500000; break;
		case AXP20X_BACKUP_VOLTAGE_3_0V:
			val->intval = 3000000; break;
		case AXP20X_BACKUP_VOLTAGE_3_1V:
			val->intval = 3100000; break;
		case AXP20X_BACKUP_VOLTAGE_3_6V:
			val->intval = 3600000; break;
		default:
			val->intval = 0;
		}
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		switch ((reg & AXP20X_BACKUP_CURRENT_MASK)) {
		case AXP20X_BACKUP_CURRENT_50uA:
			val->intval = 50; break;
		case AXP20X_BACKUP_CURRENT_100uA:
			val->intval = 100; break;
		case AXP20X_BACKUP_CURRENT_200uA:
			val->intval = 200; break;
		case AXP20X_BACKUP_CURRENT_400uA:
			val->intval = 400; break;
		default:
			val->intval = 0;
		}
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int axp20x_rtcbatt_set_prop(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct axp20x_rtcbatt_power *power = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == POWER_SUPPLY_STATUS_CHARGING)
			ret = regmap_update_bits(power->regmap,
						 AXP20X_CHRG_BAK_CTRL,
						 AXP20X_BACKUP_ENABLE,
						 AXP20X_BACKUP_ENABLE);
		else if (val->intval == POWER_SUPPLY_STATUS_NOT_CHARGING)
			ret = regmap_update_bits(power->regmap,
						 AXP20X_CHRG_BAK_CTRL,
						 AXP20X_BACKUP_ENABLE, 0);
		else
			ret = -EINVAL;
		break;

	default:
		ret = -EINVAL;
	}
	return ret;
}

static int axp20x_rtcbatt_prop_writeable(struct power_supply *psy,
					enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_STATUS;
}

static enum power_supply_property axp20x_rtcbatt_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
};

static const struct power_supply_desc axp20x_rtcbatt_desc = {
	.name = "axp20x-rtc",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = axp20x_rtcbatt_properties,
	.num_properties = ARRAY_SIZE(axp20x_rtcbatt_properties),
	.get_property = axp20x_rtcbatt_get_prop,
	.set_property = axp20x_rtcbatt_set_prop,
	.property_is_writeable = axp20x_rtcbatt_prop_writeable,
};

static int axp20x_rtcbatt_probe(struct platform_device *pdev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct axp20x_rtcbatt_power *power;
	int ret;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;

	power->regmap = axp20x->regmap;

	ret = axp20x_rtcbatt_config(pdev, pdev->dev.of_node, axp20x->regmap);
	if (ret)
		return ret;

	psy_cfg.of_node = pdev->dev.of_node;
	psy_cfg.drv_data = power;

	power->supply = devm_power_supply_register(&pdev->dev,
					&axp20x_rtcbatt_desc, &psy_cfg);
	if (IS_ERR(power->supply))
		return PTR_ERR(power->supply);

	return 0;
}

static const struct of_device_id axp20x_rtcbatt_match[] = {
	{ .compatible = "x-powers,axp202-rtc-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, axp20x_rtcbatt_match);

static struct platform_driver axp20x_rtcbatt_driver = {
	.probe    = axp20x_rtcbatt_probe,
	.driver   = {
		.name  = "axp20x-rtc-power",
		.of_match_table = axp20x_rtcbatt_match,
	},
};

module_platform_driver(axp20x_rtcbatt_driver);

MODULE_DESCRIPTION("AXP20x PMIC Backup/RTC battery driver");
MODULE_AUTHOR("Bruno Prémont <bonbons@linux-vserver.org>");
MODULE_LICENSE("GPL");
