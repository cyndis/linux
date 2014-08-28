/*
 * Copyright (C) 2014 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_CAR_H__
#define __SOC_TEGRA_CAR_H__

static int tegra124_car_initiate_emc_switch(u8 parent, u8 packed_divisor)
{
	return -ENOTSUPP;
}

#endif /* __SOC_TEGRA_CAR_H__ */
