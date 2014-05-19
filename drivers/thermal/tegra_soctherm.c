/*
 * drivers/thermal/tegra_soctherm.c
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:
 *	Mikko Perttunen <mperttunen@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <../../arch/arm/mach-tegra/fuse.h>

#define SENSOR_CONFIG0				0
#define		SENSOR_CONFIG0_STOP		(1<<0)
#define 	SENSOR_CONFIG0_TALL_SHIFT	8
#define		SENSOR_CONFIG0_TCALC_OVER	(1<<4)
#define		SENSOR_CONFIG0_TCALC_OVER_SHIFT	4
#define		SENSOR_CONFIG0_OVER		(1<<3)
#define		SENSOR_CONFIG0_OVER_SHIFT	3
#define		SENSOR_CONFIG0_CPTR_OVER	(1<<2)
#define		SENSOR_CONFIG0_CPTR_OVER_SHIFT	2
#define SENSOR_CONFIG1				4
#define 	SENSOR_CONFIG1_TSAMPLE_SHIFT	0
#define 	SENSOR_CONFIG1_TIDDQ_EN_SHIFT	15
#define 	SENSOR_CONFIG1_TEN_COUNT_SHIFT	24
#define		SENSOR_CONFIG1_TEMP_ENABLE	(1<<31)
#define		SENSOR_CONFIG1_TEMP_ENABLE_SHIFT	31
#define SENSOR_CONFIG2				8
#define		SENSOR_CONFIG2_THERMA_SHIFT	16
#define		SENSOR_CONFIG2_THERMB_SHIFT	0
#define SENSOR_STATUS0				12
#define		SENSOR_STATUS0_CAPTURE_MASK	0xffff
#define		SENSOR_STATUS0_CAPTURE_VALID	(1<<31)
#define		SENSOR_STATUS0_CAPTURE_VALID_SHIFT	31
#define SENSOR_STATUS1				16
#define		SENSOR_STATUS1_TEMP_VALID	(1<<31)
#define		SENSOR_STATUS1_TEMP_VALID_SHIFT	31
#define		SENSOR_STATUS1_TEMP_MASK	0xffff
#define SENSOR_STATUS2				20

#define THERMTRIP_CTL				0x80
#define		THERMTRIP_CTL_ANY_EN		(1<<28)
#define		THERMTRIP_CTL_TSENSE_MASK	0xff
#define		THERMTRIP_CTL_TSENSE_SHIFT	0
#define		THERMTRIP_CTL_CPU_MASK		0xff00
#define		THERMTRIP_CTL_CPU_SHIFT		8
#define		THERMTRIP_CTL_GPU_MEM_MASK	0xff0000
#define		THERMTRIP_CTL_GPU_MEM_SHIFT	16
#define THERMTRIP_DEFAULT_THRESHOLD		105

#define SENSOR_PDIV				0x1c0
#define		SENSOR_PDIV_T124		0x8888
#define SENSOR_HOTSPOT_OFF			0x1c4
#define		SENSOR_HOTSPOT_OFF_T124		0x00060600
#define SENSOR_TEMP1				0x1c8
#define		SENSOR_TEMP1_CPU_TEMP_SHIFT	16
#define		SENSOR_TEMP1_GPU_TEMP_MASK	0xffff
#define SENSOR_TEMP2				0x1cc
#define		SENSOR_TEMP2_MEM_TEMP_SHIFT	16
#define		SENSOR_TEMP2_PLLX_TEMP_MASK	0xffff

#define FUSE_TSENSOR8_CALIB			0x280
#define FUSE_SPARE_REALIGNMENT_REG_0		0x2fc

#define NOMINAL_CALIB_FT_T124			105

struct tegra_tsensor_configuration {
	u32 tall, tsample, tiddq_en, ten_count;
	u32 pdiv, tsample_ate, pdiv_ate;
};

struct tegra_tsensor {
	const char *name;
	u32 base;
	struct tegra_tsensor_configuration *config;
	u32 calib_fuse_offset;
	s32 fuse_corr_alpha, fuse_corr_beta;
};

struct tegra_thermctl_zone {
	struct tegra_soctherm *tegra;
	int sensor;
};

static struct tegra_tsensor_configuration t124_tsensor_config = {
	.tall = 16300,
	.tsample = 120,
	.tiddq_en = 1,
	.ten_count = 1,
	.pdiv = 8,
	.tsample_ate = 481,
	.pdiv_ate = 8
};

static struct tegra_tsensor t124_tsensors[] = {
	{
		.base = 0xc0,
		.name = "cpu0",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x198,
		.fuse_corr_alpha = 1135400,
		.fuse_corr_beta = -6266900,
	},
	{
		.base = 0xe0,
		.name = "cpu1",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x184,
		.fuse_corr_alpha = 1122220,
		.fuse_corr_beta = -5700700,
	},
	{
		.base = 0x100,
		.name = "cpu2",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x188,
		.fuse_corr_alpha = 1127000,
		.fuse_corr_beta = -6768200,
	},
	{
		.base = 0x120,
		.name = "cpu3",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x22c,
		.fuse_corr_alpha = 1110900,
		.fuse_corr_beta = -6232000,
	},
	{
		.base = 0x140,
		.name = "mem0",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x258,
		.fuse_corr_alpha = 1122300,
		.fuse_corr_beta = -5936400,
	},
	{
		.base = 0x160,
		.name = "mem1",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x25c,
		.fuse_corr_alpha = 1145700,
		.fuse_corr_beta = -7124600,
	},
	{
		.base = 0x180,
		.name = "gpu",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x254,
		.fuse_corr_alpha = 1120100,
		.fuse_corr_beta = -6000500,
	},
	{
		.base = 0x1a0,
		.name = "pllx",
		.config = &t124_tsensor_config,
		.calib_fuse_offset = 0x260,
		.fuse_corr_alpha = 1106500,
		.fuse_corr_beta = -6729300,
	},
	{ .name = NULL },
};

struct tegra_soctherm {
	struct reset_control *reset;
	struct clk *clock_tsensor;
	struct clk *clock_soctherm;
	void __iomem *regs;

	struct thermal_zone_device *tsensor_tzs[ARRAY_SIZE(t124_tsensors)];
	struct thermal_zone_device *thermctl_tzs[4];
};

struct tsensor_shared_calibration {
	u32 base_cp, base_ft;
	u32 actual_temp_cp, actual_temp_ft;
};

static struct tsensor_shared_calibration calculate_shared_calibration(void)
{
	struct tsensor_shared_calibration r;
	u32 val;
	u32 shifted_cp, shifted_ft;

	val = tegra_fuse_readl(FUSE_TSENSOR8_CALIB);
	r.base_cp = val & 0x3ff;
	r.base_ft = (val & (0x7ff << 10)) >> 10;

	val = tegra_fuse_readl(FUSE_SPARE_REALIGNMENT_REG_0);
	// Sign extend from 6 bits to 32 bits
	shifted_cp = (s32)((val & 0x1f) | ((val & 0x20) ? 0xffffffe0 : 0x0));
	val = ((val & (0x1f << 21)) >> 21);
	// Sign extend from 5 bits to 32 bits
	shifted_ft = (s32)((val & 0xf) | ((val & 0x10) ? 0xfffffff0 : 0x0));

	r.actual_temp_cp = 2 * 25 + shifted_cp;
	r.actual_temp_ft = 2 * NOMINAL_CALIB_FT_T124 + shifted_ft;

	return r;
}

static u32 calculate_tsensor_calibration(
	struct tegra_tsensor *sensor,
	struct tsensor_shared_calibration shared
)
{
	u32 val;
	s32 actual_tsensor_ft, actual_tsensor_cp;
	s32 delta_sens, delta_temp;
	s32 mult, div;
	s16 therma, thermb;

	val = tegra_fuse_readl(sensor->calib_fuse_offset);

	// Sign extend from 13 bits to 32 bits
	actual_tsensor_cp = (shared.base_cp * 64) +
		(s32)((val & 0xfff) | ((val & 0x1000) ? 0xfffff000 : 0x0));
	val = (val & (0x1fff << 13)) >> 13;
	// Sign extend from 13 bits to 32 bits
	actual_tsensor_ft = (shared.base_ft * 32) +
		(s32)((val & 0xfff) | ((val & 0x1000) ? 0xfffff000 : 0x0));

	delta_sens = actual_tsensor_ft - actual_tsensor_cp;
	delta_temp = shared.actual_temp_ft - shared.actual_temp_cp;

	mult = sensor->config->pdiv * sensor->config->tsample_ate;
	div = sensor->config->tsample * sensor->config->pdiv_ate;

	therma = div_s64((s64) delta_temp * (1LL << 13) * mult,
			 (s64) delta_sens * div);
	thermb = div_s64( ((s64) actual_tsensor_ft * shared.actual_temp_cp) -
	                  ((s64) actual_tsensor_cp * shared.actual_temp_ft),
	                  (s64) delta_sens );

	therma = div_s64((s64) therma * sensor->fuse_corr_alpha,
			 (s64) 1000000LL);
	thermb = div_s64((s64) thermb * sensor->fuse_corr_alpha +
				sensor->fuse_corr_beta,
			 (s64) 1000000LL);

	return ((u16)(therma) << SENSOR_CONFIG2_THERMA_SHIFT) |
		((u16)thermb << SENSOR_CONFIG2_THERMB_SHIFT);
}

static void enable_tsensor(struct tegra_soctherm *tegra,
			  struct tegra_tsensor *sensor,
			  struct tsensor_shared_calibration shared)
{
	void * __iomem base = tegra->regs + sensor->base;
	unsigned int val;

	val = 0;
	val |= sensor->config->tall << SENSOR_CONFIG0_TALL_SHIFT;
	writel(val, base + SENSOR_CONFIG0);

	val = 0;
	val |= (sensor->config->tsample - 1) << SENSOR_CONFIG1_TSAMPLE_SHIFT;
	val |= sensor->config->tiddq_en << SENSOR_CONFIG1_TIDDQ_EN_SHIFT;
	val |= sensor->config->ten_count << SENSOR_CONFIG1_TEN_COUNT_SHIFT;
	val |= SENSOR_CONFIG1_TEMP_ENABLE;
	writel(val, base + SENSOR_CONFIG1);

	writel(calculate_tsensor_calibration(sensor, shared),
	       base + SENSOR_CONFIG2);
}

static inline long translate_temp(u32 val)
{
	long t;

	t = ((val & 0xff00) >> 8) * 1000;
	if (val & 0x80)
		t += 500;
	if (val & 0x01)
		t *= -1;

	return t;
}

static int tegra_thermctl_get_temp(void *data, long *out_temp)
{
	struct tegra_thermctl_zone *zone = data;
	u32 val;

	switch (zone->sensor) {
	case 0:
		val = readl(zone->tegra->regs + SENSOR_TEMP1)
			>> SENSOR_TEMP1_CPU_TEMP_SHIFT;
		break;
	case 1:
		val = readl(zone->tegra->regs + SENSOR_TEMP2)
			>> SENSOR_TEMP2_MEM_TEMP_SHIFT;
		break;
	case 2:
		val = readl(zone->tegra->regs + SENSOR_TEMP1)
			& SENSOR_TEMP1_GPU_TEMP_MASK;
		break;
	case 3:
		val = readl(zone->tegra->regs + SENSOR_TEMP2)
			& SENSOR_TEMP2_PLLX_TEMP_MASK;
		break;
	default:
		BUG();
	}

	*out_temp = translate_temp(val);

	return 0;
}

static struct of_device_id tegra_soctherm_of_match[] = {
	{ .compatible = "nvidia,tegra124-soctherm" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_soctherm_of_match);

static int tegra_soctherm_probe(struct platform_device *pdev)
{
	struct tegra_soctherm *tegra;
	struct thermal_zone_device *tz;
	struct tsensor_shared_calibration shared_calib;
	int i;
	int err = 0;
	u32 val, prop;

	struct tegra_tsensor *tsensors = t124_tsensors;

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	tegra->regs = devm_ioremap_resource(&pdev->dev,
		platform_get_resource(pdev, IORESOURCE_MEM, 0));
	if (IS_ERR(tegra->regs)) {
		dev_err(&pdev->dev, "can't get registers");
		return PTR_ERR(tegra->regs);
	}

	tegra->reset = devm_reset_control_get(&pdev->dev, "soc-therm");
	if (IS_ERR(tegra->reset)) {
		dev_err(&pdev->dev, "can't get soc-therm reset\n");
		return PTR_ERR(tegra->reset);
	}

	tegra->clock_tsensor = devm_clk_get(&pdev->dev, "tsensor");
	if (IS_ERR(tegra->clock_tsensor)) {
		dev_err(&pdev->dev, "can't get clock tsensor\n");
		return PTR_ERR(tegra->clock_tsensor);
	}

	tegra->clock_soctherm = devm_clk_get(&pdev->dev, "soc-therm");
	if (IS_ERR(tegra->clock_soctherm)) {
		dev_err(&pdev->dev, "can't get clock soc-therm\n");
		return PTR_ERR(tegra->clock_soctherm);
	}

	reset_control_assert(tegra->reset);

	err = clk_prepare_enable(tegra->clock_soctherm);
	if (err) {
		reset_control_deassert(tegra->reset);
		return err; // FIXME teardown
	}

	err = clk_prepare_enable(tegra->clock_tsensor);
	if (err) {
		reset_control_deassert(tegra->reset);
		return err; // FIXME teardown
	}

	reset_control_deassert(tegra->reset);

	/* Initialize tsensors */

	shared_calib = calculate_shared_calibration();

	for (i = 0; tsensors[i].name; ++i)
		enable_tsensor(tegra, tsensors + i, shared_calib);

	writel(SENSOR_PDIV_T124, tegra->regs + SENSOR_PDIV);
	writel(SENSOR_HOTSPOT_OFF_T124, tegra->regs + SENSOR_HOTSPOT_OFF);

	/* Initialize thermctl sensors */

	for (i = 0; i < 4; ++i) {
		struct tegra_thermctl_zone *zone =
			devm_kzalloc(&pdev->dev, sizeof(*zone), GFP_KERNEL);
		if (!zone) {
			err = -ENOMEM;
			goto unregister_tzs;
		}

		zone->sensor = i;
		zone->tegra = tegra;

		tz = thermal_zone_of_sensor_register(
			&pdev->dev, i, zone, tegra_thermctl_get_temp, NULL);
		if (IS_ERR(tz)) {
			err = PTR_ERR(tz);
			goto unregister_tzs;
		}

		tegra->thermctl_tzs[i] = tz;
	}

	val = 0;
	val |= THERMTRIP_CTL_ANY_EN;

	if (of_property_read_u32(
		pdev->dev.of_node, "nvidia,thermtrip-threshold-cpu", &prop))
		prop = THERMTRIP_DEFAULT_THRESHOLD;
	val |= (s8)prop << THERMTRIP_CTL_CPU_SHIFT;

	if (of_property_read_u32(
		pdev->dev.of_node, "nvidia,thermtrip-threshold-gpu-mem", &prop))
		prop = THERMTRIP_DEFAULT_THRESHOLD;
	val |= (s8)prop << THERMTRIP_CTL_GPU_MEM_SHIFT;

	if (of_property_read_u32(
		pdev->dev.of_node, "nvidia,thermtrip-threshold-tsense", &prop))
		prop = THERMTRIP_DEFAULT_THRESHOLD;
	val |= (s8)prop << THERMTRIP_CTL_TSENSE_SHIFT;

	writel(val, tegra->regs + THERMTRIP_CTL);

	dev_info(&pdev->dev, "Thermal reset thresholds set up:\n"
		"  cpu %hhd gpu/mem %hhd tsense %hhd\n",
		(s8)((val & THERMTRIP_CTL_CPU_MASK) >> THERMTRIP_CTL_CPU_SHIFT),
		(s8)((val & THERMTRIP_CTL_GPU_MEM_MASK) >>
			THERMTRIP_CTL_GPU_MEM_SHIFT),
		(s8)((val & THERMTRIP_CTL_TSENSE_MASK) >>
			THERMTRIP_CTL_TSENSE_SHIFT));

	return 0;

unregister_tzs:
	for (--i; i >= 0; i--)
		thermal_zone_of_sensor_unregister(&pdev->dev,
						  tegra->thermctl_tzs[i]);

	clk_disable_unprepare(tegra->clock_tsensor);
	clk_disable_unprepare(tegra->clock_soctherm);

	return err;
}

static int tegra_soctherm_remove(struct platform_device *pdev) {
	struct tegra_soctherm *tegra = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < ARRAY_SIZE(tegra->thermctl_tzs); ++i) {
		thermal_zone_of_sensor_unregister(&pdev->dev,
						  tegra->thermctl_tzs[i]);
	}

	clk_disable_unprepare(tegra->clock_tsensor);
	clk_disable_unprepare(tegra->clock_soctherm);

	return 0;
}

static struct platform_driver tegra_soctherm_driver = {
	.probe = tegra_soctherm_probe,
	.remove = tegra_soctherm_remove,
	.driver = {
		.name = "tegra_soctherm",
		.owner = THIS_MODULE,
		.of_match_table = tegra_soctherm_of_match,
	},
};
module_platform_driver(tegra_soctherm_driver);

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_DESCRIPTION("Tegra SOCTHERM thermal management driver");
MODULE_LICENSE("GPL v2");
