/*
 * drivers/ata/ahci_tegra.c
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

#include <linux/ahci_platform.h>
#include <linux/reset.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/tegra-powergate.h>
#include <linux/regulator/consumer.h>
#include <linux/tegra-soc.h>
#include "ahci.h"

#define SATA_CONFIGURATION_0				0x180
#define		SATA_CONFIGURATION_EN_FPCI		(1<<0)

#define SCFG_OFFSET					0x1000

#define T_SATA0_CFG_1					0x04
#define		T_SATA0_CFG_1_IO_SPACE			(1<<0)
#define		T_SATA0_CFG_1_MEMORY_SPACE		(1<<1)
#define		T_SATA0_CFG_1_BUS_MASTER		(1<<2)
#define		T_SATA0_CFG_1_SERR			(1<<8)

#define T_SATA0_CFG_9					0x24

#define SATA_FPCI_BAR5					0x94

#define SATA_INTR_MASK					0x188
#define		SATA_INTR_MASK_IP_INT_MASK		(1<<16)

#define T_SATA0_AHCI_HBA_CAP_BKDR			0x300

#define T_SATA0_BKDOOR_CC				0x4a4

#define T_SATA0_CFG_SATA				0x54c
#define		T_SATA0_CFG_SATA_BACKDOOR_PROG_IF_EN	(1<<12)

#define T_SATA0_CFG_MISC				0x550

#define T_SATA0_INDEX					0x680

#define T_SATA0_CHX_PHY_CTRL1_GEN1			0x690
#define		T_SATA0_CHX_PHY_CTRL1_GEN1_TX_AMP_MASK	0xff
#define		T_SATA0_CHX_PHY_CTRL1_GEN1_TX_AMP_SHIFT	0
#define		T_SATA0_CHX_PHY_CTRL1_GEN1_TX_PEAK_MASK	(0xff<<8)
#define		T_SATA0_CHX_PHY_CTRL1_GEN1_TX_PEAK_SHIFT 8

#define T_SATA0_CHX_PHY_CTRL1_GEN2			0x694
#define		T_SATA0_CHX_PHY_CTRL1_GEN2_TX_AMP_MASK	0xff
#define		T_SATA0_CHX_PHY_CTRL1_GEN2_TX_AMP_SHIFT	0
#define		T_SATA0_CHX_PHY_CTRL1_GEN2_TX_PEAK_MASK	(0xff<<12)
#define		T_SATA0_CHX_PHY_CTRL1_GEN2_TX_PEAK_SHIFT 12

#define T_SATA0_CHX_PHY_CTRL2				0x69c
#define		T_SATA0_CHX_PHY_CTRL2_CDR_CNTL_GEN1	0x23

#define T_SATA0_CHX_PHY_CTRL11				0x6d0
#define		T_SATA0_CHX_PHY_CTRL11_GEN2_RX_EQ	(0x2800<<16)

struct sata_pad_calibration {
	u8 gen1_tx_amp, gen1_tx_peak, gen2_tx_amp, gen2_tx_peak;
};

static const struct sata_pad_calibration tegra124_pad_calibration[] = {
	{0x18, 0x04, 0x18, 0x0a},
	{0x0e, 0x04, 0x14, 0x0a},
	{0x0e, 0x07, 0x1a, 0x0e},
	{0x14, 0x0e, 0x1a, 0x0e},
};

struct tegra_ahci_priv {
	struct platform_device *pdev;
	void __iomem *sata_regs;
	struct reset_control *sata_rst;
	struct reset_control *sata_oob_rst;
	struct reset_control *sata_cold_rst;
	struct regulator_bulk_data supplies[3];
};

static inline void sata_writel(struct tegra_ahci_priv *tegra, u32 value,
				 unsigned long offset)
{
	writel(value, tegra->sata_regs + offset);
}

static inline u32 sata_readl(struct tegra_ahci_priv *tegra,
				unsigned long offset)
{
	return readl(tegra->sata_regs + offset);
}

static int tegra_ahci_power_on(struct ahci_host_priv *hpriv)
{
	struct tegra_ahci_priv *tegra = hpriv->plat_data;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(tegra->supplies),
				    tegra->supplies);
	if (ret)
		return ret;

	reset_control_assert(tegra->sata_rst);
	reset_control_assert(tegra->sata_oob_rst);
	reset_control_assert(tegra->sata_cold_rst);

	ret = tegra_powergate_power_on(TEGRA_POWERGATE_SATA);
	if (ret)
		goto reset_deassert;

	/* Enable clocks */
	ret = ahci_platform_enable_resources(hpriv);
	if (ret)
		goto power_off;

	udelay(10);

	ret = tegra_powergate_remove_clamping(TEGRA_POWERGATE_SATA);
	if (ret)
		goto disable_resources;

	udelay(10);

	reset_control_deassert(tegra->sata_cold_rst);
	reset_control_deassert(tegra->sata_oob_rst);
	reset_control_deassert(tegra->sata_rst);

	return 0;

disable_resources:
	ahci_platform_disable_resources(hpriv);

power_off:
	tegra_powergate_power_off(TEGRA_POWERGATE_SATA);

reset_deassert:
	reset_control_deassert(tegra->sata_cold_rst);
	reset_control_deassert(tegra->sata_oob_rst);
	reset_control_deassert(tegra->sata_rst);

	return ret;
}

static void tegra_ahci_power_off(struct ahci_host_priv *hpriv)
{
	struct tegra_ahci_priv *tegra = hpriv->plat_data;

	reset_control_assert(tegra->sata_rst);
	reset_control_assert(tegra->sata_oob_rst);
	reset_control_assert(tegra->sata_cold_rst);

	ahci_platform_disable_resources(hpriv);

	tegra_powergate_power_off(TEGRA_POWERGATE_SATA);

	reset_control_deassert(tegra->sata_cold_rst);
	reset_control_deassert(tegra->sata_oob_rst);
	reset_control_deassert(tegra->sata_rst);

	regulator_bulk_disable(ARRAY_SIZE(tegra->supplies), tegra->supplies);
}

static int tegra_ahci_controller_init(struct ahci_host_priv *hpriv)
{
	struct tegra_ahci_priv *tegra = hpriv->plat_data;
	int ret;
	unsigned int val;
	struct sata_pad_calibration calib;

	ret = tegra_ahci_power_on(hpriv);
	if (ret) {
		dev_err(&tegra->pdev->dev,
			"failed to power on ahci controller: %d\n", ret);
		return ret;
	}

	val = sata_readl(tegra, SATA_CONFIGURATION_0);
	val |= SATA_CONFIGURATION_EN_FPCI;
	sata_writel(tegra, val, SATA_CONFIGURATION_0);

	/* Pad calibration */

	ret = tegra_fuse_readl(0x224, &val);
	if (ret) {
		dev_err(&tegra->pdev->dev,
			"failed to read sata calibration fuse: %d\n", ret);
		return ret;
	}

	calib = tegra124_pad_calibration[val];

	sata_writel(tegra, (1 << 0), SCFG_OFFSET + T_SATA0_INDEX);

	val = sata_readl(tegra, SCFG_OFFSET + T_SATA0_CHX_PHY_CTRL1_GEN1);
	val &= ~T_SATA0_CHX_PHY_CTRL1_GEN1_TX_AMP_MASK;
	val &= ~T_SATA0_CHX_PHY_CTRL1_GEN1_TX_PEAK_MASK;
	val |= calib.gen1_tx_amp <<
			T_SATA0_CHX_PHY_CTRL1_GEN1_TX_AMP_SHIFT;
	val |= calib.gen1_tx_peak <<
			T_SATA0_CHX_PHY_CTRL1_GEN1_TX_PEAK_SHIFT;
	sata_writel(tegra, val, SCFG_OFFSET + T_SATA0_CHX_PHY_CTRL1_GEN1);

	val = sata_readl(tegra, SCFG_OFFSET + T_SATA0_CHX_PHY_CTRL1_GEN2);
	val &= ~T_SATA0_CHX_PHY_CTRL1_GEN2_TX_AMP_MASK;
	val &= ~T_SATA0_CHX_PHY_CTRL1_GEN2_TX_PEAK_MASK;
	val |= calib.gen2_tx_amp <<
			T_SATA0_CHX_PHY_CTRL1_GEN1_TX_AMP_SHIFT;
	val |= calib.gen2_tx_peak <<
			T_SATA0_CHX_PHY_CTRL1_GEN1_TX_PEAK_SHIFT;
	sata_writel(tegra, val, SCFG_OFFSET + T_SATA0_CHX_PHY_CTRL1_GEN2);

	sata_writel(tegra, T_SATA0_CHX_PHY_CTRL11_GEN2_RX_EQ,
		    SCFG_OFFSET + T_SATA0_CHX_PHY_CTRL11);
	sata_writel(tegra, T_SATA0_CHX_PHY_CTRL2_CDR_CNTL_GEN1,
		    SCFG_OFFSET + T_SATA0_CHX_PHY_CTRL2);

	sata_writel(tegra, 0, SCFG_OFFSET + T_SATA0_INDEX);

	/* Program controller device ID */

	val = sata_readl(tegra, SCFG_OFFSET + T_SATA0_CFG_SATA);
	val |= T_SATA0_CFG_SATA_BACKDOOR_PROG_IF_EN;
	sata_writel(tegra, val, SCFG_OFFSET + T_SATA0_CFG_SATA);

	sata_writel(tegra, 0x01060100, SCFG_OFFSET + T_SATA0_BKDOOR_CC);

	val = sata_readl(tegra, SCFG_OFFSET + T_SATA0_CFG_SATA);
	val &= ~T_SATA0_CFG_SATA_BACKDOOR_PROG_IF_EN;
	sata_writel(tegra, val, SCFG_OFFSET + T_SATA0_CFG_SATA);

	/* Enable IO & memory access, bus master mode */

	val = sata_readl(tegra, SCFG_OFFSET + T_SATA0_CFG_1);
	val |= T_SATA0_CFG_1_IO_SPACE | T_SATA0_CFG_1_MEMORY_SPACE |
		T_SATA0_CFG_1_BUS_MASTER | T_SATA0_CFG_1_SERR;
	sata_writel(tegra, val, SCFG_OFFSET + T_SATA0_CFG_1);

	/* Program SATA MMIO */

	sata_writel(tegra, 0x10000 << 4, SATA_FPCI_BAR5);
	sata_writel(tegra, 0x08000 << 13, SCFG_OFFSET + T_SATA0_CFG_9);

	/* Unmask SATA interrupts */

	val = sata_readl(tegra, SATA_INTR_MASK);
	val |= SATA_INTR_MASK_IP_INT_MASK;
	sata_writel(tegra, val, SATA_INTR_MASK);

	return 0;
}

static void tegra_ahci_controller_deinit(struct ahci_host_priv *hpriv)
{
	tegra_ahci_power_off(hpriv);
}

static void tegra_ahci_host_stop(struct ata_host *host)
{
	struct ahci_host_priv *hpriv = host->private_data;

	tegra_ahci_controller_deinit(hpriv);
}

static struct ata_port_operations ahci_tegra_port_ops = {
	.inherits	= &ahci_platform_ops,
	.host_stop	= tegra_ahci_host_stop,
};

static const struct ata_port_info ahci_tegra_port_info = {
	.flags		= AHCI_FLAG_COMMON,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_tegra_port_ops,
};

static const struct of_device_id tegra_ahci_of_match[] = {
	{ .compatible = "nvidia,tegra124-ahci" },
	{},
};
MODULE_DEVICE_TABLE(of, tegra_ahci_of_match);

static int tegra_ahci_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct ahci_host_priv *hpriv;
	struct tegra_ahci_priv *tegra;
	int ret;

	match = of_match_device(tegra_ahci_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	hpriv = ahci_platform_get_resources(pdev);
	if (IS_ERR(hpriv))
		return PTR_ERR(hpriv);

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	hpriv->plat_data = tegra;

	tegra->pdev = pdev;

	tegra->sata_regs = devm_ioremap_resource(&pdev->dev,
		platform_get_resource(pdev, IORESOURCE_MEM, 1));
	if (IS_ERR(tegra->sata_regs)) {
		dev_err(&pdev->dev, "Failed to get SATA IO memory");
		return PTR_ERR(tegra->sata_regs);
	}

	tegra->sata_rst = devm_reset_control_get(&pdev->dev, "sata");
	if (IS_ERR(tegra->sata_rst)) {
		dev_err(&pdev->dev, "Failed to get sata reset");
		return PTR_ERR(tegra->sata_rst);
	}

	tegra->sata_oob_rst = devm_reset_control_get(&pdev->dev, "sata-oob");
	if (IS_ERR(tegra->sata_oob_rst)) {
		dev_err(&pdev->dev, "Failed to get sata-oob reset");
		return PTR_ERR(tegra->sata_oob_rst);
	}

	tegra->sata_cold_rst = devm_reset_control_get(&pdev->dev, "sata-cold");
	if (IS_ERR(tegra->sata_cold_rst)) {
		dev_err(&pdev->dev, "Failed to get sata-cold reset");
		return PTR_ERR(tegra->sata_cold_rst);
	}

	tegra->supplies[0].supply = "avdd";
	tegra->supplies[1].supply = "hvdd";
	tegra->supplies[2].supply = "vddio";

	ret = devm_regulator_bulk_get(&pdev->dev, ARRAY_SIZE(tegra->supplies),
					tegra->supplies);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get regulators");
		return ret;
	}

	ret = tegra_ahci_controller_init(hpriv);
	if (ret)
		return ret;

	ret = ahci_platform_init_host(pdev, hpriv, &ahci_tegra_port_info,
				      0, 0, 0);
	if (ret) {
		tegra_ahci_controller_deinit(hpriv);
		return ret;
	}

	return 0;
};

static struct platform_driver tegra_ahci_driver = {
	.probe = tegra_ahci_probe,
	.remove = ata_platform_remove_one,
	.driver = {
		.name = "tegra-ahci",
		.owner = THIS_MODULE,
		.of_match_table = tegra_ahci_of_match,
	},
};
module_platform_driver(tegra_ahci_driver);

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_DESCRIPTION("Tegra124 AHCI SATA driver");
MODULE_LICENSE("GPL v2");
