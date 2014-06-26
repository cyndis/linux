/*
 * Copyright (C) 2014 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <soc/tegra/fuse.h>

#include <dt-bindings/memory/tegra124-mc.h>

#include <asm/cacheflush.h>
#ifndef CONFIG_ARM64
#include <asm/dma-iommu.h>
#endif

#define MC_INTSTATUS 0x000
#define  MC_INT_DECERR_MTS (1 << 16)
#define  MC_INT_SECERR_SEC (1 << 13)
#define  MC_INT_DECERR_VPR (1 << 12)
#define  MC_INT_INVALID_APB_ASID_UPDATE (1 << 11)
#define  MC_INT_INVALID_SMMU_PAGE (1 << 10)
#define  MC_INT_ARBITRATION_EMEM (1 << 9)
#define  MC_INT_SECURITY_VIOLATION (1 << 8)
#define  MC_INT_DECERR_EMEM (1 << 6)
#define MC_INTMASK 0x004
#define MC_ERR_STATUS 0x08
#define MC_ERR_ADR 0x0c

#define MC_EMEM_ADR_CFG 0x54
#define MC_EMEM_ADR_CFG_EMEM_NUMDEV (1 << 0)

#define PMC_STRAPPING_OPT_A_RAM_CODE_MASK (0xf << 4)
#define PMC_STRAPPING_OPT_A_RAM_CODE_SHIFT 4

struct latency_allowance {
	unsigned int reg;
	unsigned int shift;
	unsigned int mask;
	unsigned int def;
};

struct smmu_enable {
	unsigned int reg;
	unsigned int bit;
};

struct tegra_mc_client {
	unsigned int id;
	const char *name;
	unsigned int swgroup;

	struct smmu_enable smmu;
	struct latency_allowance latency;
};

static const struct tegra_mc_client tegra124_mc_clients[] = {
	{
		.id = 0x01,
		.name = "display0a",
		.swgroup = TEGRA_SWGROUP_DC,
		.smmu = {
			.reg = 0x228,
			.bit = 1,
		},
		.latency = {
			.reg = 0x2e8,
			.shift = 0,
			.mask = 0xff,
			.def = 0xc2,
		},
	}, {
		.id = 0x02,
		.name = "display0ab",
		.swgroup = TEGRA_SWGROUP_DCB,
		.smmu = {
			.reg = 0x228,
			.bit = 2,
		},
		.latency = {
			.reg = 0x2f4,
			.shift = 0,
			.mask = 0xff,
			.def = 0xc6,
		},
	}, {
		.id = 0x03,
		.name = "display0b",
		.swgroup = TEGRA_SWGROUP_DC,
		.smmu = {
			.reg = 0x228,
			.bit = 3,
		},
		.latency = {
			.reg = 0x2e8,
			.shift = 16,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x04,
		.name = "display0bb",
		.swgroup = TEGRA_SWGROUP_DCB,
		.smmu = {
			.reg = 0x228,
			.bit = 4,
		},
		.latency = {
			.reg = 0x2f4,
			.shift = 16,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x05,
		.name = "display0c",
		.swgroup = TEGRA_SWGROUP_DC,
		.smmu = {
			.reg = 0x228,
			.bit = 5,
		},
		.latency = {
			.reg = 0x2ec,
			.shift = 0,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x06,
		.name = "display0cb",
		.swgroup = TEGRA_SWGROUP_DCB,
		.smmu = {
			.reg = 0x228,
			.bit = 6,
		},
		.latency = {
			.reg = 0x2f8,
			.shift = 0,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x0e,
		.name = "afir",
		.swgroup = TEGRA_SWGROUP_AFI,
		.smmu = {
			.reg = 0x228,
			.bit = 14,
		},
		.latency = {
			.reg = 0x2e0,
			.shift = 0,
			.mask = 0xff,
			.def = 0x13,
		},
	}, {
		.id = 0x0f,
		.name = "avpcarm7r",
		.swgroup = TEGRA_SWGROUP_AVPC,
		.smmu = {
			.reg = 0x228,
			.bit = 15,
		},
		.latency = {
			.reg = 0x2e4,
			.shift = 0,
			.mask = 0xff,
			.def = 0x04,
		},
	}, {
		.id = 0x10,
		.name = "displayhc",
		.swgroup = TEGRA_SWGROUP_DC,
		.smmu = {
			.reg = 0x228,
			.bit = 16,
		},
		.latency = {
			.reg = 0x2f0,
			.shift = 0,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x11,
		.name = "displayhcb",
		.swgroup = TEGRA_SWGROUP_DCB,
		.smmu = {
			.reg = 0x228,
			.bit = 17,
		},
		.latency = {
			.reg = 0x2fc,
			.shift = 0,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x15,
		.name = "hdar",
		.swgroup = TEGRA_SWGROUP_HDA,
		.smmu = {
			.reg = 0x228,
			.bit = 21,
		},
		.latency = {
			.reg = 0x318,
			.shift = 0,
			.mask = 0xff,
			.def = 0x24,
		},
	}, {
		.id = 0x16,
		.name = "host1xdmar",
		.swgroup = TEGRA_SWGROUP_HC,
		.smmu = {
			.reg = 0x228,
			.bit = 22,
		},
		.latency = {
			.reg = 0x310,
			.shift = 0,
			.mask = 0xff,
			.def = 0x1e,
		},
	}, {
		.id = 0x17,
		.name = "host1xr",
		.swgroup = TEGRA_SWGROUP_HC,
		.smmu = {
			.reg = 0x228,
			.bit = 23,
		},
		.latency = {
			.reg = 0x310,
			.shift = 16,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x1c,
		.name = "msencsrd",
		.swgroup = TEGRA_SWGROUP_MSENC,
		.smmu = {
			.reg = 0x228,
			.bit = 28,
		},
		.latency = {
			.reg = 0x328,
			.shift = 0,
			.mask = 0xff,
			.def = 0x23,
		},
	}, {
		.id = 0x1d,
		.name = "ppcsahbdmarhdar",
		.swgroup = TEGRA_SWGROUP_PPCS,
		.smmu = {
			.reg = 0x228,
			.bit = 29,
		},
		.latency = {
			.reg = 0x344,
			.shift = 0,
			.mask = 0xff,
			.def = 0x49,
		},
	}, {
		.id = 0x1e,
		.name = "ppcsahbslvr",
		.swgroup = TEGRA_SWGROUP_PPCS,
		.smmu = {
			.reg = 0x228,
			.bit = 30,
		},
		.latency = {
			.reg = 0x344,
			.shift = 16,
			.mask = 0xff,
			.def = 0x1a,
		},
	}, {
		.id = 0x1f,
		.name = "satar",
		.swgroup = TEGRA_SWGROUP_SATA,
		.smmu = {
			.reg = 0x228,
			.bit = 31,
		},
		.latency = {
			.reg = 0x350,
			.shift = 0,
			.mask = 0xff,
			.def = 0x65,
		},
	}, {
		.id = 0x22,
		.name = "vdebsevr",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x22c,
			.bit = 2,
		},
		.latency = {
			.reg = 0x354,
			.shift = 0,
			.mask = 0xff,
			.def = 0x4f,
		},
	}, {
		.id = 0x23,
		.name = "vdember",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x22c,
			.bit = 3,
		},
		.latency = {
			.reg = 0x354,
			.shift = 16,
			.mask = 0xff,
			.def = 0x3d,
		},
	}, {
		.id = 0x24,
		.name = "vdemcer",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x22c,
			.bit = 4,
		},
		.latency = {
			.reg = 0x358,
			.shift = 0,
			.mask = 0xff,
			.def = 0x66,
		},
	}, {
		.id = 0x25,
		.name = "vdetper",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x22c,
			.bit = 5,
		},
		.latency = {
			.reg = 0x358,
			.shift = 16,
			.mask = 0xff,
			.def = 0xa5,
		},
	}, {
		.id = 0x26,
		.name = "mpcorelpr",
		.swgroup = TEGRA_SWGROUP_MPCORELP,
		.latency = {
			.reg = 0x324,
			.shift = 0,
			.mask = 0xff,
			.def = 0x04,
		},
	}, {
		.id = 0x27,
		.name = "mpcorer",
		.swgroup = TEGRA_SWGROUP_MPCORE,
		.smmu = {
			.reg = 0x22c,
			.bit = 2,
		},
		.latency = {
			.reg = 0x320,
			.shift = 0,
			.mask = 0xff,
			.def = 0x04,
		},
	}, {
		.id = 0x2b,
		.name = "msencswr",
		.swgroup = TEGRA_SWGROUP_MSENC,
		.smmu = {
			.reg = 0x22c,
			.bit = 11,
		},
		.latency = {
			.reg = 0x328,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x31,
		.name = "afiw",
		.swgroup = TEGRA_SWGROUP_AFI,
		.smmu = {
			.reg = 0x22c,
			.bit = 17,
		},
		.latency = {
			.reg = 0x2e0,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x32,
		.name = "avpcarm7w",
		.swgroup = TEGRA_SWGROUP_AVPC,
		.smmu = {
			.reg = 0x22c,
			.bit = 18,
		},
		.latency = {
			.reg = 0x2e4,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x35,
		.name = "hdaw",
		.swgroup = TEGRA_SWGROUP_HDA,
		.smmu = {
			.reg = 0x22c,
			.bit = 21,
		},
		.latency = {
			.reg = 0x318,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x36,
		.name = "host1xw",
		.swgroup = TEGRA_SWGROUP_HC,
		.smmu = {
			.reg = 0x22c,
			.bit = 22,
		},
		.latency = {
			.reg = 0x314,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x38,
		.name = "mpcorelpw",
		.swgroup = TEGRA_SWGROUP_MPCORELP,
		.latency = {
			.reg = 0x324,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x39,
		.name = "mpcorew",
		.swgroup = TEGRA_SWGROUP_MPCORE,
		.latency = {
			.reg = 0x320,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x3b,
		.name = "ppcsahbdmaw",
		.swgroup = TEGRA_SWGROUP_PPCS,
		.smmu = {
			.reg = 0x22c,
			.bit = 27,
		},
		.latency = {
			.reg = 0x348,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x3c,
		.name = "ppcsahbslvw",
		.swgroup = TEGRA_SWGROUP_PPCS,
		.smmu = {
			.reg = 0x22c,
			.bit = 28,
		},
		.latency = {
			.reg = 0x348,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x3d,
		.name = "sataw",
		.swgroup = TEGRA_SWGROUP_SATA,
		.smmu = {
			.reg = 0x22c,
			.bit = 29,
		},
		.latency = {
			.reg = 0x350,
			.shift = 16,
			.mask = 0xff,
			.def = 0x65,
		},
	}, {
		.id = 0x3e,
		.name = "vdebsevw",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x22c,
			.bit = 30,
		},
		.latency = {
			.reg = 0x35c,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x3f,
		.name = "vdedbgw",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x22c,
			.bit = 31,
		},
		.latency = {
			.reg = 0x35c,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x40,
		.name = "vdembew",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x230,
			.bit = 0,
		},
		.latency = {
			.reg = 0x360,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x41,
		.name = "vdetpmw",
		.swgroup = TEGRA_SWGROUP_VDE,
		.smmu = {
			.reg = 0x230,
			.bit = 1,
		},
		.latency = {
			.reg = 0x360,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x44,
		.name = "ispra",
		.swgroup = TEGRA_SWGROUP_ISP2,
		.smmu = {
			.reg = 0x230,
			.bit = 4,
		},
		.latency = {
			.reg = 0x370,
			.shift = 0,
			.mask = 0xff,
			.def = 0x18,
		},
	}, {
		.id = 0x46,
		.name = "ispwa",
		.swgroup = TEGRA_SWGROUP_ISP2,
		.smmu = {
			.reg = 0x230,
			.bit = 6,
		},
		.latency = {
			.reg = 0x374,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x47,
		.name = "ispwb",
		.swgroup = TEGRA_SWGROUP_ISP2,
		.smmu = {
			.reg = 0x230,
			.bit = 7,
		},
		.latency = {
			.reg = 0x374,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x4a,
		.name = "xusb_hostr",
		.swgroup = TEGRA_SWGROUP_XUSB_HOST,
		.smmu = {
			.reg = 0x230,
			.bit = 10,
		},
		.latency = {
			.reg = 0x37c,
			.shift = 0,
			.mask = 0xff,
			.def = 0x39,
		},
	}, {
		.id = 0x4b,
		.name = "xusb_hostw",
		.swgroup = TEGRA_SWGROUP_XUSB_HOST,
		.smmu = {
			.reg = 0x230,
			.bit = 11,
		},
		.latency = {
			.reg = 0x37c,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x4c,
		.name = "xusb_devr",
		.swgroup = TEGRA_SWGROUP_XUSB_DEV,
		.smmu = {
			.reg = 0x230,
			.bit = 12,
		},
		.latency = {
			.reg = 0x380,
			.shift = 0,
			.mask = 0xff,
			.def = 0x39,
		},
	}, {
		.id = 0x4d,
		.name = "xusb_devw",
		.swgroup = TEGRA_SWGROUP_XUSB_DEV,
		.smmu = {
			.reg = 0x230,
			.bit = 13,
		},
		.latency = {
			.reg = 0x380,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x4e,
		.name = "isprab",
		.swgroup = TEGRA_SWGROUP_ISP2B,
		.smmu = {
			.reg = 0x230,
			.bit = 14,
		},
		.latency = {
			.reg = 0x384,
			.shift = 0,
			.mask = 0xff,
			.def = 0x18,
		},
	}, {
		.id = 0x50,
		.name = "ispwab",
		.swgroup = TEGRA_SWGROUP_ISP2B,
		.smmu = {
			.reg = 0x230,
			.bit = 16,
		},
		.latency = {
			.reg = 0x388,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x51,
		.name = "ispwbb",
		.swgroup = TEGRA_SWGROUP_ISP2B,
		.smmu = {
			.reg = 0x230,
			.bit = 17,
		},
		.latency = {
			.reg = 0x388,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x54,
		.name = "tsecsrd",
		.swgroup = TEGRA_SWGROUP_TSEC,
		.smmu = {
			.reg = 0x230,
			.bit = 20,
		},
		.latency = {
			.reg = 0x390,
			.shift = 0,
			.mask = 0xff,
			.def = 0x9b,
		},
	}, {
		.id = 0x55,
		.name = "tsecswr",
		.swgroup = TEGRA_SWGROUP_TSEC,
		.smmu = {
			.reg = 0x230,
			.bit = 21,
		},
		.latency = {
			.reg = 0x390,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x56,
		.name = "a9avpscr",
		.swgroup = TEGRA_SWGROUP_A9AVP,
		.smmu = {
			.reg = 0x230,
			.bit = 22,
		},
		.latency = {
			.reg = 0x3a4,
			.shift = 0,
			.mask = 0xff,
			.def = 0x04,
		},
	}, {
		.id = 0x57,
		.name = "a9avpscw",
		.swgroup = TEGRA_SWGROUP_A9AVP,
		.smmu = {
			.reg = 0x230,
			.bit = 23,
		},
		.latency = {
			.reg = 0x3a4,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x58,
		.name = "gpusrd",
		.swgroup = TEGRA_SWGROUP_GPU,
		.smmu = {
			/* read-only */
			.reg = 0x230,
			.bit = 24,
		},
		.latency = {
			.reg = 0x3c8,
			.shift = 0,
			.mask = 0xff,
			.def = 0x1a,
		},
	}, {
		.id = 0x59,
		.name = "gpuswr",
		.swgroup = TEGRA_SWGROUP_GPU,
		.smmu = {
			/* read-only */
			.reg = 0x230,
			.bit = 25,
		},
		.latency = {
			.reg = 0x3c8,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x5a,
		.name = "displayt",
		.swgroup = TEGRA_SWGROUP_DC,
		.smmu = {
			.reg = 0x230,
			.bit = 26,
		},
		.latency = {
			.reg = 0x2f0,
			.shift = 16,
			.mask = 0xff,
			.def = 0x50,
		},
	}, {
		.id = 0x60,
		.name = "sdmmcra",
		.swgroup = TEGRA_SWGROUP_SDMMC1A,
		.smmu = {
			.reg = 0x234,
			.bit = 0,
		},
		.latency = {
			.reg = 0x3b8,
			.shift = 0,
			.mask = 0xff,
			.def = 0x49,
		},
	}, {
		.id = 0x61,
		.name = "sdmmcraa",
		.swgroup = TEGRA_SWGROUP_SDMMC2A,
		.smmu = {
			.reg = 0x234,
			.bit = 1,
		},
		.latency = {
			.reg = 0x3bc,
			.shift = 0,
			.mask = 0xff,
			.def = 0x49,
		},
	}, {
		.id = 0x62,
		.name = "sdmmcr",
		.swgroup = TEGRA_SWGROUP_SDMMC3A,
		.smmu = {
			.reg = 0x234,
			.bit = 2,
		},
		.latency = {
			.reg = 0x3c0,
			.shift = 0,
			.mask = 0xff,
			.def = 0x49,
		},
	}, {
		.id = 0x63,
		.swgroup = TEGRA_SWGROUP_SDMMC4A,
		.name = "sdmmcrab",
		.smmu = {
			.reg = 0x234,
			.bit = 3,
		},
		.latency = {
			.reg = 0x3c4,
			.shift = 0,
			.mask = 0xff,
			.def = 0x49,
		},
	}, {
		.id = 0x64,
		.name = "sdmmcwa",
		.swgroup = TEGRA_SWGROUP_SDMMC1A,
		.smmu = {
			.reg = 0x234,
			.bit = 4,
		},
		.latency = {
			.reg = 0x3b8,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x65,
		.name = "sdmmcwaa",
		.swgroup = TEGRA_SWGROUP_SDMMC2A,
		.smmu = {
			.reg = 0x234,
			.bit = 5,
		},
		.latency = {
			.reg = 0x3bc,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x66,
		.name = "sdmmcw",
		.swgroup = TEGRA_SWGROUP_SDMMC3A,
		.smmu = {
			.reg = 0x234,
			.bit = 6,
		},
		.latency = {
			.reg = 0x3c0,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x67,
		.name = "sdmmcwab",
		.swgroup = TEGRA_SWGROUP_SDMMC4A,
		.smmu = {
			.reg = 0x234,
			.bit = 7,
		},
		.latency = {
			.reg = 0x3c4,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x6c,
		.name = "vicsrd",
		.swgroup = TEGRA_SWGROUP_VIC,
		.smmu = {
			.reg = 0x234,
			.bit = 12,
		},
		.latency = {
			.reg = 0x394,
			.shift = 0,
			.mask = 0xff,
			.def = 0x1a,
		},
	}, {
		.id = 0x6d,
		.name = "vicswr",
		.swgroup = TEGRA_SWGROUP_VIC,
		.smmu = {
			.reg = 0x234,
			.bit = 13,
		},
		.latency = {
			.reg = 0x394,
			.shift = 16,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x72,
		.name = "viw",
		.swgroup = TEGRA_SWGROUP_VI,
		.smmu = {
			.reg = 0x234,
			.bit = 18,
		},
		.latency = {
			.reg = 0x398,
			.shift = 0,
			.mask = 0xff,
			.def = 0x80,
		},
	}, {
		.id = 0x73,
		.name = "displayd",
		.swgroup = TEGRA_SWGROUP_DC,
		.smmu = {
			.reg = 0x234,
			.bit = 19,
		},
		.latency = {
			.reg = 0x3c8,
			.shift = 0,
			.mask = 0xff,
			.def = 0x50,
		},
	},
};

struct tegra_smmu_swgroup {
	unsigned int swgroup;
	unsigned int reg;
};

static const struct tegra_smmu_swgroup tegra124_swgroups[] = {
	{ .swgroup = TEGRA_SWGROUP_DC,        .reg = 0x240 },
	{ .swgroup = TEGRA_SWGROUP_DCB,       .reg = 0x244 },
	{ .swgroup = TEGRA_SWGROUP_AFI,       .reg = 0x238 },
	{ .swgroup = TEGRA_SWGROUP_AVPC,      .reg = 0x23c },
	{ .swgroup = TEGRA_SWGROUP_HDA,       .reg = 0x254 },
	{ .swgroup = TEGRA_SWGROUP_HC,        .reg = 0x250 },
	{ .swgroup = TEGRA_SWGROUP_MSENC,     .reg = 0x264 },
	{ .swgroup = TEGRA_SWGROUP_PPCS,      .reg = 0x270 },
	{ .swgroup = TEGRA_SWGROUP_SATA,      .reg = 0x274 },
	{ .swgroup = TEGRA_SWGROUP_VDE,       .reg = 0x27c },
	{ .swgroup = TEGRA_SWGROUP_ISP2,      .reg = 0x258 },
	{ .swgroup = TEGRA_SWGROUP_XUSB_HOST, .reg = 0x288 },
	{ .swgroup = TEGRA_SWGROUP_XUSB_DEV,  .reg = 0x28c },
	{ .swgroup = TEGRA_SWGROUP_ISP2B,     .reg = 0xaa4 },
	{ .swgroup = TEGRA_SWGROUP_TSEC,      .reg = 0x294 },
	{ .swgroup = TEGRA_SWGROUP_A9AVP,     .reg = 0x290 },
	{ .swgroup = TEGRA_SWGROUP_GPU,       .reg = 0xaa8 },
	{ .swgroup = TEGRA_SWGROUP_SDMMC1A,   .reg = 0xa94 },
	{ .swgroup = TEGRA_SWGROUP_SDMMC2A,   .reg = 0xa98 },
	{ .swgroup = TEGRA_SWGROUP_SDMMC3A,   .reg = 0xa9c },
	{ .swgroup = TEGRA_SWGROUP_SDMMC4A,   .reg = 0xaa0 },
	{ .swgroup = TEGRA_SWGROUP_VIC,       .reg = 0x284 },
	{ .swgroup = TEGRA_SWGROUP_VI,        .reg = 0x280 },
};

struct tegra_smmu_group_init {
	unsigned int asid;
	const char *name;

	const struct of_device_id *matches;
};

struct tegra_smmu_soc {
	const struct tegra_smmu_group_init *groups;
	unsigned int num_groups;

	const struct tegra_mc_client *clients;
	unsigned int num_clients;

	const struct tegra_smmu_swgroup *swgroups;
	unsigned int num_swgroups;

	unsigned int num_asids;
	unsigned int atom_size;

	const struct tegra_smmu_ops *ops;
};

struct tegra_smmu_ops {
	void (*flush_dcache)(struct page *page, unsigned long offset,
			     size_t size);
};

struct tegra_smmu_master {
	struct list_head list;
	struct device *dev;
};

struct tegra_smmu_group {
	const char *name;
	const struct of_device_id *matches;
	unsigned int asid;

#ifndef CONFIG_ARM64
	struct dma_iommu_mapping *mapping;
#endif
	struct list_head masters;
};

static const struct of_device_id tegra124_periph_matches[] = {
	{ .compatible = "nvidia,tegra124-sdhci", },
	{ }
};

static const struct tegra_smmu_group_init tegra124_smmu_groups[] = {
	{ 0, "peripherals", tegra124_periph_matches },
};

static void tegra_smmu_group_release(void *data)
{
	kfree(data);
}

struct tegra_smmu {
	void __iomem *regs;
	struct iommu iommu;
	struct device *dev;

	const struct tegra_smmu_soc *soc;

	struct iommu_group **groups;
	unsigned int num_groups;

	unsigned long *asids;
	struct mutex lock;
};

struct tegra_smmu_address_space {
	struct iommu_domain *domain;
	struct tegra_smmu *smmu;
	struct page *pd;
	unsigned id;
	u32 attr;
};

static inline void smmu_writel(struct tegra_smmu *smmu, u32 value,
			       unsigned long offset)
{
	writel(value, smmu->regs + offset);
}

static inline u32 smmu_readl(struct tegra_smmu *smmu, unsigned long offset)
{
	return readl(smmu->regs + offset);
}

#define SMMU_CONFIG 0x010
#define  SMMU_CONFIG_ENABLE (1 << 0)

#define SMMU_PTB_ASID 0x01c
#define  SMMU_PTB_ASID_VALUE(x) ((x) & 0x7f)

#define SMMU_PTB_DATA 0x020
#define  SMMU_PTB_DATA_VALUE(page, attr) (page_to_phys(page) >> 12 | (attr))

#define SMMU_MK_PDE(page, attr) (page_to_phys(page) >> SMMU_PTE_SHIFT | (attr))

#define SMMU_TLB_FLUSH 0x030
#define  SMMU_TLB_FLUSH_VA_MATCH_ALL     (0 << 0)
#define  SMMU_TLB_FLUSH_VA_MATCH_SECTION (2 << 0)
#define  SMMU_TLB_FLUSH_VA_MATCH_GROUP   (3 << 0)
#define  SMMU_TLB_FLUSH_ASID(x)          (((x) & 0x7f) << 24)
#define  SMMU_TLB_FLUSH_VA_SECTION(addr) ((((addr) & 0xffc00000) >> 12) | \
					  SMMU_TLB_FLUSH_VA_MATCH_SECTION)
#define  SMMU_TLB_FLUSH_VA_GROUP(addr)   ((((addr) & 0xffffc000) >> 12) | \
					  SMMU_TLB_FLUSH_VA_MATCH_GROUP)
#define  SMMU_TLB_FLUSH_ASID_MATCH       (1 << 31)

#define SMMU_PTC_FLUSH 0x034
#define  SMMU_PTC_FLUSH_TYPE_ALL (0 << 0)
#define  SMMU_PTC_FLUSH_TYPE_ADR (1 << 0)

#define SMMU_PTC_FLUSH_HI 0x9b8
#define  SMMU_PTC_FLUSH_HI_MASK 0x3

/* per-SWGROUP SMMU_*_ASID register */
#define SMMU_ASID_ENABLE (1 << 31)
#define SMMU_ASID_MASK 0x7f
#define SMMU_ASID_VALUE(x) ((x) & SMMU_ASID_MASK)

/* page table definitions */
#define SMMU_NUM_PDE 1024
#define SMMU_NUM_PTE 1024

#define SMMU_SIZE_PD (SMMU_NUM_PDE * 4)
#define SMMU_SIZE_PT (SMMU_NUM_PTE * 4)

#define SMMU_PDE_SHIFT 22
#define SMMU_PTE_SHIFT 12

#define SMMU_PFN_MASK 0x000fffff

#define SMMU_PD_READABLE	(1 << 31)
#define SMMU_PD_WRITABLE	(1 << 30)
#define SMMU_PD_NONSECURE	(1 << 29)

#define SMMU_PDE_READABLE	(1 << 31)
#define SMMU_PDE_WRITABLE	(1 << 30)
#define SMMU_PDE_NONSECURE	(1 << 29)
#define SMMU_PDE_NEXT		(1 << 28)

#define SMMU_PTE_READABLE	(1 << 31)
#define SMMU_PTE_WRITABLE	(1 << 30)
#define SMMU_PTE_NONSECURE	(1 << 29)

#define SMMU_PDE_ATTR		(SMMU_PDE_READABLE | SMMU_PDE_WRITABLE | \
				 SMMU_PDE_NONSECURE)
#define SMMU_PTE_ATTR		(SMMU_PTE_READABLE | SMMU_PTE_WRITABLE | \
				 SMMU_PTE_NONSECURE)

#define SMMU_PDE_VACANT(n)	(((n) << 10) | SMMU_PDE_ATTR)
#define SMMU_PTE_VACANT(n)	(((n) << 12) | SMMU_PTE_ATTR)

#ifdef CONFIG_ARCH_TEGRA_124_SOC
static void tegra124_flush_dcache(struct page *page, unsigned long offset,
				  size_t size)
{
	phys_addr_t phys = page_to_phys(page) + offset;
	void *virt = page_address(page) + offset;

	__cpuc_flush_dcache_area(virt, size);
	outer_flush_range(phys, phys + size);
}

static const struct tegra_smmu_ops tegra124_smmu_ops = {
	.flush_dcache = tegra124_flush_dcache,
};
#endif

static void tegra132_flush_dcache(struct page *page, unsigned long offset,
				  size_t size)
{
	/* TODO: implement */
}

static const struct tegra_smmu_ops tegra132_smmu_ops = {
	.flush_dcache = tegra132_flush_dcache,
};

static inline void smmu_flush_ptc(struct tegra_smmu *smmu, struct page *page,
				  unsigned long offset)
{
	phys_addr_t phys = page ? page_to_phys(page) : 0;
	u32 value;

	if (page) {
		offset &= ~(smmu->soc->atom_size - 1);

#ifdef CONFIG_PHYS_ADDR_T_64BIT
		value = (phys >> 32) & SMMU_PTC_FLUSH_HI_MASK;
#else
		value = 0;
#endif
		smmu_writel(smmu, value, SMMU_PTC_FLUSH_HI);

		value = (phys + offset) | SMMU_PTC_FLUSH_TYPE_ADR;
	} else {
		value = SMMU_PTC_FLUSH_TYPE_ALL;
	}

	smmu_writel(smmu, value, SMMU_PTC_FLUSH);
}

static inline void smmu_flush_tlb(struct tegra_smmu *smmu)
{
	smmu_writel(smmu, SMMU_TLB_FLUSH_VA_MATCH_ALL, SMMU_TLB_FLUSH);
}

static inline void smmu_flush_tlb_asid(struct tegra_smmu *smmu,
				       unsigned long asid)
{
	u32 value;

	value = SMMU_TLB_FLUSH_ASID_MATCH | SMMU_TLB_FLUSH_ASID(asid) |
		SMMU_TLB_FLUSH_VA_MATCH_ALL;
	smmu_writel(smmu, value, SMMU_TLB_FLUSH);
}

static inline void smmu_flush_tlb_section(struct tegra_smmu *smmu,
					  unsigned long asid,
					  unsigned long iova)
{
	u32 value;

	value = SMMU_TLB_FLUSH_ASID_MATCH | SMMU_TLB_FLUSH_ASID(asid) |
		SMMU_TLB_FLUSH_VA_SECTION(iova);
	smmu_writel(smmu, value, SMMU_TLB_FLUSH);
}

static inline void smmu_flush_tlb_group(struct tegra_smmu *smmu,
					unsigned long asid,
					unsigned long iova)
{
	u32 value;

	value = SMMU_TLB_FLUSH_ASID_MATCH | SMMU_TLB_FLUSH_ASID(asid) |
		SMMU_TLB_FLUSH_VA_GROUP(iova);
	smmu_writel(smmu, value, SMMU_TLB_FLUSH);
}

static inline void smmu_flush(struct tegra_smmu *smmu)
{
	smmu_readl(smmu, SMMU_CONFIG);
}

static inline struct tegra_smmu *to_tegra_smmu(struct iommu *iommu)
{
	return container_of(iommu, struct tegra_smmu, iommu);
}

static struct tegra_smmu *smmu_handle = NULL;

static int tegra_smmu_alloc_asid(struct tegra_smmu *smmu, unsigned int *idp)
{
	unsigned long id;

	mutex_lock(&smmu->lock);

	id = find_first_zero_bit(smmu->asids, smmu->soc->num_asids);
	if (id >= smmu->soc->num_asids) {
		mutex_unlock(&smmu->lock);
		return -ENOSPC;
	}

	set_bit(id, smmu->asids);
	*idp = id;

	mutex_unlock(&smmu->lock);
	return 0;
}

static void tegra_smmu_free_asid(struct tegra_smmu *smmu, unsigned int id)
{
	mutex_lock(&smmu->lock);
	clear_bit(id, smmu->asids);
	mutex_unlock(&smmu->lock);
}

struct tegra_smmu_address_space *foo = NULL;

static int tegra_smmu_domain_init(struct iommu_domain *domain)
{
	struct tegra_smmu *smmu = smmu_handle;
	struct tegra_smmu_address_space *as;
	uint32_t *pd, value;
	unsigned int i;
	int err = 0;

	as = kzalloc(sizeof(*as), GFP_KERNEL);
	if (!as) {
		err = -ENOMEM;
		goto out;
	}

	as->attr = SMMU_PD_READABLE | SMMU_PD_WRITABLE | SMMU_PD_NONSECURE;
	as->smmu = smmu_handle;
	as->domain = domain;

	err = tegra_smmu_alloc_asid(smmu, &as->id);
	if (err < 0) {
		kfree(as);
		goto out;
	}

	as->pd = alloc_page(GFP_KERNEL | __GFP_DMA);
	if (!as->pd) {
		err = -ENOMEM;
		goto out;
	}

	pd = page_address(as->pd);
	SetPageReserved(as->pd);

	for (i = 0; i < SMMU_NUM_PDE; i++)
		pd[i] = SMMU_PDE_VACANT(i);

	smmu->soc->ops->flush_dcache(as->pd, 0, SMMU_SIZE_PD);
	smmu_flush_ptc(smmu, as->pd, 0);
	smmu_flush_tlb_asid(smmu, as->id);

	smmu_writel(smmu, as->id & 0x7f, SMMU_PTB_ASID);
	value = SMMU_PTB_DATA_VALUE(as->pd, as->attr);
	smmu_writel(smmu, value, SMMU_PTB_DATA);
	smmu_flush(smmu);

	domain->priv = as;

	return 0;

out:
	return err;
}

static void tegra_smmu_domain_destroy(struct iommu_domain *domain)
{
	struct tegra_smmu_address_space *as = domain->priv;

	/* TODO: free page directory and page tables */

	tegra_smmu_free_asid(as->smmu, as->id);
	kfree(as);
}

static const struct tegra_smmu_swgroup *
tegra_smmu_find_swgroup(struct tegra_smmu *smmu, unsigned int swgroup)
{
	const struct tegra_smmu_swgroup *group = NULL;
	unsigned int i;

	for (i = 0; i < smmu->soc->num_swgroups; i++) {
		if (smmu->soc->swgroups[i].swgroup == swgroup) {
			group = &smmu->soc->swgroups[i];
			break;
		}
	}

	return group;
}

static int tegra_smmu_enable(struct tegra_smmu *smmu, unsigned int swgroup,
			     unsigned int asid)
{
	const struct tegra_smmu_swgroup *group;
	unsigned int i;
	u32 value;

	for (i = 0; i < smmu->soc->num_clients; i++) {
		const struct tegra_mc_client *client = &smmu->soc->clients[i];

		if (client->swgroup != swgroup)
			continue;

		value = smmu_readl(smmu, client->smmu.reg);
		value |= BIT(client->smmu.bit);
		smmu_writel(smmu, value, client->smmu.reg);
	}

	group = tegra_smmu_find_swgroup(smmu, swgroup);
	if (group) {
		value = smmu_readl(smmu, group->reg);
		value &= ~SMMU_ASID_MASK;
		value |= SMMU_ASID_VALUE(asid);
		value |= SMMU_ASID_ENABLE;
		smmu_writel(smmu, value, group->reg);
	}

	return 0;
}

static int tegra_smmu_disable(struct tegra_smmu *smmu, unsigned int swgroup,
			      unsigned int asid)
{
	const struct tegra_smmu_swgroup *group;
	unsigned int i;
	u32 value;

	group = tegra_smmu_find_swgroup(smmu, swgroup);
	if (group) {
		value = smmu_readl(smmu, group->reg);
		value &= ~SMMU_ASID_MASK;
		value |= SMMU_ASID_VALUE(asid);
		value &= ~SMMU_ASID_ENABLE;
		smmu_writel(smmu, value, group->reg);
	}

	for (i = 0; i < smmu->soc->num_clients; i++) {
		const struct tegra_mc_client *client = &smmu->soc->clients[i];

		if (client->swgroup != swgroup)
			continue;

		value = smmu_readl(smmu, client->smmu.reg);
		value &= ~BIT(client->smmu.bit);
		smmu_writel(smmu, value, client->smmu.reg);
	}

	return 0;
}

static int tegra_smmu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct tegra_smmu_address_space *as = domain->priv;
	struct tegra_smmu *smmu = as->smmu;
	struct of_phandle_iter entry;
	int err;

	of_property_for_each_phandle_with_args(entry, dev->of_node, "iommus",
					       "#iommu-cells", 0) {
		unsigned int swgroup = entry.out_args.args[0];

		if (entry.out_args.np != smmu->dev->of_node)
			continue;

		err = tegra_smmu_enable(smmu, swgroup, as->id);
		if (err < 0)
			pr_err("failed to enable SWGROUP#%u\n", swgroup);
	}

	return 0;
}

static void tegra_smmu_detach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct tegra_smmu_address_space *as = domain->priv;
	struct tegra_smmu *smmu = as->smmu;
	struct of_phandle_iter entry;
	int err;

	of_property_for_each_phandle_with_args(entry, dev->of_node, "iommus",
					       "#iommu-cells", 0) {
		unsigned int swgroup;

		if (entry.out_args.np != smmu->dev->of_node)
			continue;

		swgroup = entry.out_args.args[0];

		err = tegra_smmu_disable(smmu, swgroup, as->id);
		if (err < 0) {
			pr_err("failed to enable SWGROUP#%u\n", swgroup);
		}
	}
}

static u32 *as_get_pte(struct tegra_smmu_address_space *as, dma_addr_t iova,
		       struct page **pagep)
{
	struct tegra_smmu *smmu = smmu_handle;
	u32 *pd = page_address(as->pd), *pt;
	u32 pde = (iova >> SMMU_PDE_SHIFT) & 0x3ff;
	u32 pte = (iova >> SMMU_PTE_SHIFT) & 0x3ff;
	struct page *page;
	unsigned int i;

	if (pd[pde] != SMMU_PDE_VACANT(pde)) {
		page = pfn_to_page(pd[pde] & SMMU_PFN_MASK);
		pt = page_address(page);
	} else {
		page = alloc_page(GFP_KERNEL | __GFP_DMA);
		if (!page)
			return NULL;

		pt = page_address(page);
		SetPageReserved(page);

		for (i = 0; i < SMMU_NUM_PTE; i++)
			pt[i] = SMMU_PTE_VACANT(i);

		smmu->soc->ops->flush_dcache(page, 0, SMMU_SIZE_PT);

		pd[pde] = SMMU_MK_PDE(page, SMMU_PDE_ATTR | SMMU_PDE_NEXT);

		smmu->soc->ops->flush_dcache(as->pd, pde << 2, 4);
		smmu_flush_ptc(smmu, as->pd, pde << 2);
		smmu_flush_tlb_section(smmu, as->id, iova);
		smmu_flush(smmu);
	}

	*pagep = page;

	return &pt[pte];
}

static int tegra_smmu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t paddr, size_t size, int prot)
{
	struct tegra_smmu_address_space *as = domain->priv;
	struct tegra_smmu *smmu = smmu_handle;
	unsigned long offset;
	struct page *page;
	u32 *pte;

	pte = as_get_pte(as, iova, &page);
	if (!pte)
		return -ENOMEM;

	offset = offset_in_page(pte);

	*pte = __phys_to_pfn(paddr) | SMMU_PTE_ATTR;

	smmu->soc->ops->flush_dcache(page, offset, 4);
	smmu_flush_ptc(smmu, page, offset);
	smmu_flush_tlb_group(smmu, as->id, iova);
	smmu_flush(smmu);

	return 0;
}

static size_t tegra_smmu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t size)
{
	struct tegra_smmu_address_space *as = domain->priv;
	struct tegra_smmu *smmu = smmu_handle;
	unsigned long offset;
	struct page *page;
	u32 *pte;

	pte = as_get_pte(as, iova, &page);
	if (!pte)
		return 0;

	offset = offset_in_page(pte);
	*pte = 0;

	smmu->soc->ops->flush_dcache(page, offset, 4);
	smmu_flush_ptc(smmu, page, offset);
	smmu_flush_tlb_group(smmu, as->id, iova);
	smmu_flush(smmu);

	return size;
}

static phys_addr_t tegra_smmu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	struct tegra_smmu_address_space *as = domain->priv;
	struct page *page;
	unsigned long pfn;
	u32 *pte;

	pte = as_get_pte(as, iova, &page);
	pfn = *pte & SMMU_PFN_MASK;

	return PFN_PHYS(pfn);
}

static int tegra_smmu_attach(struct iommu *iommu, struct device *dev)
{
	struct tegra_smmu *smmu = to_tegra_smmu(iommu);
	struct tegra_smmu_group *group;
	unsigned int i;

	for (i = 0; i < smmu->soc->num_groups; i++) {
		group = iommu_group_get_iommudata(smmu->groups[i]);

		if (of_match_node(group->matches, dev->of_node)) {
			pr_debug("adding device %s to group %s\n",
				 dev_name(dev), group->name);
			iommu_group_add_device(smmu->groups[i], dev);
			break;
		}
	}

	if (i == smmu->soc->num_groups)
		return 0;

#ifndef CONFIG_ARM64
	return arm_iommu_attach_device(dev, group->mapping);
#else
	return 0;
#endif
}

static int tegra_smmu_detach(struct iommu *iommu, struct device *dev)
{
	return 0;
}

static const struct iommu_ops tegra_smmu_ops = {
	.domain_init = tegra_smmu_domain_init,
	.domain_destroy = tegra_smmu_domain_destroy,
	.attach_dev = tegra_smmu_attach_dev,
	.detach_dev = tegra_smmu_detach_dev,
	.map = tegra_smmu_map,
	.unmap = tegra_smmu_unmap,
	.iova_to_phys = tegra_smmu_iova_to_phys,
	.attach = tegra_smmu_attach,
	.detach = tegra_smmu_detach,

	.pgsize_bitmap = SZ_4K,
};

static struct tegra_smmu *tegra_smmu_probe(struct device *dev,
					   const struct tegra_smmu_soc *soc,
					   void __iomem *regs)
{
	struct tegra_smmu *smmu;
	unsigned int i;
	size_t size;
	u32 value;
	int err;

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu)
		return ERR_PTR(-ENOMEM);

	size = BITS_TO_LONGS(soc->num_asids) * sizeof(long);

	smmu->asids = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!smmu->asids)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&smmu->iommu.list);
	mutex_init(&smmu->lock);

	smmu->iommu.ops = &tegra_smmu_ops;
	smmu->iommu.dev = dev;

	smmu->regs = regs;
	smmu->soc = soc;
	smmu->dev = dev;

	smmu_handle = smmu;
	bus_set_iommu(&platform_bus_type, &tegra_smmu_ops);

	smmu->num_groups = soc->num_groups;

	smmu->groups = devm_kcalloc(dev, smmu->num_groups, sizeof(*smmu->groups),
				    GFP_KERNEL);
	if (!smmu->groups)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < smmu->num_groups; i++) {
		struct tegra_smmu_group *group;

		smmu->groups[i] = iommu_group_alloc();
		if (IS_ERR(smmu->groups[i]))
			return ERR_CAST(smmu->groups[i]);

		err = iommu_group_set_name(smmu->groups[i], soc->groups[i].name);
		if (err < 0) {
		}

		group = kzalloc(sizeof(*group), GFP_KERNEL);
		if (!group)
			return ERR_PTR(-ENOMEM);

		group->matches = soc->groups[i].matches;
		group->asid = soc->groups[i].asid;
		group->name = soc->groups[i].name;

		iommu_group_set_iommudata(smmu->groups[i], group,
					  tegra_smmu_group_release);

#ifndef CONFIG_ARM64
		group->mapping = arm_iommu_create_mapping(&platform_bus_type,
							  0, SZ_2G);
		if (IS_ERR(group->mapping)) {
			dev_err(dev, "failed to create mapping for group %s: %ld\n",
				group->name, PTR_ERR(group->mapping));
			return ERR_CAST(group->mapping);
		}
#endif
	}

	value = (1 << 29) | (8 << 24) | 0x3f;
	smmu_writel(smmu, value, 0x18);

	value = (1 << 29) | (1 << 28) | 0x20;
	smmu_writel(smmu, value, 0x014);

	smmu_flush_ptc(smmu, NULL, 0);
	smmu_flush_tlb(smmu);
	smmu_writel(smmu, SMMU_CONFIG_ENABLE, SMMU_CONFIG);
	smmu_flush(smmu);

	err = iommu_add(&smmu->iommu);
	if (err < 0)
		return ERR_PTR(err);

	return smmu;
}

static int tegra_smmu_remove(struct tegra_smmu *smmu)
{
	iommu_remove(&smmu->iommu);

	return 0;
}

#ifdef CONFIG_ARCH_TEGRA_124_SOC
static const struct tegra_smmu_soc tegra124_smmu_soc = {
	.groups = tegra124_smmu_groups,
	.num_groups = ARRAY_SIZE(tegra124_smmu_groups),
	.clients = tegra124_mc_clients,
	.num_clients = ARRAY_SIZE(tegra124_mc_clients),
	.swgroups = tegra124_swgroups,
	.num_swgroups = ARRAY_SIZE(tegra124_swgroups),
	.num_asids = 128,
	.atom_size = 32,
	.ops = &tegra124_smmu_ops,
};
#endif

static const struct tegra_smmu_soc tegra132_smmu_soc = {
	.groups = tegra124_smmu_groups,
	.num_groups = ARRAY_SIZE(tegra124_smmu_groups),
	.clients = tegra124_mc_clients,
	.num_clients = ARRAY_SIZE(tegra124_mc_clients),
	.swgroups = tegra124_swgroups,
	.num_swgroups = ARRAY_SIZE(tegra124_swgroups),
	.num_asids = 128,
	.atom_size = 32,
	.ops = &tegra132_smmu_ops,
};

struct tegra_mc {
	struct device *dev;
	struct tegra_smmu *smmu;
	struct tegra_emem_timing *emem_timings;
	int num_emem_timings;
	void __iomem *regs;
	int irq;

	const struct tegra_mc_soc *soc;
};

static inline u32 mc_readl(struct tegra_mc *mc, unsigned long offset)
{
	return readl(mc->regs + offset);
}

static inline void mc_writel(struct tegra_mc *mc, u32 value, unsigned long offset)
{
	writel(value, mc->regs + offset);
}

static struct tegra_mc *global_mc = NULL;

static int t124_mc_emem_configuration_regs[] = {
	0x90,	/* MC_EMEM_ARB_CFG */
	0x94,	/* MC_EMEM_ARB_OUTSTANDING_REQ */
	0x98,	/* MC_EMEM_ARB_TIMING_RCD */
	0x9c,	/* MC_EMEM_ARB_TIMING_RP */
	0xa0,	/* MC_EMEM_ARB_TIMING_RC */
	0xa4,	/* MC_EMEM_ARB_TIMING_RAS */
	0xa8,	/* MC_EMEM_ARB_TIMING_FAW */
	0xac,	/* MC_EMEM_ARB_TIMING_RRD */
	0xb0,	/* MC_EMEM_ARB_TIMING_RAP2PRE */
	0xb4,	/* MC_EMEM_ARB_TIMING_WAP2PRE */
	0xb8,	/* MC_EMEM_ARB_TIMING_R2R */
	0xbc,	/* MC_EMEM_ARB_TIMING_W2W */
	0xc0,	/* MC_EMEM_ARB_TIMING_R2W */
	0xc4,	/* MC_EMEM_ARB_TIMING_W2R */
	0xd0,	/* MC_EMEM_ARB_DA_TURNS */
	0xd4,	/* MC_EMEM_ARB_DA_COVERS */
	0xd8,	/* MC_EMEM_ARB_MISC0 */
	0xdc,	/* MC_EMEM_ARB_MISC1 */
	0xe0	/* MC_EMEM_ARB_RING1_THROTTLE */
};

struct tegra_emem_timing {
	unsigned long rate;

	u32 configuration[ARRAY_SIZE(t124_mc_emem_configuration_regs)];
};

static int emem_load_timing(struct device *dev,
			    struct tegra_emem_timing *timing,
			    struct device_node *node)
{
	int err;
	u32 tmp;

	err = of_property_read_u32(node, "clock-frequency", &tmp);
	if (err) {
		dev_err(dev,
			"timing %s: failed to read rate\n", node->name);
		return err;
	}

	timing->rate = tmp;

	err = of_property_read_u32_array(node, "nvidia,emem-configuration",
					 timing->configuration,
					 ARRAY_SIZE(timing->configuration));
	if (err) {
		dev_err(dev,
			"timing %s: failed to read EMEM configuration\n",
			node->name);
		return err;
	}

	return 0;
}

static int tegra_emem_probe(struct device *dev, struct tegra_mc *mc)
{
	struct device_node *node, *child;
	int err, i, child_count;
	u32 ram_code, node_ram_code;

	ram_code = tegra_read_ram_code();

	mc->num_emem_timings = 0;

	for_each_child_of_node(dev->of_node, node) {
		if (strcmp(node->name, "timings"))
			continue;

		err = of_property_read_u32(node, "nvidia,ram-code",
						&node_ram_code);
		if (err) {
			dev_warn(dev, "skipping timing without ram-code\n");
			continue;
		}

		if (node_ram_code != ram_code)
			continue;

		child_count = of_get_child_count(node);

		mc->emem_timings = devm_kzalloc(dev,
			sizeof(struct tegra_emem_timing) * child_count,
			GFP_KERNEL);
		if (!mc->emem_timings)
			return -ENOMEM;

		mc->num_emem_timings = child_count;

		i = 0;

		for_each_child_of_node(node, child) {
			struct tegra_emem_timing *timing = mc->emem_timings + (i++);

			err = emem_load_timing(dev, timing, child);
			if (err)
				return err;
		}

		break;
	}

	return 0;
}

int tegra_mc_get_emem_device_count(u8 *count)
{
	if (!global_mc)
		return -EPROBE_DEFER;

	*count = (mc_readl(global_mc, MC_EMEM_ADR_CFG) &
			MC_EMEM_ADR_CFG_EMEM_NUMDEV) + 1;

	return 0;
}

int tegra_mc_write_emem_configuration(unsigned long rate)
{
	int i;
	struct tegra_emem_timing *timing = NULL;

	if (!global_mc)
		return -EPROBE_DEFER;

	for (i = 0; i < global_mc->num_emem_timings; ++i) {
		if (global_mc->emem_timings[i].rate == rate) {
			timing = global_mc->emem_timings + i;
			break;
		}
	}

	if (!timing)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(timing->configuration); ++i)
		mc_writel(global_mc, timing->configuration[i],
			  t124_mc_emem_configuration_regs[i]);

	mc_readl(global_mc, MC_EMEM_ADR_CFG);

	wmb();

	return 0;
}

struct tegra_mc_soc {
	const struct tegra_mc_client *clients;
	unsigned int num_clients;

	const struct tegra_smmu_soc *smmu;
};

#ifdef CONFIG_ARCH_TEGRA_124_SOC
static const struct tegra_mc_soc tegra124_mc_soc = {
	.clients = tegra124_mc_clients,
	.num_clients = ARRAY_SIZE(tegra124_mc_clients),
	.smmu = &tegra124_smmu_soc,
};
#endif

static const struct tegra_mc_soc tegra132_mc_soc = {
	.clients = tegra124_mc_clients,
	.num_clients = ARRAY_SIZE(tegra124_mc_clients),
	.smmu = &tegra132_smmu_soc,
};

static const struct of_device_id tegra_mc_of_match[] = {
#ifdef CONFIG_ARCH_TEGRA_124_SOC
	{ .compatible = "nvidia,tegra124-mc", .data = &tegra124_mc_soc },
#endif
	{ .compatible = "nvidia,tegra132-mc", .data = &tegra132_mc_soc },
	{ }
};

static irqreturn_t tegra124_mc_irq(int irq, void *data)
{
	struct tegra_mc *mc = data;
	u32 value, status, mask;

	/* mask all interrupts to avoid flooding */
	mask = mc_readl(mc, MC_INTMASK);
	mc_writel(mc, 0, MC_INTMASK);

	status = mc_readl(mc, MC_INTSTATUS);
	mc_writel(mc, status, MC_INTSTATUS);

	dev_dbg(mc->dev, "INTSTATUS: %08x\n", status);

	if (status & MC_INT_DECERR_MTS)
		dev_dbg(mc->dev, "  DECERR_MTS\n");

	if (status & MC_INT_SECERR_SEC)
		dev_dbg(mc->dev, "  SECERR_SEC\n");

	if (status & MC_INT_DECERR_VPR)
		dev_dbg(mc->dev, "  DECERR_VPR\n");

	if (status & MC_INT_INVALID_APB_ASID_UPDATE)
		dev_dbg(mc->dev, "  INVALID_APB_ASID_UPDATE\n");

	if (status & MC_INT_INVALID_SMMU_PAGE)
		dev_dbg(mc->dev, "  INVALID_SMMU_PAGE\n");

	if (status & MC_INT_ARBITRATION_EMEM)
		dev_dbg(mc->dev, "  ARBITRATION_EMEM\n");

	if (status & MC_INT_SECURITY_VIOLATION)
		dev_dbg(mc->dev, "  SECURITY_VIOLATION\n");

	if (status & MC_INT_DECERR_EMEM)
		dev_dbg(mc->dev, "  DECERR_EMEM\n");

	value = mc_readl(mc, MC_ERR_STATUS);

	dev_dbg(mc->dev, "ERR_STATUS: %08x\n", value);
	dev_dbg(mc->dev, "  type: %x\n", (value >> 28) & 0x7);
	dev_dbg(mc->dev, "  protection: %x\n", (value >> 25) & 0x7);
	dev_dbg(mc->dev, "  adr_hi: %x\n", (value >> 20) & 0x3);
	dev_dbg(mc->dev, "  swap: %x\n", (value >> 18) & 0x1);
	dev_dbg(mc->dev, "  security: %x\n", (value >> 17) & 0x1);
	dev_dbg(mc->dev, "  r/w: %x\n", (value >> 16) & 0x1);
	dev_dbg(mc->dev, "  adr1: %x\n", (value >> 12) & 0x7);
	dev_dbg(mc->dev, "  client: %x\n", value & 0x7f);

	value = mc_readl(mc, MC_ERR_ADR);
	dev_dbg(mc->dev, "ERR_ADR: %08x\n", value);

	mc_writel(mc, mask, MC_INTMASK);

	return IRQ_HANDLED;
}

static int tegra_mc_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct resource *res;
	struct tegra_mc *mc;
	unsigned int i;
	u32 value;
	int err;

	match = of_match_node(tegra_mc_of_match, pdev->dev.of_node);
	if (!match)
		return -ENODEV;

	mc = devm_kzalloc(&pdev->dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;

	platform_set_drvdata(pdev, mc);
	mc->soc = match->data;
	mc->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mc->regs))
		return PTR_ERR(mc->regs);

	for (i = 0; i < mc->soc->num_clients; i++) {
		const struct latency_allowance *la = &mc->soc->clients[i].latency;
		u32 value;

		value = readl(mc->regs + la->reg);
		value &= ~(la->mask << la->shift);
		value |= (la->def & la->mask) << la->shift;
		writel(value, mc->regs + la->reg);
	}

	mc->smmu = tegra_smmu_probe(&pdev->dev, mc->soc->smmu, mc->regs);
	if (IS_ERR(mc->smmu)) {
		dev_err(&pdev->dev, "failed to probe SMMU: %ld\n",
			PTR_ERR(mc->smmu));
		return PTR_ERR(mc->smmu);
	}

	err = tegra_emem_probe(&pdev->dev, mc);
	if (err) {
		dev_err(&pdev->dev, "failed to probe EMEM: %d\n", err);
		return err;
	}

	mc->irq = platform_get_irq(pdev, 0);
	if (mc->irq < 0) {
		dev_err(&pdev->dev, "interrupt not specified\n");
		return mc->irq;
	}

	err = devm_request_irq(&pdev->dev, mc->irq, tegra124_mc_irq,
			       IRQF_SHARED, dev_name(&pdev->dev), mc);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to request IRQ#%u: %d\n", mc->irq,
			err);
		return err;
	}

	/* FIXME ARBITRATION_EMEM (and probably some other one, but with lesser
	 * effect, cause bandwidth losses on low freqs due to high amount of
	 * interrupts */

	value = MC_INT_DECERR_MTS | MC_INT_SECERR_SEC | MC_INT_DECERR_VPR |
		MC_INT_INVALID_APB_ASID_UPDATE | MC_INT_INVALID_SMMU_PAGE |
		MC_INT_ARBITRATION_EMEM | MC_INT_SECURITY_VIOLATION |
		MC_INT_DECERR_EMEM;
	mc_writel(mc, value, MC_INTMASK);

	global_mc = mc;

	return 0;
}

static int tegra_mc_remove(struct platform_device *pdev)
{
	struct tegra_mc *mc = platform_get_drvdata(pdev);
	int err;

	err = tegra_smmu_remove(mc->smmu);
	if (err < 0)
		dev_err(&pdev->dev, "failed to remove SMMU: %d\n", err);

	return 0;
}

static struct platform_driver tegra_mc_driver = {
	.driver = {
		.name = "tegra124-mc",
		.of_match_table = tegra_mc_of_match,
	},
	.probe = tegra_mc_probe,
	.remove = tegra_mc_remove,
};
module_platform_driver(tegra_mc_driver);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra124 Memory Controller driver");
MODULE_LICENSE("GPL v2");
