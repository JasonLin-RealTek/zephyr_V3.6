/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2024 Realtek Semiconductor Corporation, SIBG-SD7
 * Author: Lin Yu-Cheng <lin_yu_cheng@realtek.com>
 */

#define DT_DRV_COMPAT realtek_rts5912_sccon

#include <zephyr/logging/log.h>
#include <zephyr/drivers/clock_control/clock_control_rts5912.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/rts5912_clock.h>

#include <reg/reg_system.h>

LOG_MODULE_REGISTER(sccon, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

#define RC25M_FREQ DT_PROP(DT_INST_CLOCKS_CTLR_BY_NAME(0, rc25m), clock_frequency)
#define PLL_FREQ   DT_PROP(DT_INST_CLOCKS_CTLR_BY_NAME(0, pll), clock_frequency)

struct rts5912_sccon_config {
	uint32_t reg_base;
	uint8_t sysclk_src;
	uint8_t sysclk_div;
};

static int rts5912_periph_clock_control(const struct device *dev, clock_control_subsys_t sub_system,
					bool on_off)
{
	return 0;
}

static int rts5912_clock_control_on(const struct device *dev, clock_control_subsys_t sub_system)
{
	return rts5912_periph_clock_control(dev, sub_system, true);
}

static int rts5912_clock_control_off(const struct device *dev, clock_control_subsys_t sub_system)
{
	return rts5912_periph_clock_control(dev, sub_system, false);
}

static int rts5912_clock_control_get_rate(const struct device *dev,
					  clock_control_subsys_t sub_system, uint32_t *rate)
{
	return 0;
}

static DEVICE_API(clock_control, rts5912_clock_control_api) = {
	.on = rts5912_clock_control_on,
	.off = rts5912_clock_control_off,
	.get_rate = rts5912_clock_control_get_rate,
};

static int rts5912_clock_control_init(const struct device *dev)
{
	const struct rts5912_sccon_config *const config = dev->config;
	SYSTEM_Type *sys_reg = (SYSTEM_Type *)config->reg_base;

	/* Setup system clock */
	// sys_reg->SYSCLK = (config->sysclk_src << SYSTEM_SYSCLK_SRC_Pos) |
	// 		   (config->sysclk_div << SYSTEM_SYSCLK_DIV_Pos);

	return 0;
}

const struct rts5912_sccon_config rts5912_sccon_config = {
	.reg_base = DT_INST_REG_ADDR_BY_IDX(0, 0),
	.sysclk_src = 1,
	.sysclk_div = 0,
};

DEVICE_DT_INST_DEFINE(0, &rts5912_clock_control_init, NULL, NULL, &rts5912_sccon_config,
		      PRE_KERNEL_1, CONFIG_CLOCK_CONTROL_INIT_PRIORITY, &rts5912_clock_control_api);
