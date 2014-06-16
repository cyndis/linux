/*
 * NVIDIA Tegra Activity Monitor driver
 *
 * Copyright (C) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:
 *     Mikko Perttunen <mperttunen@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define ACTMON_GLB_STATUS			0x0
#define ACTMON_GLB_PERIOD_CTRL			0x4

#define ACTMON_DEV_CTRL				0x0
#define		ACTMON_DEV_CTRL_K_VAL_SHIFT	10
#define		ACTMON_DEV_CTRL_ENB_PERIODIC	(1<<18)
#define		ACTMON_DEV_CTRL_AT_END_EN	(1<<19)
#define		ACTMON_DEV_CTRL_AVG_BELOW_WMARK_EN (1<<20)
#define		ACTMON_DEV_CTRL_AVG_ABOVE_WMARK_EN (1<<21)
#define		ACTMON_DEV_CTRL_CONSECUTIVE_BELOW_WMARK_NUM_SHIFT 23
#define		ACTMON_DEV_CTRL_CONSECUTIVE_ABOVE_WMARK_NUM_SHIFT 26
#define		ACTMON_DEV_CTRL_CONSECUTIVE_BELOW_WMARK_EN (1<<29)
#define		ACTMON_DEV_CTRL_CONSECUTIVE_ABOVE_WMARK_EN (1<<30)
#define		ACTMON_DEV_CTRL_ENB		(1<<31)
#define ACTMON_DEV_UPPER_WMARK			0x4
#define ACTMON_DEV_LOWER_WMARK			0x8
#define ACTMON_DEV_INIT_AVG			0xc
#define ACTMON_DEV_AVG_UPPER_WMARK		0x10
#define ACTMON_DEV_AVG_LOWER_WMARK		0x14
#define ACTMON_DEV_COUNT_WEIGHT			0x18
#define ACTMON_DEV_AVG_COUNT			0x20
#define ACTMON_DEV_INTR_STATUS			0x24

#define ACTMON_SAMPLING_PERIOD			12 // ms
#define ACTMON_AVERAGE_WINDOW_LOG2		6
#define ACTMON_AVERAGE_BAND			6
#define TEST_MAX_FREQ				1000000

struct tegra_actmon_device_data {
	u32 offset;
	u32 irq_mask;

	u32 boost_frequency_step;
	u8 boost_up_coeff;
	u8 boost_down_coeff;
	u8 boost_up_threshold;
	u8 boost_down_threshold;
	u32 count_weight;
	u8 above_watermark_window;
	u8 below_watermark_window;
};

static struct tegra_actmon_device_data actmon_device_data_t124[] = {
	{
		// MC_ALL
		.offset = 0x1c0,
		.irq_mask = 1 << 26,
		.boost_frequency_step = 16000,
		.boost_up_coeff = 200,
		.boost_down_coeff = 50,
		.boost_up_threshold = 60,
		.boost_down_threshold = 40,
		.above_watermark_window = 1,
		.below_watermark_window = 3,
		.count_weight = 0x400, /* this should probably be more than 1 for MC.. */
	},
};

struct tegra_actmon_device {
	void __iomem *regs;
	const struct tegra_actmon_device_data *data;
	u32 avg_band_freq;
};

struct tegra_actmon {
	void __iomem *regs;
	struct clk *clock;
	struct reset_control *reset;
	struct tegra_actmon_device devices[ARRAY_SIZE(actmon_device_data_t124)];
};

static struct of_device_id tegra_actmon_of_match[] = {
	{ .compatible = "nvidia,tegra124-actmon" },
	{ },
};

static void actmon_write_barrier(struct tegra_actmon *tegra) {
	wmb();
	readl(tegra->regs + ACTMON_GLB_STATUS);
}

void tegra_actmon_init_device(struct tegra_actmon_device *device) {
	u32 val;

	device->avg_band_freq = TEST_MAX_FREQ * ACTMON_AVERAGE_BAND / 1000;

	// DS sets this to dev_clk_freq * actmon_sampling_period
	// this is avg_count
	writel(0, device->regs + ACTMON_DEV_INIT_AVG);

	// Set avg watermark
	writel(device->avg_band_freq * ACTMON_SAMPLING_PERIOD,
	       device->regs + ACTMON_DEV_AVG_UPPER_WMARK);
	writel(0, device->regs + ACTMON_DEV_AVG_LOWER_WMARK);

	// Set watermark

	writel(0, device->regs + ACTMON_DEV_UPPER_WMARK);
	writel(0, device->regs + ACTMON_DEV_LOWER_WMARK);

	// Other stuff

	writel(device->data->count_weight,
	       device->regs + ACTMON_DEV_COUNT_WEIGHT);

	writel(0xffffffff, device->regs + ACTMON_DEV_INTR_STATUS);

	val = 0;
	val |= ACTMON_DEV_CTRL_ENB_PERIODIC |
	       ACTMON_DEV_CTRL_AVG_ABOVE_WMARK_EN |
	       ACTMON_DEV_CTRL_AVG_BELOW_WMARK_EN;
	val |= (ACTMON_AVERAGE_WINDOW_LOG2 - 1)
		<< ACTMON_DEV_CTRL_K_VAL_SHIFT;
	val |= (device->data->below_watermark_window - 1)
		<< ACTMON_DEV_CTRL_CONSECUTIVE_BELOW_WMARK_NUM_SHIFT;
	val |= (device->data->above_watermark_window - 1)
		<< ACTMON_DEV_CTRL_CONSECUTIVE_ABOVE_WMARK_NUM_SHIFT;
	/*
	val |= ACTMON_DEV_CTRL_CONSECUTIVE_BELOW_WMARK_EN |
	       ACTMON_DEV_CTRL_CONSECUTIVE_ABOVE_WMARK_EN;
	*/

/*
	val = 0;
	val |= ACTMON_DEV_CTRL_ENB_PERIODIC | ACTMON_DEV_CTRL_AT_END_EN;
	val |= ACTMON_DEV_CTRL_CONSECUTIVE_ABOVE_WMARK_EN;
	val |= ACTMON_DEV_CTRL_CONSECUTIVE_BELOW_WMARK_EN;
	val |= ACTMON_DEV_CTRL_AVG_ABOVE_WMARK_EN;
	val |= ACTMON_DEV_CTRL_AVG_BELOW_WMARK_EN;
	val |= (ACTMON_AVERAGE_WINDOW_LOG2 - 1)
		<< ACTMON_DEV_CTRL_K_VAL_SHIFT;
	val |= (device->data->below_watermark_window - 1)
		<< ACTMON_DEV_CTRL_CONSECUTIVE_BELOW_WMARK_NUM_SHIFT;
	val |= (device->data->above_watermark_window - 1)
		<< ACTMON_DEV_CTRL_CONSECUTIVE_ABOVE_WMARK_NUM_SHIFT;
*/
	writel(val, device->regs + ACTMON_DEV_CTRL);
}

void tegra_actmon_init(struct tegra_actmon *tegra) {
	writel(ACTMON_SAMPLING_PERIOD - 1,
	       tegra->regs + ACTMON_GLB_PERIOD_CTRL);
}

irqreturn_t actmon_isr(int irq, void *data) {
	struct tegra_actmon *tegra = data;
	struct tegra_actmon_device *device = NULL;
	int i;
	u32 val;

	val = readl(tegra->regs + ACTMON_GLB_STATUS);

	for (i = 0; i < ARRAY_SIZE(tegra->devices); ++i) {
		if (val & tegra->devices[i].data->irq_mask) {
			device = tegra->devices + i;
			break;
		}
	}

	if (!device)
		return IRQ_NONE;

	// Clear interrupt

	printk("actmon: isr, intr %08x, dev %i\n", readl(device->regs + ACTMON_DEV_INTR_STATUS), i);

	writel(0xffffffff, device->regs + ACTMON_DEV_INTR_STATUS);

	u32 avg_count = readl(device->regs + ACTMON_DEV_AVG_COUNT);
	if (avg_count > 0)
		printk("actmon avg_count %d\n", avg_count);

	return IRQ_HANDLED;
}

irqreturn_t actmon_thread_isr(int irq, void *data) {
	return IRQ_HANDLED;
}

static int tegra_actmon_probe(struct platform_device *pdev)
{
	int err = 0;
	int i;
	int irq;
	struct tegra_actmon *tegra;

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	tegra->regs = devm_ioremap_resource(&pdev->dev,
		platform_get_resource(pdev, IORESOURCE_MEM, 0));
	if (IS_ERR(tegra->regs)) {
		dev_err(&pdev->dev, "Failed to get IO memory\n");
		return PTR_ERR(tegra->regs);
	}

	tegra->reset = devm_reset_control_get(&pdev->dev, "actmon");
	if (IS_ERR(tegra->reset)) {
		dev_err(&pdev->dev, "Failed to get reset\n");
		return PTR_ERR(tegra->reset);
	}

	tegra->clock = devm_clk_get(&pdev->dev, "actmon");
	if (IS_ERR(tegra->clock)) {
		dev_err(&pdev->dev, "Failed to get actmon clock\n");
		return PTR_ERR(tegra->clock);
	}

	irq = platform_get_irq(pdev, 0);
	printk("actmon: irq = %d\n", irq);
	err = devm_request_threaded_irq(&pdev->dev, irq, actmon_isr,
	                                actmon_thread_isr, 0, "tegra-actmon",
	                                tegra);
	if (err) {
		dev_err(&pdev->dev, "Interrupt request failed\n");
		return err;
	}

	reset_control_assert(tegra->reset);

	err = clk_prepare_enable(tegra->clock);
	if (err) {
		reset_control_deassert(tegra->reset);
		return err;
	}

	reset_control_deassert(tegra->reset);

	tegra_actmon_init(tegra);

	for (i = 0; i < ARRAY_SIZE(actmon_device_data_t124); ++i) {
		struct tegra_actmon_device_data *data =
			actmon_device_data_t124 + i;
		struct tegra_actmon_device *device = tegra->devices + i;
		device->data = data;
		device->regs = tegra->regs + data->offset;
		/*
		device->clk = devm_clk_get(&pdev->dev, data->clock_name);
		if (IS_ERR(device->clk)) {
			dev_err(&pdev->dev, "Failed to get %s clock\n",
				data->clock_name);
			return PTR_ERR(device->clk);
		}
		*/

		tegra_actmon_init_device(device);

		actmon_write_barrier(tegra);

		u32 val;
		val = readl(device->regs + ACTMON_DEV_CTRL);
		val |= ACTMON_DEV_CTRL_ENB;
		writel(val, device->regs + ACTMON_DEV_CTRL);
		actmon_write_barrier(tegra);
	}

	platform_set_drvdata(pdev, tegra);

	return 0;
}

static int tegra_actmon_remove(struct platform_device *pdev)
{
	struct tegra_actmon *tegra = platform_get_drvdata(pdev);

	clk_disable_unprepare(tegra->clock);

	return 0;
}

static struct platform_driver tegra_actmon_driver = {
	.probe		= tegra_actmon_probe,
	.remove		= tegra_actmon_remove,
	.driver		= {
		.name	= "tegra-actmon",
		.of_match_table = tegra_actmon_of_match,
	}
};
module_platform_driver(tegra_actmon_driver);

MODULE_DESCRIPTION("Tegra Activity Monitor driver");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, tegra_actmon_of_match);
