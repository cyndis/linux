/*
 * Copyright (C) 2014 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_MC_H__
#define __SOC_TEGRA_MC_H__

static int tegra124_mc_write_emem_configuration(unsigned long rate) {
	return -ENOTSUPP;
}

static int tegra124_mc_get_emem_device_count(u8 *count) {
	return -ENOTSUPP;
}

#endif /* __SOC_TEGRA_MC_H__ */
