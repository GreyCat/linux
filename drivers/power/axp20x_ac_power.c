/*
 * AXP20x PMIC AC power driver
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
#include <linux/irq.h>
#include <linux/mfd/axp20x.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define DRVNAME "axp20x-ac-power"
/* Fields of AXP20X_PWR_INPUT_STATUS */
#define AXP20X_PWR_STATUS_AC_PRESENT     BIT(7)
#define AXP20X_PWR_STATUS_AC_AVAILABLE   BIT(6)
#define AXP20X_PWR_STATUS_AC_VBUS_SHORT  BIT(1)
#define AXP20X_PWR_STATUS_AC_VBUS_SEL    BIT(0)

/* Fields of AXP20X_ADC_EN1 */
#define AXP20X_ADC_EN1_ACIN_VOLT BIT(5)
#define AXP20X_ADC_EN1_ACIN_CURR BIT(4)

struct axp20x_ac_power {
	struct regmap *regmap;
	struct power_supply *supply;
};

static int axp20x_ac_power_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val)
{
	struct axp20x_ac_power *power = power_supply_get_drvdata(psy);
	unsigned int input;
	int r;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		r = axp20x_read_variable_width(power->regmap,
					       AXP20X_ACIN_V_ADC_H, 12);
		if (r < 0)
			return r;

		val->intval = r * 1700; /* 1 step = 1.7 mV */
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		r = axp20x_read_variable_width(power->regmap,
					       AXP20X_ACIN_I_ADC_H, 12);
		if (r < 0)
			return r;

		val->intval = r * 375; /* 1 step = 0.375 mA */
		return 0;
	default:
		break;
	}

	/* All the properties below need the input-status reg value */
	r = regmap_read(power->regmap, AXP20X_PWR_INPUT_STATUS, &input);
	if (r)
		return r;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !!(input & AXP20X_PWR_STATUS_AC_PRESENT);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = !!(input & AXP20X_PWR_STATUS_AC_AVAILABLE);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property axp20x_ac_power_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
};

static const struct power_supply_desc axp20x_ac_power_desc = {
	.name = "axp20x-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = axp20x_ac_power_properties,
	.num_properties = ARRAY_SIZE(axp20x_ac_power_properties),
	.get_property = axp20x_ac_power_get_property,
};

static irqreturn_t axp20x_irq_ac_over_v(int irq, void *devid)
{
	struct axp20x_ac_power *power = devid;

	dev_warn(&power->supply->dev, "IRQ#%d AC over voltage\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_ac_plugin(int irq, void *devid)
{
	struct axp20x_ac_power *power = devid;

	dev_info(&power->supply->dev, "IRQ#%d AC connected\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_ac_removal(int irq, void *devid)
{
	struct axp20x_ac_power *power = devid;

	dev_info(&power->supply->dev, "IRQ#%d AC disconnected\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static int axp20x_ac_power_probe(struct platform_device *pdev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct axp20x_ac_power *power;
	static const char * const irq_names[] = { "ACIN_PLUGIN",
		"ACIN_REMOVAL", "ACIN_OVER_V" };
	irqreturn_t (*irq_funcs[])(int, void *) = { axp20x_irq_ac_plugin,
		axp20x_irq_ac_removal, axp20x_irq_ac_over_v };
	int i, irq, r;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;

	power->regmap = axp20x->regmap;

	/* Enable ac voltage and current measurement */
	r = regmap_update_bits(power->regmap, AXP20X_ADC_EN1,
			AXP20X_ADC_EN1_ACIN_CURR | AXP20X_ADC_EN1_ACIN_VOLT,
			AXP20X_ADC_EN1_ACIN_CURR | AXP20X_ADC_EN1_ACIN_VOLT);
	if (r)
		return r;

	psy_cfg.of_node = pdev->dev.of_node;
	psy_cfg.drv_data = power;

	power->supply = devm_power_supply_register(&pdev->dev,
					&axp20x_ac_power_desc, &psy_cfg);
	if (IS_ERR(power->supply))
		return PTR_ERR(power->supply);

	/* Request irqs after registering, as irqs may trigger immediately */
	for (i = 0; i < ARRAY_SIZE(irq_names); i++) {
		irq = platform_get_irq_byname(pdev, irq_names[i]);
		if (irq < 0) {
			dev_warn(&pdev->dev, "No IRQ for %s: %d\n",
				 irq_names[i], irq);
			continue;
		}
		irq = regmap_irq_get_virq(axp20x->regmap_irqc, irq);
		r = devm_request_any_context_irq(&pdev->dev, irq,
				irq_funcs[i], 0, DRVNAME, power);
		if (r < 0)
			dev_warn(&pdev->dev, "Error requesting %s IRQ: %d\n",
				 irq_names[i], r);
	}

	return 0;
}

static const struct of_device_id axp20x_ac_power_match[] = {
	{ .compatible = "x-powers,axp202-ac-power-supply" },
	{ }
};
MODULE_DEVICE_TABLE(of, axp20x_ac_power_match);

static struct platform_driver axp20x_ac_power_driver = {
	.probe = axp20x_ac_power_probe,
	.driver = {
		.name = DRVNAME,
		.of_match_table = axp20x_ac_power_match,
	},
};

module_platform_driver(axp20x_ac_power_driver);

MODULE_AUTHOR("Bruno Prémont <bonbons@linux-vserver.org>");
MODULE_DESCRIPTION("AXP20x PMIC AC power supply status driver");
MODULE_LICENSE("GPL");
