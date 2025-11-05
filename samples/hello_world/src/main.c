/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>

/* size of stack area used by each thread */
#define STACKSIZE 1024

/* scheduling priority used by each thread */
#define PRIORITY 7

void blink0(void)
{
	while(1){
		printf("blink0\n");
		k_msleep(15000);
	}
	
}

const struct device *uart_srap_dev = DEVICE_DT_GET(DT_NODELABEL(uart1_wrapper));

void uart_wrap_pin(const struct device *dev);
int main(void)
{
	while(1){
printf("Hello World! %s\n", CONFIG_BOARD_TARGET);
k_msleep(30000);
	//uart_wrap_pin(uart_srap_dev);

	}
	
	

	return 0;
}



K_THREAD_DEFINE(blink0_id, STACKSIZE, blink0, NULL, NULL, NULL,
		PRIORITY, 0, 0);