/*
 * AC power input driver for X-Powers AXP20x PMICs
 *
 * Copyright 2014-2015 Bruno Prémont <bonbons@linux-vserver.org>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/mfd/axp20x.h>

#define DRVNAME "axp20x-battery-power"
#define MONITOR_DELAY_JIFFIES    (HZ * 60)   /*60 sec */

struct axp20x_battery_power {
	struct regmap *regmap;
	struct power_supply *supply;
	struct delayed_work monitor;
	int health;
	int capacity;
	int percent;
	int charge_user_imax;
	int tbatt_min;
	int tbatt_max;
};

/* Fields of AXP20X_PWR_INPUT_STATUS */
#define AXP20X_PWR_STATUS_AC_PRESENT     (1 << 7)
#define AXP20X_PWR_STATUS_AC_AVAILABLE   (1 << 6)
#define AXP20X_PWR_STATUS_VBUS_PRESENT   (1 << 5)
#define AXP20X_PWR_STATUS_VBUS_AVAILABLE (1 << 4)
#define AXP20X_PWR_STATUS_VBUS_VHOLD     (1 << 3)
#define AXP20X_PWR_STATUS_BAT_CHARGING   (1 << 2)
#define AXP20X_PWR_STATUS_AC_VBUS_SHORT  (1 << 1)
#define AXP20X_PWR_STATUS_AC_VBUS_SEL    (1 << 0)

/* Fields of AXP20X_PWR_OP_MODE */
#define AXP20X_PWR_OP_OVERTEMP             (1 << 7)
#define AXP20X_PWR_OP_CHARGING             (1 << 6)
#define AXP20X_PWR_OP_BATT_PRESENT         (1 << 5)
#define AXP20X_PWR_OP_BATT_ACTIVATED       (1 << 3)
#define AXP20X_PWR_OP_BATT_CHG_CURRENT_LOW (1 << 2)

/* Fields of AXP20X_ADC_EN1 */
#define AXP20X_ADC_EN1_BATT_VOLT BIT(7)
#define AXP20X_ADC_EN1_BATT_CURR BIT(6)
#define AXP20X_ADC_EN1_APS_V  (1 << 1)
#define AXP20X_ADC_EN1_TEMP   (1 << 0)

/* Fields of AXP20X_ADC_RATE */
#define AXP20X_ADR_RATE_MASK    (3 << 6)
#define AXP20X_ADR_RATE_25Hz    (0 << 6)
#define AXP20X_ADR_RATE_50Hz    (1 << 6)
#define AXP20X_ADR_RATE_100Hz   (2 << 6)
#define AXP20X_ADR_RATE_200Hz   (3 << 6)
#define AXP20X_ADR_TS_CURR_MASK (3 << 4)
#define AXP20X_ADR_TS_CURR_20uA (0 << 4)
#define AXP20X_ADR_TS_CURR_40uA (1 << 4)
#define AXP20X_ADR_TS_CURR_60uA (2 << 4)
#define AXP20X_ADR_TS_CURR_80uA (3 << 4)
#define AXP20X_ADR_TS_UNRELATED (1 << 2)
#define AXP20X_ADR_TS_WHEN_MASK (3 << 0)
#define AXP20X_ADR_TS_WHEN_OFF  (0 << 0)
#define AXP20X_ADR_TS_WHEN_CHG  (1 << 0)
#define AXP20X_ADR_TS_WHEN_ADC  (2 << 0)
#define AXP20X_ADR_TS_WHEN_ON   (3 << 0)

/* Fields of AXP20X_VBUS_IPSOUT_MGMT */
#define AXP20X_VBUS_VHOLD_MASK   (7 << 3)
#define AXP20X_VBUS_VHOLD_mV(b)  (4000000 + (((b) >> 3) & 7) * 100000)
#define AXP20X_VBUS_CLIMIT_MASK  (3)
#define AXP20X_VBUC_CLIMIT_900mA (0)
#define AXP20X_VBUC_CLIMIT_500mA (1)
#define AXP20X_VBUC_CLIMIT_100mA (2)
#define AXP20X_VBUC_CLIMIT_NONE  (3)

/* Fields of AXP20X_OFF_CTRL */
#define AXP20X_OFF_CTRL_BATT_MON    (1 << 6)
#define AXP20X_OFF_CTRL_CHGLED_MASK (3 << 4)
#define AXP20X_OFF_CTRL_CHGLED_HR   (0 << 4)
#define AXP20X_OFF_CTRL_CHGLED_1Hz  (1 << 4)
#define AXP20X_OFF_CTRL_CHGLED_4Hz  (2 << 4)
#define AXP20X_OFF_CTRL_CHGLED_LOW  (3 << 4)
#define AXP20X_OFF_CTRL_CHGLED_FIX  (1 << 3)
/* Fields of AXP20X_CHRG_CTRL1 */
#define AXP20X_CHRG_CTRL1_ENABLE    (1 << 7)
#define AXP20X_CHRG_CTRL1_TGT_VOLT  (3 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_1V  (0 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_15V (1 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_2V  (2 << 5)
#define AXP20X_CHRG_CTRL1_TGT_4_36V (3 << 5)
#define AXP20X_CHRG_CTRL1_END_CURR  (1 << 4)
#define AXP20X_CHRG_CTRL1_TGT_CURR  0x0f
/* Fields of AXP20X_CHRG_CTRL2 */
#define AXP20X_CHRG_CTRL2_PRE_MASK  (3 << 6)
#define AXP20X_CHRG_CTRL2_PRE_40MIN (0 << 6)
#define AXP20X_CHRG_CTRL2_PRE_50MIN (1 << 6)
#define AXP20X_CHRG_CTRL2_PRE_60MIN (2 << 6)
#define AXP20X_CHRG_CTRL2_PRE_70MIN (3 << 6)
#define AXP20X_CHRG_CTRL2_CHGLED_FL (1 << 4)
#define AXP20X_CHRG_CTRL2_CHG_MASK  (0 << 6)
#define AXP20X_CHRG_CTRL2_CHG_6H    (0 << 0)
#define AXP20X_CHRG_CTRL2_CHG_8H    (1 << 0)
#define AXP20X_CHRG_CTRL2_CHG_10H   (2 << 6)
#define AXP20X_CHRG_CTRL2_CHG_12H   (3 << 0)
/* Fields of AXP20X_FG_RES */
#define AXP20X_FG_ENABLE   (1 << 7)
#define AXP20X_FG_PERCENT  (0x7f)

static void axp20x_battery_poll(struct axp20x_battery_power *power)
{
	int health, percent, reg, ret;

	/* check and refresh power->health, power->percent */
	health  = POWER_SUPPLY_HEALTH_UNKNOWN;
	percent = 0;

	ret = regmap_read(power->regmap, AXP20X_PWR_OP_MODE, &reg);
	if (ret)
		return;

	if (!(reg & AXP20X_PWR_OP_BATT_PRESENT))
		goto out;

	reg = axp20x_read_variable_width(power->regmap,
					AXP20X_BATT_V_H, 12);
	if (reg >= 0 && reg * 1100 < 2000000)
		health = POWER_SUPPLY_HEALTH_DEAD;
	// TODO: check for 'good' health (unknown but not activating)

	ret = regmap_read(power->regmap, AXP20X_FG_RES, &reg);
	if (ret == 0)
		percent = reg & 0x7f;

	if (power->tbatt_min || power->tbatt_max) {
		/* test for out-of-bound temperature */
		reg = axp20x_read_variable_width(power->regmap,
						AXP20X_BATT_V_H, 12);
		if (reg >= 0 && reg < power->tbatt_min)
			health = POWER_SUPPLY_HEALTH_COLD;
		else if (reg >= 0 && reg > power->tbatt_max)
			health = POWER_SUPPLY_HEALTH_OVERHEAT;
	}


out:
	if (power->health != health || power->percent != percent) {
		power->health = health;
		power->percent = percent;
		if (power->supply)
			power_supply_changed(power->supply);
	}
	return;
/*

	int ret, status1, status2, vbusmgt, adc_cfg, bpercent;
	uint8_t adc[19];

	ret = regmap_read(power->regmap, AXP20X_PWR_INPUT_STATUS, &status1);
	if (ret)
		return ret;
	ret = regmap_read(power->regmap, AXP20X_PWR_OP_MODE, &status2);
	if (ret)
		return ret;

	ret = regmap_read(power->regmap, AXP20X_ADC_RATE, &adc_cfg);
	if (ret)
		return ret;

	if (init == 2) {
		int reg = AXP20X_ADC_EN1_VBUS_V | AXP20X_ADC_EN1_VBUS_C;
		if (!(status1 & AXP20X_PWR_STATUS_AC_VBUS_SHORT))
			reg |= AXP20X_ADC_EN1_ACIN_V | AXP20X_ADC_EN1_ACIN_C;
		if (devdata->battery_name[0])
			reg |= AXP20X_ADC_EN1_BATT_V | AXP20X_ADC_EN1_BATT_C;
		if (devdata->battery_name[0] &&
		    !(adc_cfg & AXP20X_ADR_TS_UNRELATED))
			reg |= AXP20X_ADC_EN1_TEMP;

		regmap_update_bits(axp20x->regmap, AXP20X_ADC_EN1,
			AXP20X_ADC_EN1_ACIN_V | AXP20X_ADC_EN1_ACIN_C |
			AXP20X_ADC_EN1_VBUS_V | AXP20X_ADC_EN1_VBUS_C |
			AXP20X_ADC_EN1_BATT_V | AXP20X_ADC_EN1_BATT_C |
			AXP20X_ADC_EN1_TEMP, reg);
	}

	ret = regmap_read(axp20x->regmap, AXP20X_VBUS_IPSOUT_MGMT, &vbusmgt);
	if (ret)
		return ret;

	ret = regmap_bulk_read(axp20x->regmap, AXP20X_ACIN_V_ADC_H, adc, 8);
	if (ret)
		return ret;
	if (devdata->battery_name[0] && !(adc_cfg & AXP20X_ADR_TS_UNRELATED)) {
		ret = regmap_bulk_read(axp20x->regmap, AXP20X_TS_IN_H, adc+8, 2);
		if (ret)
			return ret;
	}
	if (devdata->battery_name[0]) {
		ret = regmap_bulk_read(axp20x->regmap, AXP20X_PWR_BATT_H, adc+10, 3);
		if (ret)
			return ret;
		ret = regmap_bulk_read(axp20x->regmap, AXP20X_BATT_V_H, adc+13, 6);
		if (ret)
			return ret;
		ret = regmap_read(axp20x->regmap, AXP20X_FG_RES, &bpercent);
		if (ret)
			return ret;
	}

	switch (adc_cfg & AXP20X_ADR_RATE_MASK) {
	case AXP20X_ADR_RATE_200Hz:
		timespec_add_ns(&ts,  5000000); break;
	case AXP20X_ADR_RATE_100Hz:
		timespec_add_ns(&ts, 10000000); break;
	case AXP20X_ADR_RATE_50Hz:
		timespec_add_ns(&ts, 20000000); break;
	case AXP20X_ADR_RATE_25Hz:
	default:
		timespec_add_ns(&ts, 40000000);
	}

	ret = devdata->status1 | (devdata->status2 << 8) |
	      ((devdata->batt_percent & 0x7f) << 16);
	dev_info(axp20x->dev, "Status [init=%d]: 0x%02x 0x%02x 0x%02x [ts=%d.%06ds, next=%d.%06ds]\n", init, devdata->status1, devdata->status2, devdata->vbusmgt, (int)ts.tv_sec, (int)ts.tv_nsec / 1000, (int)devdata->next_check.tv_sec, (int)devdata->next_check.tv_nsec / 1000);
	if (init == 2)
		timespec_add_ns(&ts, 200000000);
	spin_lock(&devdata->lock);
	devdata->next_check = ts;
	devdata->vbusmgt    = vbusmgt;
	devdata->status1    = status1;
	devdata->status2    = status2;
	if (devdata->battery_name[0] && !(adc_cfg & AXP20X_ADR_TS_UNRELATED))
		devdata->tbatt = ((adc[8] << 4) | (adc[9] & 0x0f)) * 800;
	if (devdata->battery_name[0]) {
		devdata->vbatt = ((adc[13] << 4) | (adc[14] & 0x0f)) * 1100;
		if (status1 & AXP20X_PWR_STATUS_BAT_CHARGING)
			devdata->ibatt = ((adc[15] << 4) | (adc[16] & 0x0f));
		else
			devdata->ibatt = ((adc[17] << 4) | (adc[18] & 0x0f));
		devdata->ibatt *= 500;
		devdata->pbatt = ((adc[10] << 16) | (adc[11] << 8) | adc[12]) *
				 55 / 100;
		devdata->batt_percent = bpercent & 0x7f;
	}
	spin_unlock(&devdata->lock);

	if (init == 2 || init == 0)
		return 0;

	if ((ret ^ status1) & AXP20X_PWR_STATUS_BAT_CHARGING) {
		power_supply_changed(&devdata->battery);
	} else if (((ret >> 8) ^ status2) & (AXP20X_PWR_OP_CHARGING |
		   AXP20X_PWR_OP_BATT_PRESENT | AXP20X_PWR_OP_BATT_ACTIVATED |
		   AXP20X_PWR_OP_BATT_CHG_CURRENT_LOW)) {
		power_supply_changed(&devdata->battery);
	} else if (((ret >> 16) & 0x7f) != (bpercent & 0x7f)) {
		power_supply_changed(&devdata->battery);
	}
	return 0; */
}

static void axp20x_battery_monitor(struct work_struct *work)
{
	struct axp20x_battery_power *power = container_of(work,
		struct axp20x_battery_power, monitor.work);

	axp20x_battery_poll(power);
	schedule_delayed_work(&power->monitor, MONITOR_DELAY_JIFFIES);
}

static void axp20x_battery_chg_reconfig(struct power_supply *psy);

static int axp20x_battery_uv_to_temp(struct axp20x_battery_power *devdata,
	int uv)
{
	/* TODO: convert µV to °C */
	return uv;
}

static int axp20x_battery_power_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val)
{
	struct axp20x_battery_power *power = power_supply_get_drvdata(psy);
	int ret, reg;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = regmap_read(power->regmap, AXP20X_CHRG_CTRL1, &reg);
		if (ret)
			return ret;
		val->intval = (reg & AXP20X_CHRG_CTRL1_TGT_CURR) * 100000 +
			      300000;
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		ret = regmap_read(power->regmap, AXP20X_CHRG_CTRL1, &reg);
		if (ret)
			return ret;
		switch (reg & AXP20X_CHRG_CTRL1_TGT_VOLT) {
		case AXP20X_CHRG_CTRL1_TGT_4_1V:
			val->intval = 4100000;
			break;
		case AXP20X_CHRG_CTRL1_TGT_4_15V:
			val->intval = 4150000;
			break;
		case AXP20X_CHRG_CTRL1_TGT_4_2V:
			val->intval = 4200000;
			break;
		case AXP20X_CHRG_CTRL1_TGT_4_36V:
			val->intval = 4360000;
			break;
		default:
			ret = -EINVAL;
		}
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		ret = regmap_read(power->regmap, AXP20X_APS_WARN_L2, &reg);
		if (ret)
			return ret;
		val->intval = 2867200 + 1400 * reg * 4;
		return 0;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;

	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(power->regmap, AXP20X_PWR_OP_MODE, &reg);
		if (ret)
			return ret;
		val->intval = !!(reg & AXP20X_PWR_OP_BATT_PRESENT);
		return 0;

	case POWER_SUPPLY_PROP_STATUS:
		ret = regmap_read(power->regmap, AXP20X_PWR_INPUT_STATUS, &reg);
		if (ret)
			return ret;
		if (reg & AXP20X_PWR_STATUS_BAT_CHARGING) {
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			return 0;
		}
		reg = axp20x_read_variable_width(power->regmap,
						AXP20X_BATT_DISCHRG_I_H, 12);
		if (reg < 0)
			return reg;
		reg *= 500;
		if (reg < 2000 && power->percent == 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (reg < 2000)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = regmap_read(power->regmap, AXP20X_PWR_INPUT_STATUS, &reg);
		if (ret)
			return ret;
		if (reg & AXP20X_PWR_STATUS_BAT_CHARGING)
			reg = axp20x_read_variable_width(power->regmap,
						AXP20X_BATT_CHRG_I_H, 12);
		else
			reg = axp20x_read_variable_width(power->regmap,
						AXP20X_BATT_DISCHRG_I_H, 12);
		if (reg < 0)
			return reg;
		val->intval = reg * 500; /* 1 step = 0.5mA */
		return 0;

	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = power->health;
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		reg = axp20x_read_variable_width(power->regmap,
						AXP20X_BATT_V_H, 12);
		if (reg < 0)
			return reg;
		val->intval = reg * 1100; /* 1 step = 1.1mV */
		return 0;

/*	case POWER_SUPPLY_PROP_POWER_NOW:
		reg = axp20x_read_variable_width(power->regmap,
					AXP20X_PWR_BATT_H, 24);
		if (reg < 0)
			return reg;
		val->intval = reg * 55 / 100;
		return 0; */

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = power->capacity;
		return 0;

	case POWER_SUPPLY_PROP_CAPACITY:
		ret = regmap_read(power->regmap, AXP20X_FG_RES, &reg);
		if (ret)
			return ret;
		val->intval = reg & 0x7f;
		return 0;

	case POWER_SUPPLY_PROP_TEMP:
		reg = axp20x_read_variable_width(power->regmap,
						AXP20X_BATT_V_H, 12);
		if (reg < 0)
			return reg;
		val->intval = axp20x_battery_uv_to_temp(power,
					reg * 800); /* 1 step = 0.8mV */
		return 0;

	case POWER_SUPPLY_PROP_TEMP_ALERT_MIN:
		val->intval = axp20x_battery_uv_to_temp(power,
							power->tbatt_min);
		return 0;

	case POWER_SUPPLY_PROP_TEMP_ALERT_MAX:
		val->intval = axp20x_battery_uv_to_temp(power,
							power->tbatt_max);
		return 0;

	default:
		ret = -EINVAL;
	}
	return ret;
}

static int axp20x_battery_max_chg_current(struct axp20x_battery_power *power)
{
	int status, vbusmgt, ret;

	ret = regmap_read(power->regmap, AXP20X_PWR_INPUT_STATUS, &status);
	if (ret)
		return ret;

	if ((status & AXP20X_PWR_STATUS_AC_PRESENT) &&
	    (status & AXP20X_PWR_STATUS_AC_AVAILABLE)) {
		/* AC available - unrestricted power */
		return power->capacity / 2;
	} else if ((status & AXP20X_PWR_STATUS_VBUS_PRESENT) &&
		   (status & AXP20X_PWR_STATUS_VBUS_AVAILABLE)) {
		/* VBUS available - limited power */
		ret = regmap_read(power->regmap, AXP20X_VBUS_IPSOUT_MGMT, &vbusmgt);
		if (ret)
			return ret;
		switch (vbusmgt & AXP20X_VBUS_CLIMIT_MASK) {
		case AXP20X_VBUC_CLIMIT_100mA:
			return 0;
		case AXP20X_VBUC_CLIMIT_500mA:
			return 300000;
		case AXP20X_VBUC_CLIMIT_900mA:
			return 600000;
		case AXP20X_VBUC_CLIMIT_NONE:
			return power->capacity / 2;
		default:
			return 0;
		}
	} else {
		/* on-battery */
		return 0;
	}
}

static int axp20x_battery_power_set_property(struct power_supply *psy,
	enum power_supply_property psp, const union power_supply_propval *val)
{
	struct axp20x_battery_power *power = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == POWER_SUPPLY_STATUS_CHARGING) {
			ret = axp20x_battery_max_chg_current(power);
			if (ret < 0)
				break;
			if (ret == 0) {
				ret = -EBUSY;
				break;
			}
			ret = regmap_update_bits(power->regmap,
						 AXP20X_PWR_OP_MODE,
						 AXP20X_PWR_OP_CHARGING,
						 AXP20X_PWR_OP_CHARGING);
			if (ret == 0)
				axp20x_battery_chg_reconfig(psy);
		} else if (val->intval == POWER_SUPPLY_STATUS_NOT_CHARGING) {
			ret = regmap_update_bits(power->regmap,
						 AXP20X_PWR_OP_MODE,
						 AXP20X_PWR_OP_CHARGING, 0);
		} else
			ret = -EINVAL;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		/* TODO: adjust AXP20X_APS_WARN_L1 and AXP20X_APS_WARN_L2 accordingly */
		ret = -EINVAL;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		switch (val->intval) {
		case 4100000:
			ret = regmap_update_bits(power->regmap,
						 AXP20X_CHRG_CTRL1,
						 AXP20X_CHRG_CTRL1_TGT_VOLT,
						 AXP20X_CHRG_CTRL1_TGT_4_1V);
			break;
		case 4150000:
			ret = regmap_update_bits(power->regmap,
						 AXP20X_CHRG_CTRL1,
						 AXP20X_CHRG_CTRL1_TGT_VOLT,
						 AXP20X_CHRG_CTRL1_TGT_4_15V);
			break;
		case 4200000:
			ret = regmap_update_bits(power->regmap,
						 AXP20X_CHRG_CTRL1,
						 AXP20X_CHRG_CTRL1_TGT_VOLT,
						 AXP20X_CHRG_CTRL1_TGT_4_2V);
			break;
		case 4360000:
			/* refuse this as it's too much for Li-ion! */
		default:
			ret = -EINVAL;
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		if (((val->intval - 300000) / 100000) > 0x0f)
			ret = -EINVAL;
		else if (val->intval < 300000)
			ret = -EINVAL;
		else {
			power->charge_user_imax = val->intval;
			axp20x_battery_chg_reconfig(psy);
			ret = 0;
		}
		break;

	default:
		ret = -EINVAL;
	}
	return ret;
}

static enum power_supply_property axp20x_battery_power_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	/* POWER_SUPPLY_PROP_POWER_NOW, */
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	/* POWER_SUPPLY_PROP_CHARGE_NOW, */
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_ALERT_MIN,
	POWER_SUPPLY_PROP_TEMP_ALERT_MAX,
};

static int axp20x_battery_power_property_writeable(struct power_supply *psy,
	enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN ||
	       psp == POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN ||
	       psp == POWER_SUPPLY_PROP_CURRENT_MAX ||
	       psp == POWER_SUPPLY_PROP_STATUS;
}

static void axp20x_battery_chg_reconfig(struct power_supply *psy)
{
	struct axp20x_battery_power *power = power_supply_get_drvdata(psy);
	int charge_max, ret;

	charge_max = axp20x_battery_max_chg_current(power);
	if (charge_max < 0)
		return;

	if (charge_max == 0) {
		ret = regmap_update_bits(power->regmap,
					 AXP20X_PWR_OP_MODE,
					 AXP20X_PWR_OP_CHARGING, 0);
	} else {
		if (power->charge_user_imax < charge_max)
			charge_max = power->charge_user_imax;
		if (((charge_max - 300000) / 100000) > 0x0f)
			charge_max = 300000 + 0x0f * 100000;
		ret = regmap_update_bits(power->regmap,
					 AXP20X_CHRG_CTRL1,
					 AXP20X_CHRG_CTRL1_TGT_CURR,
					(charge_max - 300000) / 100000);
		ret = regmap_update_bits(power->regmap,
					 AXP20X_PWR_OP_MODE,
					 AXP20X_PWR_OP_CHARGING,
					 AXP20X_PWR_OP_CHARGING);
	}
	power_supply_changed(psy);
}

static const struct power_supply_desc axp20x_battery_power_desc = {
	.name = "axp20x-batt",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = axp20x_battery_power_properties,
	.num_properties = ARRAY_SIZE(axp20x_battery_power_properties) - 3,
	.property_is_writeable = axp20x_battery_power_property_writeable,
	.get_property = axp20x_battery_power_get_property,
	.set_property = axp20x_battery_power_set_property,
	.external_power_changed = axp20x_battery_chg_reconfig,
};

static const struct power_supply_desc axp20x_battery_ts_power_desc = {
	.name = "axp20x-batt",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = axp20x_battery_power_properties,
	.num_properties = ARRAY_SIZE(axp20x_battery_power_properties),
	.property_is_writeable = axp20x_battery_power_property_writeable,
	.get_property = axp20x_battery_power_get_property,
	.set_property = axp20x_battery_power_set_property,
	.external_power_changed = axp20x_battery_chg_reconfig,
};

static irqreturn_t axp20x_irq_batt_plugin(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	power->health = POWER_SUPPLY_HEALTH_UNKNOWN; // Check voltage
	regmap_update_bits(power->regmap, AXP20X_PWR_OP_MODE,
			AXP20X_PWR_OP_CHARGING, AXP20X_PWR_OP_CHARGING);
	dev_info(&power->supply->dev, "IRQ#%d Battery connected\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_removal(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	power->health = POWER_SUPPLY_HEALTH_UNKNOWN;
	regmap_update_bits(power->regmap, AXP20X_PWR_OP_MODE,
			AXP20X_PWR_OP_CHARGING, 0);
	dev_info(&power->supply->dev, "IRQ#%d Battery disconnected\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_activation(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	power->health = POWER_SUPPLY_HEALTH_UNKNOWN;
	dev_info(&power->supply->dev, "IRQ#%d Battery activation started\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_activated(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	power->health = POWER_SUPPLY_HEALTH_GOOD;
	dev_info(&power->supply->dev, "IRQ#%d Battery activation completed\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_charging(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	dev_dbg(&power->supply->dev, "IRQ#%d Battery charging\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_charged(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	dev_dbg(&power->supply->dev, "IRQ#%d Battery charged\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_high_temp(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	power->health = POWER_SUPPLY_HEALTH_OVERHEAT;
	regmap_update_bits(power->regmap, AXP20X_PWR_OP_MODE,
			AXP20X_PWR_OP_CHARGING, 0);
	dev_warn(&power->supply->dev, "IRQ#%d Battery temperature high!\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_low_temp(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	power->health = POWER_SUPPLY_HEALTH_COLD;
	dev_warn(&power->supply->dev, "IRQ#%d Battery temperature low!\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_batt_chg_curr_low(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	dev_info(&power->supply->dev, "IRQ#%d External power too weak for target charging current!\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_power_low(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	dev_warn(&power->supply->dev, "IRQ#%d System power running out soon!\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static irqreturn_t axp20x_irq_power_low_crit(int irq, void *devid)
{
	struct axp20x_battery_power *power = devid;

	dev_crit(&power->supply->dev, "IRQ#%d System power running out now!\n", irq);
	power_supply_changed(power->supply);

	return IRQ_HANDLED;
}

static int axp20x_power_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct axp20x_battery_power *power = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&power->monitor);
	return 0;
}

static int axp20x_power_resume(struct platform_device *pdev)
{
	struct axp20x_battery_power *power = platform_get_drvdata(pdev);

	axp20x_battery_poll(power);
	schedule_delayed_work(&power->monitor, MONITOR_DELAY_JIFFIES);
	return 0;
}

static void axp20x_power_shutdown(struct platform_device *pdev)
{
	struct axp20x_battery_power *power = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&power->monitor);
}

static int axp20x_battery_config(struct platform_device *pdev,
	struct device_node *np, struct axp20x_battery_power *power)
{
	int i, ret = 0, reg, new_reg = 0;
	u32 ocv[OCV_CURVE_SIZE], temp[3], rdc, capa;

	ret = regmap_read(power->regmap, AXP20X_PWR_OP_MODE, &reg);
	if (ret)
		return ret;

	ret = of_property_read_u32_array(np, "ocv", ocv, OCV_CURVE_SIZE);
	for (i = 0; ret == 0 && i < OCV_CURVE_SIZE; i++)
		if (ocv[i] > 100) {
			dev_warn(&pdev->dev, "OCV[%d] %u > 100\n", i, ocv[i]);
			return -EINVAL;
		}

	ret = of_property_read_u32_array(np, "resistance", &rdc, 1);
	if (ret != 0)
		rdc = 100;

	ret = of_property_read_u32_array(np, "capacity", &capa, 1);
	if (ret != 0)
		capa = 0;

	ret = of_property_read_u32_array(np, "temp_sensor", temp, 3);
	if (ret != 0)
		memset(temp, 0, sizeof(temp));
	else if (temp[0] != 20 && temp[0] != 40 && temp[0] != 60 &&
		 temp[0] != 80) {
		dev_warn(&pdev->dev, "Invalid battery temperature sensor current setting\n");
		return -EINVAL;
	}

	dev_dbg(&pdev->dev, "FDT settings: capacity=%d, resistance=%d, temp_sensor=<%d %d %d>\n", capa, rdc, temp[0], temp[1], temp[2]);
	/* apply settings */
	power->health = POWER_SUPPLY_HEALTH_UNKNOWN;
	regmap_update_bits(power->regmap, AXP20X_FG_RES, AXP20X_FG_ENABLE, 0x00);
	regmap_update_bits(power->regmap, AXP20X_RDC_H, 0x80, 0x00);
	regmap_update_bits(power->regmap, AXP20X_RDC_L, 0xff, (rdc * 10000 + 5371) / 10742);
	regmap_update_bits(power->regmap, AXP20X_RDC_H, 0x1f, ((rdc * 10000 + 5371) / 10742) >> 8);
	if (of_find_property(np, "ocv", NULL))
		for (i = 0; i < OCV_CURVE_SIZE; i++) {
			ret = regmap_update_bits(power->regmap, AXP20X_OCV(i),
						 0xff, ocv[i]);
			if (ret < 0)
				dev_warn(&pdev->dev,
					 "Failed to store OCV[%d] setting: %d\n",
					 i, ret);
		}
	regmap_update_bits(power->regmap, AXP20X_FG_RES, AXP20X_FG_ENABLE, AXP20X_FG_ENABLE);

	if (capa == 0 && !(reg & AXP20X_PWR_OP_BATT_PRESENT)) {
		/* No battery present or configured -> disable */
		regmap_update_bits(power->regmap, AXP20X_CHRG_CTRL1, AXP20X_CHRG_CTRL1_ENABLE, 0x00);
		regmap_update_bits(power->regmap, AXP20X_OFF_CTRL, AXP20X_OFF_CTRL_BATT_MON, 0x00);
		dev_info(&pdev->dev, "No battery, disabling charger\n");
		return -ENODEV;
	}

	if (temp[0] == 0) {
		regmap_update_bits(power->regmap, AXP20X_ADC_RATE,
				   AXP20X_ADR_TS_WHEN_MASK |
				   AXP20X_ADR_TS_UNRELATED,
				   AXP20X_ADR_TS_UNRELATED |
				   AXP20X_ADR_TS_WHEN_OFF);
		power->tbatt_min = 0;
		power->tbatt_max = 0;
	} else {
		power->tbatt_min = temp[1];
		power->tbatt_max = temp[2];
		switch (temp[0]) {
		case 20:
			regmap_update_bits(power->regmap, AXP20X_ADC_RATE,
					   AXP20X_ADR_TS_CURR_MASK |
					   AXP20X_ADR_TS_WHEN_MASK |
					   AXP20X_ADR_TS_UNRELATED,
					   AXP20X_ADR_TS_CURR_20uA |
					   AXP20X_ADR_TS_WHEN_ADC);
			break;
		case 40:
			regmap_update_bits(power->regmap, AXP20X_ADC_RATE,
					   AXP20X_ADR_TS_CURR_MASK |
					   AXP20X_ADR_TS_WHEN_MASK |
					   AXP20X_ADR_TS_UNRELATED,
					   AXP20X_ADR_TS_CURR_40uA |
					   AXP20X_ADR_TS_WHEN_ADC);
			break;
		case 60:
			regmap_update_bits(power->regmap, AXP20X_ADC_RATE,
					   AXP20X_ADR_TS_CURR_MASK |
					   AXP20X_ADR_TS_WHEN_MASK |
					   AXP20X_ADR_TS_UNRELATED,
					   AXP20X_ADR_TS_CURR_60uA |
					   AXP20X_ADR_TS_WHEN_ADC);
			break;
		case 80:
			regmap_update_bits(power->regmap, AXP20X_ADC_RATE,
					   AXP20X_ADR_TS_CURR_MASK |
					   AXP20X_ADR_TS_WHEN_MASK |
					   AXP20X_ADR_TS_UNRELATED,
					   AXP20X_ADR_TS_CURR_80uA |
					   AXP20X_ADR_TS_WHEN_ADC);
			break;
		}
		new_reg = temp[1] / (0x10 * 800);
		regmap_update_bits(power->regmap, AXP20X_V_HTF_CHRG, 0xff,
				   new_reg);
		regmap_update_bits(power->regmap, AXP20X_V_HTF_DISCHRG, 0xff,
				   new_reg);
		new_reg = temp[2] / (0x10 * 800);
		regmap_update_bits(power->regmap, AXP20X_V_LTF_CHRG, 0xff,
				   new_reg);
		regmap_update_bits(power->regmap, AXP20X_V_LTF_DISCHRG, 0xff,
				   new_reg);
	}
	/* Enable battery voltage and current measurement */
	ret = regmap_update_bits(power->regmap, AXP20X_ADC_EN1,
			AXP20X_ADC_EN1_BATT_CURR | AXP20X_ADC_EN1_BATT_VOLT |
			(temp[0] ? AXP20X_ADC_EN1_TEMP : 0),
			AXP20X_ADC_EN1_BATT_CURR | AXP20X_ADC_EN1_BATT_VOLT |
			AXP20X_ADC_EN1_TEMP);
	if (ret)
		return ret;

	power->capacity  = capa * 1000;
	power->charge_user_imax = (capa < 300 ? 300 : capa) * 1000;
	/* Prefer longer battery life over longer runtime. */
	regmap_update_bits(power->regmap, AXP20X_CHRG_CTRL1,
			   AXP20X_CHRG_CTRL1_TGT_VOLT,
			   AXP20X_CHRG_CTRL1_TGT_4_15V);

	/* TODO: configure CHGLED? */

	/* Default to about 5% capacity, about 3.5V */
	regmap_update_bits(power->regmap, AXP20X_APS_WARN_L1, 0xff,
			   (3500000 - 2867200) / 4 / 1400);
	regmap_update_bits(power->regmap, AXP20X_APS_WARN_L2, 0xff,
			   (3304000 - 2867200) / 4 / 1400);
	/* RDC - disable capacity monitor, reconfigure, re-enable */
	regmap_update_bits(power->regmap, AXP20X_FG_RES, 0x80, 0x80);
	regmap_update_bits(power->regmap, AXP20X_RDC_H, 0x80, 0x00);
	regmap_update_bits(power->regmap, AXP20X_RDC_H, 0x1f, ((rdc * 10000 + 5371) / 10742) >> 8);
	regmap_update_bits(power->regmap, AXP20X_RDC_L, 0xff, (rdc * 10000 + 5371) / 10742);
	regmap_update_bits(power->regmap, AXP20X_FG_RES, 0x80, 0x00);
	regmap_update_bits(power->regmap, AXP20X_OFF_CTRL, AXP20X_OFF_CTRL_BATT_MON, AXP20X_OFF_CTRL_BATT_MON);
	return 0;
}



static int axp20x_power_probe(struct platform_device *pdev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct axp20x_battery_power *power;
	static const char * const irq_names[] = { "BATT_HOT", "BATT_COLD",
		"BATT_PLUGIN", "BATT_REMOVAL", "BATT_ACTIVATE",
		"BATT_ACTIVATED", "BATT_CHARGING", "BATT_CHARGED",
		"BATT_CHG_CURR_LOW", "BATT_POWER_LOW_WARN",
		"BATT_POWER_LOW_CRIT" };
	irqreturn_t (*irq_funcs[])(int, void *) = { axp20x_irq_batt_high_temp,
		axp20x_irq_batt_low_temp, axp20x_irq_batt_plugin,
		axp20x_irq_batt_removal, axp20x_irq_batt_activation,
		axp20x_irq_batt_activated, axp20x_irq_batt_charging,
		axp20x_irq_batt_charged, axp20x_irq_batt_chg_curr_low,
		axp20x_irq_power_low, axp20x_irq_power_low_crit };
	int i, irq, r;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	power = devm_kzalloc(&pdev->dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;

	INIT_DELAYED_WORK(&power->monitor, axp20x_battery_monitor);
	power->health = POWER_SUPPLY_HEALTH_UNKNOWN;
	power->regmap = axp20x->regmap;
	r = axp20x_battery_config(pdev, pdev->dev.of_node, power);
	if (r)
		return r;

	axp20x_battery_poll(power);
	psy_cfg.of_node = pdev->dev.of_node;
	psy_cfg.drv_data = power;

	// Use axp20x_battery_ts_power_desc if battery temprature sensor
	// is present (of configured)
	power->supply = devm_power_supply_register(&pdev->dev,
					&axp20x_battery_power_desc, &psy_cfg);
	if (IS_ERR(power->supply))
		return PTR_ERR(power->supply);
	axp20x_battery_chg_reconfig(power->supply);

	/* Request irqs after registering, as irqs may trigger immediately */
	// Start at i=2 if battery temprature sensor is not present
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

	schedule_delayed_work(&power->monitor, MONITOR_DELAY_JIFFIES);



//	spin_lock_init(&devdata->lock);
//	devdata->axp20x = axp20x;
//	platform_set_drvdata(pdev, devdata);
//
//	devdata->battery_supplies[0] = "axp20x-usb";
//	devdata->battery_supplies[1] = "axp20x-ac";
//	battery = &devdata->battery;
//	snprintf(devdata->battery_name, sizeof(devdata->battery_name), "axp20x-battery");
//	battery->name                   = devdata->battery_name;
//	battery->type                   = POWER_SUPPLY_TYPE_BATTERY;
//	battery->properties             = axp20x_battery_props;
//	battery->num_properties         = ARRAY_SIZE(axp20x_battery_props);
//	battery->property_is_writeable  = axp20x_battery_prop_writeable;
//	battery->get_property           = axp20x_battery_get_prop;
//	battery->set_property           = axp20x_battery_set_prop;
//	battery->supplied_from          = devdata->battery_supplies;
//	battery->num_supplies           = 1;
//	battery->external_power_changed = axp20x_battery_chg_reconfig;
//
//	/* configure hardware and check FDT params */
//	regmap_update_bits(axp20x->regmap, AXP20X_ADC_RATE,
//			   AXP20X_ADR_RATE_MASK, AXP20X_ADR_RATE_50Hz);
//
//	ret = axp20x_battery_config(pdev, devdata, axp20x);
//	if (ret)
//		return ret;
//	else if (devdata->tbatt_min == 0 && devdata->tbatt_max == 0)
//		battery->num_properties -= 3;
//
//	ret = axp20x_battery_poll(devdata, 2);
//	if (ret)
//		return ret;
//
//	if (!(devdata->status1 & AXP20X_PWR_STATUS_AC_VBUS_SHORT))
//		battery->num_supplies = 2;
//
//	/* register present supplies */
//	ret = power_supply_register(&pdev->dev, battery);
//	if (ret)
//		goto err_unreg_ac;
//	power_supply_changed(&devdata->battery);
//
//	INIT_WORK(&devdata->work, axp20x_power_monitor);
//
//	/* configure interrupts */
//	axp20x_init_irq(pdev, axp20x, "BATT_PLUGIN", battery->name, axp20x_irq_batt_plugin);
//	axp20x_init_irq(pdev, axp20x, "BATT_REMOVAL", battery->name, axp20x_irq_batt_removal);
//	axp20x_init_irq(pdev, axp20x, "BATT_ACTIVATE", battery->name, axp20x_irq_batt_activation);
//	axp20x_init_irq(pdev, axp20x, "BATT_ACTIVATED", battery->name, axp20x_irq_batt_activated);
//	axp20x_init_irq(pdev, axp20x, "BATT_CHARGING", battery->name, axp20x_irq_batt_charging);
//	axp20x_init_irq(pdev, axp20x, "BATT_CHARGED", battery->name, axp20x_irq_batt_charged);
//	if (devdata->tbatt_min != 0 || devdata->tbatt_max != 0) {
//		axp20x_init_irq(pdev, axp20x, "BATT_HOT", battery->name, axp20x_irq_batt_high_temp);
//		axp20x_init_irq(pdev, axp20x, "BATT_COLD", battery->name, axp20x_irq_batt_low_temp);
//	}
//	axp20x_init_irq(pdev, axp20x, "BATT_CHG_CURR_LOW", battery->name, axp20x_irq_batt_chg_curr_low);
//
//	axp20x_init_irq(pdev, axp20x, "POWER_LOW_WARN", battery->name, axp20x_irq_power_low);
//	axp20x_init_irq(pdev, axp20x, "POWER_LOW_CRIT", battery->name, axp20x_irq_power_low_crit);
//
	return 0;
}

static int axp20x_power_remove(struct platform_device *pdev)
{
	struct axp20x_battery_power *power = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&power->monitor);
	return 0;
}

static const struct of_device_id axp20x_battery_power_match[] = {
	{ .compatible = "x-powers,axp202-battery-power-supply" },
	{ }
};
MODULE_DEVICE_TABLE(of, axp20x_battery_power_match);

static struct platform_driver axp20x_battery_power_driver = {
	.probe    = axp20x_power_probe,
//	.remove   = axp20x_power_remove,
//	.suspend  = axp20x_power_suspend,
//	.resume   = axp20x_power_resume,
//	.shutdown = axp20x_power_shutdown,
	.driver   = {
		.name  = DRVNAME,
		.of_match_table = axp20x_battery_power_match,
	},
};

module_platform_driver(axp20x_battery_power_driver);

MODULE_AUTHOR("Bruno Prémont <bonbons@linux-vserver.org>");
MODULE_DESCRIPTION("AXP20x PMIC battery charger driver");
MODULE_LICENSE("GPL");
