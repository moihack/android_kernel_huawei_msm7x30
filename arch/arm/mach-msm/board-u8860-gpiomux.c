/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/mach-types.h>
#include <mach/gpio.h>
#include <mach/gpiomux.h>
#include <mach/socinfo.h>
#include "devices.h"

static struct gpiomux_setting ts_int_sus_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting ts_resout_sus_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting i2c_dcdc_sus_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_OUT_HIGH,
};

static struct gpiomux_setting apds_int_sus_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_UP,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting akm_int_sus_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct msm_gpiomux_config u8800_standard_configs[] __initdata = {
	{	/* TOUCH_RESET */
		.gpio = 85,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &ts_resout_sus_cfg,
		},
	},
	{
		/* APDS_INT */
		.gpio = 89,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &apds_int_sus_cfg,
		},
	},
	{
		/* AKM_INT */
		.gpio = 132,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &akm_int_sus_cfg,
		},
	},
	{	/* TOUCH_INT */
		.gpio = 148,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &ts_int_sus_cfg,
		},
	},
	{	/* I2C_SCL_DCDC */
		.gpio = 149,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &i2c_dcdc_sus_cfg,
		},
	},
	{	/* I2C_SDA_DCDC */
		.gpio = 150,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &i2c_dcdc_sus_cfg,
		},
	},
};

static struct gpiomux_setting mdp_vsync_cfg = {
	.func = GPIOMUX_FUNC_1,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_DOWN,
	.dir = GPIOMUX_IN,
};

static struct gpiomux_setting audio_pamp_cfg = {
	.func = GPIOMUX_FUNC_GPIO,
	.drv = GPIOMUX_DRV_2MA,
	.pull = GPIOMUX_PULL_NONE,
	.dir = GPIOMUX_OUT_LOW,
};

static struct msm_gpiomux_config msm7x30_standard_configs[] __initdata = {
	{	/* MDP_VSYNC */
		.gpio = 30,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &mdp_vsync_cfg,
		},
	},
	{	/* AUDIO_PAMP */
		.gpio = 82,
		.settings = {
			[GPIOMUX_SUSPENDED]	= &audio_pamp_cfg,
		},
	},
};

int __init msm7x30_init_gpiomux(void)
{
	int rc = msm_gpiomux_init(NR_GPIO_IRQS);
	if (rc) {
		pr_err(KERN_ERR "msm_gpiomux_init failed %d\n", rc);
		return rc;
	}

	msm_gpiomux_install(u8800_standard_configs,
			ARRAY_SIZE(u8800_standard_configs));

	msm_gpiomux_install(msm7x30_standard_configs,
			ARRAY_SIZE(msm7x30_standard_configs));

	return rc;
}

postcore_initcall(msm7x30_init_gpiomux);
