/*
 * bq2415x charger driver
 *
 * Copyright (C) 2011-2012  Pali Rohár <pali.rohar@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Datasheets:
 * http://www.ti.com/product/bq24150
 * http://www.ti.com/product/bq24150a
 * http://www.ti.com/product/bq24152
 * http://www.ti.com/product/bq24153
 * http://www.ti.com/product/bq24153a
 * http://www.ti.com/product/bq24155
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/param.h>
#include <linux/err.h>
#include <linux/workqueue.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>

#include <linux/power/bq2415x_charger.h>

/* timeout for resetting chip timer */
#define BQ2415X_TIMER_TIMEOUT		10

#define BQ2415X_REG_STATUS		0x00
#define BQ2415X_REG_CONTROL		0x01
#define BQ2415X_REG_VOLTAGE		0x02
#define BQ2415X_REG_VENDER		0x03
#define BQ2415X_REG_CURRENT		0x04

/* reset state for all registers */
#define BQ2415X_RESET_STATUS		BIT(6)
#define BQ2415X_RESET_CONTROL		(BIT(4)|BIT(5))
#define BQ2415X_RESET_VOLTAGE		(BIT(1)|BIT(3))
#define BQ2415X_RESET_CURRENT		(BIT(0)|BIT(3)|BIT(7))

/* status register */
#define BQ2415X_BIT_TMR_RST		7
#define BQ2415X_BIT_OTG			7
#define BQ2415X_BIT_EN_STAT		6
#define BQ2415X_MASK_STAT		(BIT(4)|BIT(5))
#define BQ2415X_SHIFT_STAT		4
#define BQ2415X_BIT_BOOST		3
#define BQ2415X_MASK_FAULT		(BIT(0)|BIT(1)|BIT(2))
#define BQ2415X_SHIFT_FAULT		0

/* control register */
#define BQ2415X_MASK_LIMIT		(BIT(6)|BIT(7))
#define BQ2415X_SHIFT_LIMIT		6
#define BQ2415X_MASK_VLOWV		(BIT(4)|BIT(5))
#define BQ2415X_SHIFT_VLOWV		4
#define BQ2415X_BIT_TE			3
#define BQ2415X_BIT_CE			2
#define BQ2415X_BIT_HZ_MODE		1
#define BQ2415X_BIT_OPA_MODE		0

/* voltage register */
#define BQ2415X_MASK_VO		(BIT(2)|BIT(3)|BIT(4)|BIT(5)|BIT(6)|BIT(7))
#define BQ2415X_SHIFT_VO		2
#define BQ2415X_BIT_OTG_PL		1
#define BQ2415X_BIT_OTG_EN		0

/* vender register */
#define BQ2415X_MASK_VENDER		(BIT(5)|BIT(6)|BIT(7))
#define BQ2415X_SHIFT_VENDER		5
#define BQ2415X_MASK_PN			(BIT(3)|BIT(4))
#define BQ2415X_SHIFT_PN		3
#define BQ2415X_MASK_REVISION		(BIT(0)|BIT(1)|BIT(2))
#define BQ2415X_SHIFT_REVISION		0

/* current register */
#define BQ2415X_MASK_RESET		BIT(7)
#define BQ2415X_MASK_VI_CHRG		(BIT(4)|BIT(5)|BIT(6))
#define BQ2415X_SHIFT_VI_CHRG		4
/* N/A					BIT(3) */
#define BQ2415X_MASK_VI_TERM		(BIT(0)|BIT(1)|BIT(2))
#define BQ2415X_SHIFT_VI_TERM		0


enum bq2415x_command {
	BQ2415X_TIMER_RESET,
	BQ2415X_OTG_STATUS,
	BQ2415X_STAT_PIN_STATUS,
	BQ2415X_STAT_PIN_ENABLE,
	BQ2415X_STAT_PIN_DISABLE,
	BQ2415X_CHARGE_STATUS,
	BQ2415X_BOOST_STATUS,
	BQ2415X_FAULT_STATUS,

	BQ2415X_CHARGE_TERMINATION_STATUS,
	BQ2415X_CHARGE_TERMINATION_ENABLE,
	BQ2415X_CHARGE_TERMINATION_DISABLE,
	BQ2415X_CHARGER_STATUS,
	BQ2415X_CHARGER_ENABLE,
	BQ2415X_CHARGER_DISABLE,
	BQ2415X_HIGH_IMPEDANCE_STATUS,
	BQ2415X_HIGH_IMPEDANCE_ENABLE,
	BQ2415X_HIGH_IMPEDANCE_DISABLE,
	BQ2415X_BOOST_MODE_STATUS,
	BQ2415X_BOOST_MODE_ENABLE,
	BQ2415X_BOOST_MODE_DISABLE,

	BQ2415X_OTG_LEVEL,
	BQ2415X_OTG_ACTIVATE_HIGH,
	BQ2415X_OTG_ACTIVATE_LOW,
	BQ2415X_OTG_PIN_STATUS,
	BQ2415X_OTG_PIN_ENABLE,
	BQ2415X_OTG_PIN_DISABLE,

	BQ2415X_VENDER_CODE,
	BQ2415X_PART_NUMBER,
	BQ2415X_REVISION,
};

enum bq2415x_chip {
	BQUNKNOWN,
	BQ24150,
	BQ24150A,
	BQ24151,
	BQ24151A,
	BQ24152,
	BQ24153,
	BQ24153A,
	BQ24155,
	BQ24156,
	BQ24156A,
	BQ24158,
};

static char *bq2415x_chip_name[] = {
	"unknown",
	"bq24150",
	"bq24150a",
	"bq24151",
	"bq24151a",
	"bq24152",
	"bq24153",
	"bq24153a",
	"bq24155",
	"bq24156",
	"bq24156a",
	"bq24158",
};

struct bq2415x_device {
	struct device *dev;
	struct bq2415x_platform_data init_data;
	struct bq2415x_callbacks callbacks;
	struct power_supply charger;
	struct delayed_work work;
	enum bq2415x_chip chip;
	char *model;
	char *name;
	int autotimer;	/* 1 - if driver automatically reset timer, 0 - not */
	int id;
	enum bq2415x_status last_status;
	enum bq2415x_mode mode;
	struct wake_lock wakelock;
};

/* each registered chip must have unique id */
static DEFINE_IDR(bq2415x_id);

static DEFINE_MUTEX(bq2415x_id_mutex);
static DEFINE_MUTEX(bq2415x_timer_mutex);
static DEFINE_MUTEX(bq2415x_i2c_mutex);

/**** i2c read functions ****/

/* read value from register */
static int bq2415x_i2c_read(struct bq2415x_device *bq, u8 reg)
{
	struct i2c_client *client = to_i2c_client(bq->dev);
	struct i2c_msg msg[2];
	u8 val;
	int ret;
	int tries;

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(reg);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = &val;
	msg[1].len = sizeof(val);

	for (tries = 0; tries < 5; tries++) {
		mutex_lock(&bq2415x_i2c_mutex);
		ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		mutex_unlock(&bq2415x_i2c_mutex);

		if (ret != ARRAY_SIZE(msg))
			msleep(10);
		else
			break;
	}


	if (ret < 0)
		return ret;

	return val;
}

/* read value from register, apply mask and right shift it */
static int bq2415x_i2c_read_mask(struct bq2415x_device *bq, u8 reg,
				 u8 mask, u8 shift)
{
	int ret;

	if (shift > 8)
		return -EINVAL;

	ret = bq2415x_i2c_read(bq, reg);
	if (ret < 0)
		return ret;
	return (ret & mask) >> shift;
}

/* read value from register and return one specified bit */
static int bq2415x_i2c_read_bit(struct bq2415x_device *bq, u8 reg, u8 bit)
{
	if (bit > 8)
		return -EINVAL;
	return bq2415x_i2c_read_mask(bq, reg, BIT(bit), bit);
}

/**** i2c write functions ****/

/* write value to register */
static int bq2415x_i2c_write(struct bq2415x_device *bq, u8 reg, u8 val)
{
	struct i2c_client *client = to_i2c_client(bq->dev);
	struct i2c_msg msg[1];
	u8 data[2];
	int ret;
	int tries;

	data[0] = reg;
	data[1] = val;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = data;
	msg[0].len = ARRAY_SIZE(data);

	for (tries = 0; tries < 5; tries++) {
		mutex_lock(&bq2415x_i2c_mutex);
		ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		mutex_unlock(&bq2415x_i2c_mutex);

		if (ret != ARRAY_SIZE(msg))
			msleep(10);
		else
			break;
	}

	/* i2c_transfer returns number of messages transferred */
	if (ret < 0)
		return ret;
	else if (ret != 1)
		return -EIO;

	return 0;
}

/* read value from register, change it with mask left shifted and write back */
static int bq2415x_i2c_write_mask(struct bq2415x_device *bq, u8 reg, u8 val,
				  u8 mask, u8 shift)
{
	int ret;

	if (shift > 8)
		return -EINVAL;

	ret = bq2415x_i2c_read(bq, reg);
	if (ret < 0)
		return ret;

	ret &= ~mask;
	ret |= val << shift;

	return bq2415x_i2c_write(bq, reg, ret);
}

/* change only one bit in register */
static int bq2415x_i2c_write_bit(struct bq2415x_device *bq, u8 reg,
				 bool val, u8 bit)
{
	if (bit > 8)
		return -EINVAL;
	return bq2415x_i2c_write_mask(bq, reg, val, BIT(bit), bit);
}

/**** global functions ****/

/* exec command function */
static int bq2415x_exec_command(struct bq2415x_device *bq,
				enum bq2415x_command command)
{
	int ret;

	switch (command) {
	case BQ2415X_TIMER_RESET:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_STATUS,
				1, BQ2415X_BIT_TMR_RST);
	case BQ2415X_OTG_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_STATUS,
				BQ2415X_BIT_OTG);
	case BQ2415X_STAT_PIN_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_STATUS,
				BQ2415X_BIT_EN_STAT);
	case BQ2415X_STAT_PIN_ENABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_STATUS, 1,
				BQ2415X_BIT_EN_STAT);
	case BQ2415X_STAT_PIN_DISABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_STATUS, 0,
				BQ2415X_BIT_EN_STAT);
	case BQ2415X_CHARGE_STATUS:
		return bq2415x_i2c_read_mask(bq, BQ2415X_REG_STATUS,
				BQ2415X_MASK_STAT, BQ2415X_SHIFT_STAT);
	case BQ2415X_BOOST_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_STATUS,
				BQ2415X_BIT_BOOST);
	case BQ2415X_FAULT_STATUS:
		return bq2415x_i2c_read_mask(bq, BQ2415X_REG_STATUS,
			BQ2415X_MASK_FAULT, BQ2415X_SHIFT_FAULT);

	case BQ2415X_CHARGE_TERMINATION_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_CONTROL,
				BQ2415X_BIT_TE);
	case BQ2415X_CHARGE_TERMINATION_ENABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				1, BQ2415X_BIT_TE);
	case BQ2415X_CHARGE_TERMINATION_DISABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				0, BQ2415X_BIT_TE);
	case BQ2415X_CHARGER_STATUS:
		ret = bq2415x_i2c_read_bit(bq, BQ2415X_REG_CONTROL,
			BQ2415X_BIT_CE);
		if (ret < 0)
			return ret;
		else
			return ret > 0 ? 0 : 1;
	case BQ2415X_CHARGER_ENABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				0, BQ2415X_BIT_CE);
	case BQ2415X_CHARGER_DISABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				1, BQ2415X_BIT_CE);
	case BQ2415X_HIGH_IMPEDANCE_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_CONTROL,
				BQ2415X_BIT_HZ_MODE);
	case BQ2415X_HIGH_IMPEDANCE_ENABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				1, BQ2415X_BIT_HZ_MODE);
	case BQ2415X_HIGH_IMPEDANCE_DISABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				0, BQ2415X_BIT_HZ_MODE);
	case BQ2415X_BOOST_MODE_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_CONTROL,
				BQ2415X_BIT_OPA_MODE);
	case BQ2415X_BOOST_MODE_ENABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				1, BQ2415X_BIT_OPA_MODE);
	case BQ2415X_BOOST_MODE_DISABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_CONTROL,
				0, BQ2415X_BIT_OPA_MODE);

	case BQ2415X_OTG_LEVEL:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_VOLTAGE,
				BQ2415X_BIT_OTG_PL);
	case BQ2415X_OTG_ACTIVATE_HIGH:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_VOLTAGE,
				1, BQ2415X_BIT_OTG_PL);
	case BQ2415X_OTG_ACTIVATE_LOW:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_VOLTAGE,
				0, BQ2415X_BIT_OTG_PL);
	case BQ2415X_OTG_PIN_STATUS:
		return bq2415x_i2c_read_bit(bq, BQ2415X_REG_VOLTAGE,
				BQ2415X_BIT_OTG_EN);
	case BQ2415X_OTG_PIN_ENABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_VOLTAGE,
				1, BQ2415X_BIT_OTG_EN);
	case BQ2415X_OTG_PIN_DISABLE:
		return bq2415x_i2c_write_bit(bq, BQ2415X_REG_VOLTAGE,
				0, BQ2415X_BIT_OTG_EN);

	case BQ2415X_VENDER_CODE:
		return bq2415x_i2c_read_mask(bq, BQ2415X_REG_VENDER,
			BQ2415X_MASK_VENDER, BQ2415X_SHIFT_VENDER);
	case BQ2415X_PART_NUMBER:
		return bq2415x_i2c_read_mask(bq, BQ2415X_REG_VENDER,
				BQ2415X_MASK_PN, BQ2415X_SHIFT_PN);
	case BQ2415X_REVISION:
		return bq2415x_i2c_read_mask(bq, BQ2415X_REG_VENDER,
			BQ2415X_MASK_REVISION, BQ2415X_SHIFT_REVISION);
	}
	return -EINVAL;
}

/* detect chip type */
static enum bq2415x_chip bq2415x_detect_chip(struct bq2415x_device *bq)
{
	struct i2c_client *client = to_i2c_client(bq->dev);
	int ret = bq2415x_exec_command(bq, BQ2415X_PART_NUMBER);

	if (ret < 0)
		return ret;

	switch (client->addr) {
	case 0x6b:
		switch (ret) {
		case 0:
			if (bq->chip == BQ24151A)
				return bq->chip;
			else
				return BQ24151;
		case 1:
			if (bq->chip == BQ24150A ||
				bq->chip == BQ24152 ||
				bq->chip == BQ24155)
				return bq->chip;
			else
				return BQ24150;
		case 2:
			if (bq->chip == BQ24153A)
				return bq->chip;
			else
				return BQ24153;
		default:
			return BQUNKNOWN;
		}
		break;

	case 0x6a:
		switch (ret) {
		case 0:
			if (bq->chip == BQ24156A)
				return bq->chip;
			else
				return BQ24156;
		case 2:
			return BQ24158;
		default:
			return BQUNKNOWN;
		}
		break;
	}

	return BQUNKNOWN;
}

/* detect chip revision */
static int bq2415x_detect_revision(struct bq2415x_device *bq)
{
	int ret = bq2415x_exec_command(bq, BQ2415X_REVISION);
	int chip = bq2415x_detect_chip(bq);

	if (ret < 0 || chip < 0)
		return -1;

	switch (chip) {
	case BQ24150:
	case BQ24150A:
	case BQ24151:
	case BQ24151A:
	case BQ24152:
		if (ret >= 0 && ret <= 3)
			return ret;
		else
			return -1;
	case BQ24153:
	case BQ24153A:
	case BQ24156:
	case BQ24156A:
	case BQ24158:
		if (ret == 3)
			return 0;
		else if (ret == 1)
			return 1;
		else
			return -1;
	case BQ24155:
		if (ret == 3)
			return 3;
		else
			return -1;
	case BQUNKNOWN:
		return -1;
	}

	return -1;
}

/* return chip vender code */
static int bq2415x_get_vender_code(struct bq2415x_device *bq)
{
	int ret;

	ret = bq2415x_exec_command(bq, BQ2415X_VENDER_CODE);
	if (ret < 0)
		return 0;

	/* convert to binary */
	return (ret & 0x1) +
	       ((ret >> 1) & 0x1) * 10 +
	       ((ret >> 2) & 0x1) * 100;
}

/* reset all chip registers to default state */
static void bq2415x_reset_chip(struct bq2415x_device *bq)
{
	bq2415x_i2c_write(bq, BQ2415X_REG_CURRENT, BQ2415X_RESET_CURRENT);
	bq2415x_i2c_write(bq, BQ2415X_REG_VOLTAGE, BQ2415X_RESET_VOLTAGE);
	bq2415x_i2c_write(bq, BQ2415X_REG_CONTROL, BQ2415X_RESET_CONTROL);
	bq2415x_i2c_write(bq, BQ2415X_REG_STATUS, BQ2415X_RESET_STATUS);
}

/**** properties functions ****/

/* set current limit in mA */
static int bq2415x_set_current_limit(struct bq2415x_device *bq, int mA)
{
	int val;

	if (mA <= 100)
		val = 0;
	else if (mA <= 500)
		val = 1;
	else if (mA <= 800)
		val = 2;
	else
		val = 3;

	bq->init_data.current_limit = mA;

	return bq2415x_i2c_write_mask(bq, BQ2415X_REG_CONTROL, val,
			BQ2415X_MASK_LIMIT, BQ2415X_SHIFT_LIMIT);
}

/* get current limit in mA */
static int bq2415x_get_current_limit(struct bq2415x_device *bq)
{
	int ret;

	ret = bq2415x_i2c_read_mask(bq, BQ2415X_REG_CONTROL,
			BQ2415X_MASK_LIMIT, BQ2415X_SHIFT_LIMIT);
	if (ret < 0)
		return ret;
	else if (ret == 0)
		return 100;
	else if (ret == 1)
		return 500;
	else if (ret == 2)
		return 800;
	else if (ret == 3)
		return 1800;
	return -EINVAL;
}

/* set weak battery voltage in mV */
static int bq2415x_set_weak_battery_voltage(struct bq2415x_device *bq, int mV)
{
	int val;

	/* round to 100mV */
	if (mV <= 3400 + 50)
		val = 0;
	else if (mV <= 3500 + 50)
		val = 1;
	else if (mV <= 3600 + 50)
		val = 2;
	else
		val = 3;

	bq->init_data.weak_battery_voltage = mV;

	return bq2415x_i2c_write_mask(bq, BQ2415X_REG_CONTROL, val,
			BQ2415X_MASK_VLOWV, BQ2415X_SHIFT_VLOWV);
}

/* get weak battery voltage in mV */
static int bq2415x_get_weak_battery_voltage(struct bq2415x_device *bq)
{
	int ret;

	ret = bq2415x_i2c_read_mask(bq, BQ2415X_REG_CONTROL,
			BQ2415X_MASK_VLOWV, BQ2415X_SHIFT_VLOWV);
	if (ret < 0)
		return ret;
	return 100 * (34 + ret);
}

/* set battery regulation voltage in mV */
static int bq2415x_set_battery_regulation_voltage(struct bq2415x_device *bq,
						  int mV)
{
	int val = (mV/10 - 350) / 2;

	/*
	 * According to datasheet, maximum battery regulation voltage is
	 * 4440mV which is b101111 = 47.
	 */
	if (val < 0)
		val = 0;
	else if (val > 47)
		return -EINVAL;

	bq->init_data.battery_regulation_voltage = mV;

	return bq2415x_i2c_write_mask(bq, BQ2415X_REG_VOLTAGE, val,
			BQ2415X_MASK_VO, BQ2415X_SHIFT_VO);
}

/* get battery regulation voltage in mV */
static int bq2415x_get_battery_regulation_voltage(struct bq2415x_device *bq)
{
	int ret = bq2415x_i2c_read_mask(bq, BQ2415X_REG_VOLTAGE,
			BQ2415X_MASK_VO, BQ2415X_SHIFT_VO);

	if (ret < 0)
		return ret;
	return 10 * (350 + 2*ret);
}

/* set charge current in mA (platform data must provide resistor sense) */
static int bq2415x_set_charge_current(struct bq2415x_device *bq, int mA)
{
	int val;

	if (bq->init_data.resistor_sense <= 0)
		return -ENOSYS;

	val = (mA * bq->init_data.resistor_sense - 37400) / 6800;
	if (val < 0)
		val = 0;
	else if (val > 7)
		val = 7;

	bq->init_data.charge_current = mA;

	return bq2415x_i2c_write_mask(bq, BQ2415X_REG_CURRENT, val,
			BQ2415X_MASK_VI_CHRG | BQ2415X_MASK_RESET,
			BQ2415X_SHIFT_VI_CHRG);
}

/* get charge current in mA (platform data must provide resistor sense) */
static int bq2415x_get_charge_current(struct bq2415x_device *bq)
{
	int ret;

	if (bq->init_data.resistor_sense <= 0)
		return -ENOSYS;

	ret = bq2415x_i2c_read_mask(bq, BQ2415X_REG_CURRENT,
			BQ2415X_MASK_VI_CHRG, BQ2415X_SHIFT_VI_CHRG);
	if (ret < 0)
		return ret;
	return (37400 + 6800*ret) / bq->init_data.resistor_sense;
}

/* set termination current in mA (platform data must provide resistor sense) */
static int bq2415x_set_termination_current(struct bq2415x_device *bq, int mA)
{
	int val;

	if (bq->init_data.resistor_sense <= 0)
		return -ENOSYS;

	val = (mA * bq->init_data.resistor_sense - 3400) / 3400;
	if (val < 0)
		val = 0;
	else if (val > 7)
		val = 7;

	bq->init_data.termination_current = mA;

	return bq2415x_i2c_write_mask(bq, BQ2415X_REG_CURRENT, val,
			BQ2415X_MASK_VI_TERM | BQ2415X_MASK_RESET,
			BQ2415X_SHIFT_VI_TERM);
}

/* get termination current in mA (platform data must provide resistor sense) */
static int bq2415x_get_termination_current(struct bq2415x_device *bq)
{
	int ret;

	if (bq->init_data.resistor_sense <= 0)
		return -ENOSYS;

	ret = bq2415x_i2c_read_mask(bq, BQ2415X_REG_CURRENT,
			BQ2415X_MASK_VI_TERM, BQ2415X_SHIFT_VI_TERM);
	if (ret < 0)
		return ret;
	return (3400 + 3400*ret) / bq->init_data.resistor_sense;
}

static int bq2415x_set_stat_pin_enable(struct bq2415x_device *bq, bool enabled)
{
	enum bq2415x_command command;

	if (enabled)
		command = BQ2415X_STAT_PIN_ENABLE;
	else
		command = BQ2415X_STAT_PIN_DISABLE;

	bq->init_data.stat_pin_enable = enabled;

	return bq2415x_exec_command(bq, command);
}

static int bq2415x_get_stat_pin_enable(struct bq2415x_device *bq)
{
	return bq2415x_exec_command(bq, BQ2415X_STAT_PIN_STATUS);
}

static int bq2415x_set_otg_pin_enable(struct bq2415x_device *bq, bool enabled)
{
	enum bq2415x_command command;

	if (enabled)
		command = BQ2415X_OTG_PIN_ENABLE;
	else
		command = BQ2415X_OTG_PIN_DISABLE;

	bq->init_data.otg_pin_enable = enabled;

	return bq2415x_exec_command(bq, command);
}

static int bq2415x_get_otg_pin_enable(struct bq2415x_device *bq)
{
	return bq2415x_exec_command(bq, BQ2415X_OTG_PIN_STATUS);
}

/* set default value of property */
#define bq2415x_set_default_value(bq, prop) \
	do { \
		if (bq->init_data.prop != -1) \
			bq2415x_set_##prop(bq, bq->init_data.prop); \
	} while (0)

/* set default values of all properties */
static int bq2415x_set_defaults(struct bq2415x_device *bq)
{
	bq2415x_reset_chip(bq);

	bq2415x_exec_command(bq, BQ2415X_CHARGE_TERMINATION_DISABLE);

	bq2415x_set_default_value(bq, current_limit);
	bq2415x_set_default_value(bq, weak_battery_voltage);
	bq2415x_set_default_value(bq, battery_regulation_voltage);

	bq2415x_set_default_value(bq, stat_pin_enable);
	bq2415x_set_default_value(bq, otg_pin_enable);

	if (bq->init_data.resistor_sense > 0) {
		bq2415x_set_default_value(bq, charge_current);
		bq2415x_set_default_value(bq, termination_current);
		bq2415x_exec_command(bq, BQ2415X_CHARGE_TERMINATION_ENABLE);
	}

	return 0;
}

/**** charger mode functions ****/

static void bq2415x_set_autotimer(struct bq2415x_device *bq, int state);

/* set charger mode */
static int bq2415x_set_mode(struct bq2415x_device *bq, enum bq2415x_mode mode)
{
	int ret = 0;

	bq2415x_set_defaults(bq);

	switch (mode) {
	case BQ2415X_MODE_OFF:
		bq2415x_exec_command(bq, BQ2415X_CHARGER_DISABLE);
		bq2415x_exec_command(bq, BQ2415X_BOOST_MODE_DISABLE);
		ret = bq2415x_exec_command(bq, BQ2415X_HIGH_IMPEDANCE_ENABLE);
		break;
	case BQ2415X_MODE_CHARGE:
		bq2415x_exec_command(bq, BQ2415X_HIGH_IMPEDANCE_DISABLE);
		bq2415x_exec_command(bq, BQ2415X_BOOST_MODE_DISABLE);
		ret = bq2415x_exec_command(bq, BQ2415X_CHARGER_ENABLE);
		break;
	case BQ2415X_MODE_BOOST:
		bq2415x_exec_command(bq, BQ2415X_HIGH_IMPEDANCE_DISABLE);
		bq2415x_exec_command(bq, BQ2415X_CHARGER_DISABLE);
		ret = bq2415x_exec_command(bq, BQ2415X_BOOST_MODE_ENABLE);
		break;
	}

	/* Safe to turn off safety timer when charger is in turned off mode. */
	bq2415x_set_autotimer(bq, (mode != BQ2415X_MODE_OFF));

	bq->mode = mode;

	return ret;

}

static void bq2415x_set_current_limit_cb(struct bq2415x_callbacks *ptr, int mA)
{
	struct bq2415x_device *bq =
		container_of(ptr, struct bq2415x_device, callbacks);

	dev_dbg(bq->dev, "set_current_limit_cb %d\n", mA);

	bq2415x_set_current_limit(bq, mA);
}

static void bq2415x_set_mode_cb(struct bq2415x_callbacks *ptr,
	enum bq2415x_mode mode)
{
	struct bq2415x_device *bq =
		container_of(ptr, struct bq2415x_device, callbacks);

	dev_dbg(bq->dev, "set_mode_cb %d\n", mode);

	bq2415x_set_mode(bq, mode);
}

/**** timer functions ****/

/* enable/disable auto resetting chip timer */
static void bq2415x_set_autotimer(struct bq2415x_device *bq, int state)
{
	mutex_lock(&bq2415x_timer_mutex);

	if (bq->autotimer == state) {
		mutex_unlock(&bq2415x_timer_mutex);
		return;
	}

	bq->autotimer = state;

	if (state) {
		wake_lock(&bq->wakelock);
		schedule_delayed_work(&bq->work, BQ2415X_TIMER_TIMEOUT * HZ);
		bq2415x_exec_command(bq, BQ2415X_TIMER_RESET);
	} else {
		cancel_delayed_work_sync(&bq->work);
		wake_unlock(&bq->wakelock);
	}

	mutex_unlock(&bq2415x_timer_mutex);
}

/* compare values & reset if needed (for safety) */
static void bq2415x_compare_values(struct bq2415x_device *bq)
{
	bool reset = false;
	int value = 0;

	value = bq2415x_get_current_limit(bq);
	if (value != bq->init_data.current_limit) {
		reset = true;
		goto end;
	}
	value = bq2415x_get_weak_battery_voltage(bq);
	if (value != bq->init_data.weak_battery_voltage) {
		reset = true;
		goto end;
	}
	value = bq2415x_get_battery_regulation_voltage(bq);
	if (value != bq->init_data.battery_regulation_voltage) {
		reset = true;
		goto end;
	}

	if (bq->init_data.resistor_sense > 0) {
		value = bq2415x_get_charge_current(bq);
		if (value != bq->init_data.charge_current) {
			reset = true;
			goto end;
		}
	}

	value = bq2415x_get_stat_pin_enable(bq);
	if (value != bq->init_data.stat_pin_enable) {
		reset = true;
		goto end;
	}
	value = bq2415x_get_otg_pin_enable(bq);
	if (value != bq->init_data.otg_pin_enable) {
		reset = true;
		goto end;
	}

end:
	if (reset) {
		dev_err(bq->dev, "Registers corrupted, restoring\n");
		bq2415x_set_mode(bq, bq->mode);
	}
}

/* delayed work function for auto resetting chip timer */
static void bq2415x_timer_work(struct work_struct *work)
{
	struct bq2415x_device *bq = container_of(work, struct bq2415x_device,
						 work.work);
	int ret;
	int error;
	int boost;
	int status;

	if (!bq->autotimer)
		return;

	ret = bq2415x_exec_command(bq, BQ2415X_TIMER_RESET);
	if (ret < 0) {
		dev_err(bq->dev, "Resetting timer failed\n");
		goto reschedule;
	}

	status = bq2415x_exec_command(bq, BQ2415X_CHARGE_STATUS);
	if (status < 0) {
		dev_err(bq->dev, "Unknown error\n");
		goto reschedule;
	}

	boost = bq2415x_exec_command(bq, BQ2415X_BOOST_MODE_STATUS);
	if (boost < 0) {
		dev_err(bq->dev, "Unknown error\n");
		goto reschedule;
	}

	error = bq2415x_exec_command(bq, BQ2415X_FAULT_STATUS);
	if (error < 0) {
		dev_err(bq->dev, "Unknown error\n");
		goto reschedule;
	}

	if (bq->last_status != status) {
		switch (status) {
		case 0: /* Ready */
			bq->last_status = BQ2415X_STATUS_READY;
			break;
		case 1: /* Charge in progress */
			bq->last_status = BQ2415X_STATUS_CHARGING;
			break;
		case 2: /* Charge done */
			bq->last_status = BQ2415X_STATUS_CHARGE_DONE;
			break;
		case 3: /* Fault */
			bq->last_status = BQ2415X_STATUS_FAULT;
			break;
		}

		if (bq->init_data.status_changed)
			bq->init_data.status_changed(bq->last_status);
	}

	if (boost) {
		switch (error) {
		/* Non fatal errors, chip is OK */
		case 0: /* No error */
			break;
		case 6: /* Timer expired */
			dev_err(bq->dev, "Timer expired\n");
			bq2415x_set_mode(bq, bq->mode);
			break;
		case 3: /* Battery voltage too low */
			dev_err(bq->dev, "Battery voltage to low\n");
			break;

		/* Fatal errors, disable and reset chip */
		case 1: /* Overvoltage protection (chip fried) */
			dev_err(bq->dev,
				"Overvoltage protection (chip fried)\n");
			break;
		case 2: /* Overload */
			dev_err(bq->dev, "Overload\n");
			break;
		case 4: /* Battery overvoltage protection */
			dev_err(bq->dev, "Battery overvoltage protection\n");
			break;
		case 5: /* Thermal shutdown (too hot) */
			dev_err(bq->dev, "Thermal shutdown (too hot)\n");
			break;
		case 7: /* N/A */
			dev_err(bq->dev, "Unknown error\n");
			break;
		}
	} else {
		switch (error) {
		/* Non fatal errors, chip is OK */
		case 0: /* No error */
			break;
		case 2: /* Sleep mode */
			dev_err(bq->dev, "Sleep mode\n");
			break;
		case 3: /* Poor input source */
			dev_err(bq->dev, "Poor input source\n");
			break;
		case 6: /* Timer expired */
			dev_err(bq->dev, "Timer expired\n");
			bq2415x_set_mode(bq, bq->mode);
			break;
		case 7: /* No battery */
			dev_err(bq->dev, "No battery\n");
			bq2415x_set_mode(bq, bq->mode);
			break;

		/* Fatal errors, disable and reset chip */
		case 1: /* Overvoltage protection (chip fried) */
			dev_err(bq->dev,
				"Overvoltage protection (chip fried)\n");
			break;
		case 4: /* Battery overvoltage protection */
			dev_err(bq->dev, "Battery overvoltage protection\n");
			break;
		case 5: /* Thermal shutdown (too hot) */
			dev_err(bq->dev, "Thermal shutdown (too hot)\n");
			break;
		}
	}

reschedule:
	bq2415x_compare_values(bq);

	schedule_delayed_work(&bq->work, BQ2415X_TIMER_TIMEOUT * HZ);
}

/**** power supply interface code ****/

static enum power_supply_property bq2415x_power_supply_props[] = {
	/* TODO: maybe add more power supply properties */
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int bq2415x_power_supply_get_property(struct power_supply *psy,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = bq2415x_exec_command(bq, BQ2415X_CHARGE_STATUS);
		if (ret < 0)
			return ret;
		else if (ret == 0) /* Ready */
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (ret == 1) /* Charge in progress */
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (ret == 2) /* Charge done */
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = bq->model;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int bq2415x_power_supply_init(struct bq2415x_device *bq)
{
	int ret;
	int chip;
	char revstr[8];

	bq->charger.name = bq->name;
	bq->charger.type = POWER_SUPPLY_TYPE_USB;
	bq->charger.properties = bq2415x_power_supply_props;
	bq->charger.num_properties = ARRAY_SIZE(bq2415x_power_supply_props);
	bq->charger.get_property = bq2415x_power_supply_get_property;

	ret = bq2415x_detect_chip(bq);
	if (ret < 0)
		chip = BQUNKNOWN;
	else
		chip = ret;

	ret = bq2415x_detect_revision(bq);
	if (ret < 0)
		strcpy(revstr, "unknown");
	else
		sprintf(revstr, "1.%d", ret);

	bq->model = kasprintf(GFP_KERNEL,
				"chip %s, revision %s, vender code %.3d",
				bq2415x_chip_name[chip], revstr,
				bq2415x_get_vender_code(bq));
	if (!bq->model) {
		dev_err(bq->dev, "failed to allocate model name\n");
		return -ENOMEM;
	}

	ret = power_supply_register(bq->dev, &bq->charger);
	if (ret) {
		kfree(bq->model);
		return ret;
	}

	return 0;
}

static void bq2415x_power_supply_exit(struct bq2415x_device *bq)
{
	bq->autotimer = 0;
	cancel_delayed_work_sync(&bq->work);
	power_supply_unregister(&bq->charger);
	kfree(bq->model);
}

/**** additional sysfs entries for power supply interface ****/

/* show *_status entries */
static ssize_t bq2415x_sysfs_show_status(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						charger);
	enum bq2415x_command command;
	int ret;

	if (strcmp(attr->attr.name, "otg_status") == 0)
		command = BQ2415X_OTG_STATUS;
	else if (strcmp(attr->attr.name, "charge_status") == 0)
		command = BQ2415X_CHARGE_STATUS;
	else if (strcmp(attr->attr.name, "boost_status") == 0)
		command = BQ2415X_BOOST_STATUS;
	else if (strcmp(attr->attr.name, "fault_status") == 0)
		command = BQ2415X_FAULT_STATUS;
	else
		return -EINVAL;

	ret = bq2415x_exec_command(bq, command);
	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

/*
 * set timer entry:
 *    auto - enable auto mode
 *    off - disable auto mode
 *    (other values) - reset chip timer
 */
static ssize_t bq2415x_sysfs_set_timer(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf,
				       size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						charger);
	int ret = 0;

	if (strncmp(buf, "auto", 4) == 0)
		bq2415x_set_autotimer(bq, 1);
	else if (strncmp(buf, "off", 3) == 0)
		bq2415x_set_autotimer(bq, 0);
	else
		ret = bq2415x_exec_command(bq, BQ2415X_TIMER_RESET);

	if (ret < 0)
		return ret;
	return count;
}

/* show timer entry (auto or off) */
static ssize_t bq2415x_sysfs_show_timer(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);

	if (bq->autotimer)
		return sprintf(buf, "auto\n");
	return sprintf(buf, "off\n");
}

/* directly set raw value to chip register, format: 'register value' */
static ssize_t bq2415x_sysfs_set_registers(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf,
					   size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	ssize_t ret = 0;
	unsigned int reg;
	unsigned int val;

	if (sscanf(buf, "%x %x", &reg, &val) != 2)
		return -EINVAL;

	if (reg > 4 || val > 255)
		return -EINVAL;

	ret = bq2415x_i2c_write(bq, reg, val);
	if (ret < 0)
		return ret;
	return count;
}

/* print value of chip register, format: 'register=value' */
static ssize_t bq2415x_sysfs_print_reg(struct bq2415x_device *bq,
				       u8 reg,
				       char *buf)
{
	int ret = bq2415x_i2c_read(bq, reg);

	if (ret < 0)
		return sprintf(buf, "%#.2x=error %d\n", reg, ret);
	return sprintf(buf, "%#.2x=%#.2x\n", reg, ret);
}

/* show all raw values of chip register, format per line: 'register=value' */
static ssize_t bq2415x_sysfs_show_registers(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	ssize_t ret = 0;

	ret += bq2415x_sysfs_print_reg(bq, BQ2415X_REG_STATUS, buf+ret);
	ret += bq2415x_sysfs_print_reg(bq, BQ2415X_REG_CONTROL, buf+ret);
	ret += bq2415x_sysfs_print_reg(bq, BQ2415X_REG_VOLTAGE, buf+ret);
	ret += bq2415x_sysfs_print_reg(bq, BQ2415X_REG_VENDER, buf+ret);
	ret += bq2415x_sysfs_print_reg(bq, BQ2415X_REG_CURRENT, buf+ret);
	return ret;
}

/* set current and voltage limit entries (in mA or mV) */
static ssize_t bq2415x_sysfs_set_limit(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf,
				       size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	long val;
	int ret;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	if (strcmp(attr->attr.name, "current_limit") == 0)
		ret = bq2415x_set_current_limit(bq, val);
	else if (strcmp(attr->attr.name, "weak_battery_voltage") == 0)
		ret = bq2415x_set_weak_battery_voltage(bq, val);
	else if (strcmp(attr->attr.name, "battery_regulation_voltage") == 0)
		ret = bq2415x_set_battery_regulation_voltage(bq, val);
	else if (strcmp(attr->attr.name, "charge_current") == 0)
		ret = bq2415x_set_charge_current(bq, val);
	else if (strcmp(attr->attr.name, "termination_current") == 0)
		ret = bq2415x_set_termination_current(bq, val);
	else
		return -EINVAL;

	if (ret < 0)
		return ret;
	return count;
}

/* show current and voltage limit entries (in mA or mV) */
static ssize_t bq2415x_sysfs_show_limit(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	int ret;

	if (strcmp(attr->attr.name, "current_limit") == 0)
		ret = bq2415x_get_current_limit(bq);
	else if (strcmp(attr->attr.name, "weak_battery_voltage") == 0)
		ret = bq2415x_get_weak_battery_voltage(bq);
	else if (strcmp(attr->attr.name, "battery_regulation_voltage") == 0)
		ret = bq2415x_get_battery_regulation_voltage(bq);
	else if (strcmp(attr->attr.name, "charge_current") == 0)
		ret = bq2415x_get_charge_current(bq);
	else if (strcmp(attr->attr.name, "termination_current") == 0)
		ret = bq2415x_get_termination_current(bq);
	else
		return -EINVAL;

	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

/* set *_enable entries */
static ssize_t bq2415x_sysfs_set_enable(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	enum bq2415x_command command;
	long val;
	int ret;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	if (strcmp(attr->attr.name, "charge_termination_enable") == 0)
		command = val ? BQ2415X_CHARGE_TERMINATION_ENABLE :
			BQ2415X_CHARGE_TERMINATION_DISABLE;
	else if (strcmp(attr->attr.name, "high_impedance_enable") == 0)
		command = val ? BQ2415X_HIGH_IMPEDANCE_ENABLE :
			BQ2415X_HIGH_IMPEDANCE_DISABLE;
	else if (strcmp(attr->attr.name, "otg_pin_enable") == 0)
		command = val ? BQ2415X_OTG_PIN_ENABLE :
			BQ2415X_OTG_PIN_DISABLE;
	else if (strcmp(attr->attr.name, "stat_pin_enable") == 0)
		command = val ? BQ2415X_STAT_PIN_ENABLE :
			BQ2415X_STAT_PIN_DISABLE;
	else
		return -EINVAL;

	ret = bq2415x_exec_command(bq, command);
	if (ret < 0)
		return ret;
	return count;
}

/* show *_enable entries */
static ssize_t bq2415x_sysfs_show_enable(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2415x_device *bq = container_of(psy, struct bq2415x_device,
						 charger);
	enum bq2415x_command command;
	int ret;

	if (strcmp(attr->attr.name, "charge_termination_enable") == 0)
		command = BQ2415X_CHARGE_TERMINATION_STATUS;
	else if (strcmp(attr->attr.name, "high_impedance_enable") == 0)
		command = BQ2415X_HIGH_IMPEDANCE_STATUS;
	else if (strcmp(attr->attr.name, "otg_pin_enable") == 0)
		command = BQ2415X_OTG_PIN_STATUS;
	else if (strcmp(attr->attr.name, "stat_pin_enable") == 0)
		command = BQ2415X_STAT_PIN_STATUS;
	else
		return -EINVAL;

	ret = bq2415x_exec_command(bq, command);
	if (ret < 0)
		return ret;
	return sprintf(buf, "%d\n", ret);
}

static DEVICE_ATTR(current_limit, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_limit, bq2415x_sysfs_set_limit);
static DEVICE_ATTR(weak_battery_voltage, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_limit, bq2415x_sysfs_set_limit);
static DEVICE_ATTR(battery_regulation_voltage, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_limit, bq2415x_sysfs_set_limit);
static DEVICE_ATTR(charge_current, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_limit, bq2415x_sysfs_set_limit);
static DEVICE_ATTR(termination_current, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_limit, bq2415x_sysfs_set_limit);

static DEVICE_ATTR(charge_termination_enable, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_enable, bq2415x_sysfs_set_enable);
static DEVICE_ATTR(high_impedance_enable, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_enable, bq2415x_sysfs_set_enable);
static DEVICE_ATTR(otg_pin_enable, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_enable, bq2415x_sysfs_set_enable);
static DEVICE_ATTR(stat_pin_enable, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_enable, bq2415x_sysfs_set_enable);
static DEVICE_ATTR(timer, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_timer, bq2415x_sysfs_set_timer);

static DEVICE_ATTR(registers, S_IWUSR | S_IRUGO,
		bq2415x_sysfs_show_registers, bq2415x_sysfs_set_registers);

static DEVICE_ATTR(otg_status, S_IRUGO, bq2415x_sysfs_show_status, NULL);
static DEVICE_ATTR(charge_status, S_IRUGO, bq2415x_sysfs_show_status, NULL);
static DEVICE_ATTR(boost_status, S_IRUGO, bq2415x_sysfs_show_status, NULL);
static DEVICE_ATTR(fault_status, S_IRUGO, bq2415x_sysfs_show_status, NULL);

static struct attribute *bq2415x_sysfs_attributes[] = {
	/*
	 * TODO: some (appropriate) of these attrs should be switched to
	 * use power supply class props.
	 */
	&dev_attr_current_limit.attr,
	&dev_attr_weak_battery_voltage.attr,
	&dev_attr_battery_regulation_voltage.attr,
	&dev_attr_charge_current.attr,
	&dev_attr_termination_current.attr,

	&dev_attr_charge_termination_enable.attr,
	&dev_attr_high_impedance_enable.attr,
	&dev_attr_otg_pin_enable.attr,
	&dev_attr_stat_pin_enable.attr,

	&dev_attr_timer.attr,

	&dev_attr_registers.attr,

	&dev_attr_otg_status.attr,
	&dev_attr_charge_status.attr,
	&dev_attr_boost_status.attr,
	&dev_attr_fault_status.attr,
	NULL,
};

static const struct attribute_group bq2415x_sysfs_attr_group = {
	.attrs = bq2415x_sysfs_attributes,
};

static int bq2415x_sysfs_init(struct bq2415x_device *bq)
{
	return sysfs_create_group(&bq->charger.dev->kobj,
			&bq2415x_sysfs_attr_group);
}

static void bq2415x_sysfs_exit(struct bq2415x_device *bq)
{
	sysfs_remove_group(&bq->charger.dev->kobj, &bq2415x_sysfs_attr_group);
}

/* main bq2415x probe function */
static int bq2415x_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int ret;
	int num;
	char *name;
	struct bq2415x_device *bq;

	if (!client->dev.platform_data) {
		dev_err(&client->dev, "platform data not set\n");
		return -ENODEV;
	}

	/* Get new ID for the new device */
	ret = idr_pre_get(&bq2415x_id, GFP_KERNEL);
	if (ret == 0)
		return -ENOMEM;

	mutex_lock(&bq2415x_id_mutex);
	ret = idr_get_new(&bq2415x_id, client, &num);
	mutex_unlock(&bq2415x_id_mutex);

	if (ret < 0)
		return ret;

	name = kasprintf(GFP_KERNEL, "%s-%d", id->name, num);
	if (!name) {
		dev_err(&client->dev, "failed to allocate device name\n");
		ret = -ENOMEM;
		goto error_1;
	}

	bq = devm_kzalloc(&client->dev, sizeof(*bq), GFP_KERNEL);
	if (!bq) {
		dev_err(&client->dev, "failed to allocate device data\n");
		ret = -ENOMEM;
		goto error_2;
	}

	i2c_set_clientdata(client, bq);

	bq->id = num;
	bq->dev = &client->dev;
	bq->chip = id->driver_data;
	bq->name = name;
	bq->autotimer = 0;

	memcpy(&bq->init_data, client->dev.platform_data,
			sizeof(bq->init_data));

	ret = bq2415x_power_supply_init(bq);
	if (ret) {
		dev_err(bq->dev, "failed to register power supply: %d\n", ret);
		goto error_2;
	}

	ret = bq2415x_sysfs_init(bq);
	if (ret) {
		dev_err(bq->dev, "failed to create sysfs entries: %d\n", ret);
		goto error_3;
	}

	wake_lock_init(&bq->wakelock, WAKE_LOCK_SUSPEND, "bq2415x_timer");

	bq->init_data.stat_pin_enable = 0;
	bq->init_data.otg_pin_enable = 0;

	ret = bq2415x_set_mode(bq, BQ2415X_MODE_OFF);
	if (ret) {
		dev_err(bq->dev, "failed to set mode to off: %d\n", ret);
		goto error_4;
	}

	bq->callbacks.set_current_limit = bq2415x_set_current_limit_cb;
	bq->callbacks.set_mode = bq2415x_set_mode_cb;
	if (bq->init_data.register_callbacks) {
		bq->init_data.register_callbacks(&bq->callbacks);
	}

	INIT_DELAYED_WORK(&bq->work, bq2415x_timer_work);

	dev_info(bq->dev, "driver registered\n");
	return 0;

error_4:
	wake_lock_destroy(&bq->wakelock);
	bq2415x_sysfs_exit(bq);
error_3:
	bq2415x_power_supply_exit(bq);
error_2:
	kfree(name);
error_1:
	mutex_lock(&bq2415x_id_mutex);
	idr_remove(&bq2415x_id, num);
	mutex_unlock(&bq2415x_id_mutex);

	return ret;
}

/* main bq2415x remove function */

static int bq2415x_remove(struct i2c_client *client)
{
	struct bq2415x_device *bq = i2c_get_clientdata(client);

	if (bq->init_data.unregister_callbacks)
		bq->init_data.unregister_callbacks();

	bq2415x_sysfs_exit(bq);
	bq2415x_power_supply_exit(bq);

	bq2415x_set_mode(bq, BQ2415X_MODE_OFF);

	mutex_lock(&bq2415x_id_mutex);
	idr_remove(&bq2415x_id, bq->id);
	mutex_unlock(&bq2415x_id_mutex);

	wake_lock_destroy(&bq->wakelock);

	dev_info(bq->dev, "driver unregistered\n");

	kfree(bq->name);

	return 0;
}

static const struct i2c_device_id bq2415x_i2c_id_table[] = {
	{ "bq2415x", BQUNKNOWN },
	{ "bq24150", BQ24150 },
	{ "bq24150a", BQ24150A },
	{ "bq24151", BQ24151 },
	{ "bq24151a", BQ24151A },
	{ "bq24152", BQ24152 },
	{ "bq24153", BQ24153 },
	{ "bq24153a", BQ24153A },
	{ "bq24155", BQ24155 },
	{ "bq24156", BQ24156 },
	{ "bq24156a", BQ24156A },
	{ "bq24158", BQ24158 },
	{},
};
MODULE_DEVICE_TABLE(i2c, bq2415x_i2c_id_table);

static void bq2415x_shutdown(struct i2c_client *client)
{
	struct bq2415x_device *bq = i2c_get_clientdata(client);

	bq2415x_set_mode(bq, BQ2415X_MODE_OFF);

	dev_info(bq->dev, "chip reset/off\n");
}

static struct i2c_driver bq2415x_driver = {
	.driver = {
		.name = "bq2415x-charger",
	},
	.probe = bq2415x_probe,
	.remove = bq2415x_remove,
	.id_table = bq2415x_i2c_id_table,
	.shutdown = bq2415x_shutdown,
};
module_i2c_driver(bq2415x_driver);

MODULE_AUTHOR("Pali Rohár <pali.rohar@gmail.com>");
MODULE_DESCRIPTION("bq2415x charger driver");
MODULE_LICENSE("GPL");
