/*
 * drivers/clk/tegra/clk-emc.c
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

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sort.h>
#include <linux/string.h>

#include <soc/tegra/fuse.h>

#define EMC_FBIO_CFG5				0x104
#define	EMC_FBIO_CFG5_DRAM_TYPE_MASK		0x3
#define	EMC_FBIO_CFG5_DRAM_TYPE_SHIFT		0

#define EMC_INTSTATUS				0x0
#define EMC_INTSTATUS_CLKCHANGE_COMPLETE	BIT(4)

#define EMC_CFG					0xc
#define EMC_CFG_DRAM_CLKSTOP_PD			BIT(31)
#define EMC_CFG_DRAM_CLKSTOP_SR			BIT(30)
#define EMC_CFG_DRAM_ACPD			BIT(29)
#define EMC_CFG_DYN_SREF			BIT(28)
#define EMC_CFG_PWR_MASK			((0xF << 28) | BIT(18))
#define EMC_CFG_DSR_VTTGEN_DRV_EN		BIT(18)

#define EMC_REFCTRL				0x20
#define EMC_REFCTRL_DEV_SEL_SHIFT		0
#define EMC_REFCTRL_ENABLE			BIT(31)

#define EMC_TIMING_CONTROL			0x28
#define EMC_RC					0x2c
#define EMC_RFC					0x30
#define EMC_RAS					0x34
#define EMC_RP					0x38
#define EMC_R2W					0x3c
#define EMC_W2R					0x40
#define EMC_R2P					0x44
#define EMC_W2P					0x48
#define EMC_RD_RCD				0x4c
#define EMC_WR_RCD				0x50
#define EMC_RRD					0x54
#define EMC_REXT				0x58
#define EMC_WDV					0x5c
#define EMC_QUSE				0x60
#define EMC_QRST				0x64
#define EMC_QSAFE				0x68
#define EMC_RDV					0x6c
#define EMC_REFRESH				0x70
#define EMC_BURST_REFRESH_NUM			0x74
#define EMC_PDEX2WR				0x78
#define EMC_PDEX2RD				0x7c
#define EMC_PCHG2PDEN				0x80
#define EMC_ACT2PDEN				0x84
#define EMC_AR2PDEN				0x88
#define EMC_RW2PDEN				0x8c
#define EMC_TXSR				0x90
#define EMC_TCKE				0x94
#define EMC_TFAW				0x98
#define EMC_TRPAB				0x9c
#define EMC_TCLKSTABLE				0xa0
#define EMC_TCLKSTOP				0xa4
#define EMC_TREFBW				0xa8
#define EMC_ODT_WRITE				0xb0
#define EMC_ODT_READ				0xb4
#define EMC_WEXT				0xb8
#define EMC_CTT					0xbc
#define EMC_RFC_SLR				0xc0
#define EMC_MRS_WAIT_CNT2			0xc4

#define EMC_MRS_WAIT_CNT			0xc8
#define EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT	0
#define EMC_MRS_WAIT_CNT_SHORT_WAIT_MASK	\
	(0x3FF << EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT)
#define EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT	16
#define EMC_MRS_WAIT_CNT_LONG_WAIT_MASK		\
	(0x3FF << EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT)

#define EMC_MRS					0xcc
#define EMC_MODE_SET_DLL_RESET			BIT(8)
#define EMC_MODE_SET_LONG_CNT			BIT(26)
#define EMC_EMRS				0xd0
#define EMC_REF					0xd4
#define EMC_PRE					0xd8

#define EMC_SELF_REF				0xe0
#define EMC_SELF_REF_CMD_ENABLED		BIT(0)
#define EMC_SELF_REF_DEV_SEL_SHIFT		30

#define EMC_MRW					0xe8

#define EMC_MRR					0xec
#define EMC_MRR_MA_SHIFT			16
#define LPDDR2_MR4_TEMP_SHIFT			0

#define EMC_XM2DQSPADCTRL3			0xf8
#define EMC_FBIO_SPARE				0x100

#define EMC_FBIO_CFG6				0x114
#define EMC_EMRS2				0x12c
#define EMC_MRW2				0x134
#define EMC_MRW4				0x13c
#define EMC_EINPUT				0x14c
#define EMC_EINPUT_DURATION			0x150
#define EMC_PUTERM_EXTRA			0x154
#define EMC_TCKESR				0x158
#define EMC_TPD					0x15c

#define EMC_AUTO_CAL_CONFIG			0x2a4
#define EMC_AUTO_CAL_CONFIG_AUTO_CAL_START	BIT(31)
#define EMC_AUTO_CAL_INTERVAL			0x2a8
#define EMC_AUTO_CAL_STATUS			0x2ac
#define EMC_AUTO_CAL_STATUS_ACTIVE		BIT(31)
#define EMC_STATUS				0x2b4
#define EMC_STATUS_TIMING_UPDATE_STALLED	BIT(23)

#define EMC_CFG_2				0x2b8
#define EMC_CFG_2_MODE_SHIFT			0
#define EMC_CFG_2_DIS_STP_OB_CLK_DURING_NON_WR	BIT(6)

#define EMC_CFG_DIG_DLL				0x2bc
#define EMC_CFG_DIG_DLL_PERIOD			0x2c0
#define EMC_RDV_MASK				0x2cc
#define EMC_WDV_MASK				0x2d0
#define EMC_CTT_DURATION			0x2d8
#define EMC_CTT_TERM_CTRL			0x2dc
#define EMC_ZCAL_INTERVAL			0x2e0
#define EMC_ZCAL_WAIT_CNT			0x2e4

#define EMC_ZQ_CAL				0x2ec
#define EMC_ZQ_CAL_CMD				BIT(0)
#define EMC_ZQ_CAL_LONG				BIT(4)
#define EMC_ZQ_CAL_LONG_CMD_DEV0		\
	(DRAM_DEV_SEL_0 | EMC_ZQ_CAL_LONG | EMC_ZQ_CAL_CMD)
#define EMC_ZQ_CAL_LONG_CMD_DEV1		\
	(DRAM_DEV_SEL_1 | EMC_ZQ_CAL_LONG | EMC_ZQ_CAL_CMD)

#define EMC_XM2CMDPADCTRL			0x2f0
#define EMC_XM2DQSPADCTRL			0x2f8
#define EMC_XM2DQSPADCTRL2			0x2fc
#define EMC_XM2DQSPADCTRL2_RX_FT_REC_ENABLE	BIT(0)
#define EMC_XM2DQSPADCTRL2_VREF_ENABLE		BIT(5)
#define EMC_XM2DQPADCTRL			0x300
#define EMC_XM2DQPADCTRL2			0x304
#define EMC_XM2CLKPADCTRL			0x308
#define EMC_XM2COMPPADCTRL			0x30c
#define EMC_XM2VTTGENPADCTRL			0x310
#define EMC_XM2VTTGENPADCTRL2			0x314
#define EMC_XM2VTTGENPADCTRL3			0x318
#define EMC_XM2DQSPADCTRL4			0x320
#define EMC_DLL_XFORM_DQS0			0x328
#define EMC_DLL_XFORM_DQS1			0x32c
#define EMC_DLL_XFORM_DQS2			0x330
#define EMC_DLL_XFORM_DQS3			0x334
#define EMC_DLL_XFORM_DQS4			0x338
#define EMC_DLL_XFORM_DQS5			0x33c
#define EMC_DLL_XFORM_DQS6			0x340
#define EMC_DLL_XFORM_DQS7			0x344
#define EMC_DLL_XFORM_QUSE0			0x348
#define EMC_DLL_XFORM_QUSE1			0x34c
#define EMC_DLL_XFORM_QUSE2			0x350
#define EMC_DLL_XFORM_QUSE3			0x354
#define EMC_DLL_XFORM_QUSE4			0x358
#define EMC_DLL_XFORM_QUSE5			0x35c
#define EMC_DLL_XFORM_QUSE6			0x360
#define EMC_DLL_XFORM_QUSE7			0x364
#define EMC_DLL_XFORM_DQ0			0x368
#define EMC_DLL_XFORM_DQ1			0x36c
#define EMC_DLL_XFORM_DQ2			0x370
#define EMC_DLL_XFORM_DQ3			0x374
#define EMC_DLI_TRIM_TXDQS0			0x3a8
#define EMC_DLI_TRIM_TXDQS1			0x3ac
#define EMC_DLI_TRIM_TXDQS2			0x3b0
#define EMC_DLI_TRIM_TXDQS3			0x3b4
#define EMC_DLI_TRIM_TXDQS4			0x3b8
#define EMC_DLI_TRIM_TXDQS5			0x3bc
#define EMC_DLI_TRIM_TXDQS6			0x3c0
#define EMC_DLI_TRIM_TXDQS7			0x3c4
#define EMC_STALL_THEN_EXE_AFTER_CLKCHANGE	0x3cc
#define EMC_SEL_DPD_CTRL			0x3d8
#define EMC_SEL_DPD_CTRL_DATA_SEL_DPD		BIT(8)
#define EMC_SEL_DPD_CTRL_ODT_SEL_DPD		BIT(5)
#define EMC_SEL_DPD_CTRL_RESET_SEL_DPD		BIT(4)
#define EMC_SEL_DPD_CTRL_CA_SEL_DPD		BIT(3)
#define EMC_SEL_DPD_CTRL_CLK_SEL_DPD		BIT(2)
#define EMC_SEL_DPD_CTRL_DDR3_MASK	\
	((0xf << 2) | BIT(8))
#define EMC_SEL_DPD_CTRL_MASK \
	((0x3 << 2) | BIT(5) | BIT(8))
#define EMC_PRE_REFRESH_REQ_CNT			0x3dc
#define EMC_DYN_SELF_REF_CONTROL		0x3e0
#define EMC_TXSRDLL				0x3e4
#define EMC_CCFIFO_ADDR				0x3e8
#define EMC_CCFIFO_DATA				0x3ec
#define EMC_CCFIFO_STATUS			0x3f0
#define EMC_CDB_CNTL_1				0x3f4
#define EMC_CDB_CNTL_2				0x3f8
#define EMC_XM2CLKPADCTRL2			0x3fc
#define EMC_AUTO_CAL_CONFIG2			0x458
#define EMC_AUTO_CAL_CONFIG3			0x45c
#define EMC_IBDLY				0x468
#define EMC_DLL_XFORM_ADDR0			0x46c
#define EMC_DLL_XFORM_ADDR1			0x470
#define EMC_DLL_XFORM_ADDR2			0x474
#define EMC_DSR_VTTGEN_DRV			0x47c
#define EMC_TXDSRVTTGEN				0x480
#define EMC_XM2CMDPADCTRL4			0x484
#define EMC_XM2CMDPADCTRL5			0x488
#define EMC_DLL_XFORM_DQS8			0x4a0
#define EMC_DLL_XFORM_DQS9			0x4a4
#define EMC_DLL_XFORM_DQS10			0x4a8
#define EMC_DLL_XFORM_DQS11			0x4ac
#define EMC_DLL_XFORM_DQS12			0x4b0
#define EMC_DLL_XFORM_DQS13			0x4b4
#define EMC_DLL_XFORM_DQS14			0x4b8
#define EMC_DLL_XFORM_DQS15			0x4bc
#define EMC_DLL_XFORM_QUSE8			0x4c0
#define EMC_DLL_XFORM_QUSE9			0x4c4
#define EMC_DLL_XFORM_QUSE10			0x4c8
#define EMC_DLL_XFORM_QUSE11			0x4cc
#define EMC_DLL_XFORM_QUSE12			0x4d0
#define EMC_DLL_XFORM_QUSE13			0x4d4
#define EMC_DLL_XFORM_QUSE14			0x4d8
#define EMC_DLL_XFORM_QUSE15			0x4dc
#define EMC_DLL_XFORM_DQ4			0x4e0
#define EMC_DLL_XFORM_DQ5			0x4e4
#define EMC_DLL_XFORM_DQ6			0x4e8
#define EMC_DLL_XFORM_DQ7			0x4ec
#define EMC_DLI_TRIM_TXDQS8			0x520
#define EMC_DLI_TRIM_TXDQS9			0x524
#define EMC_DLI_TRIM_TXDQS10			0x528
#define EMC_DLI_TRIM_TXDQS11			0x52c
#define EMC_DLI_TRIM_TXDQS12			0x530
#define EMC_DLI_TRIM_TXDQS13			0x534
#define EMC_DLI_TRIM_TXDQS14			0x538
#define EMC_DLI_TRIM_TXDQS15			0x53c
#define EMC_CDB_CNTL_3				0x540
#define EMC_XM2DQSPADCTRL5			0x544
#define EMC_XM2DQSPADCTRL6			0x548
#define EMC_XM2DQPADCTRL3			0x54c
#define EMC_DLL_XFORM_ADDR3			0x550
#define EMC_DLL_XFORM_ADDR4			0x554
#define EMC_DLL_XFORM_ADDR5			0x558
#define EMC_CFG_PIPE				0x560
#define EMC_QPOP				0x564
#define EMC_QUSE_WIDTH				0x568
#define EMC_PUTERM_WIDTH			0x56c
#define EMC_BGBIAS_CTL0				0x570
#define EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_RX BIT(3)
#define EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_VTTGEN BIT(2)
#define EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD	BIT(1)
#define EMC_PUTERM_ADJ				0x574

#define DRAM_DEV_SEL_ALL			0
#define DRAM_DEV_SEL_0				(2 << 30)
#define DRAM_DEV_SEL_1				(1 << 30)

#define CLK_SOURCE_EMC				0x19c
#define CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_SHIFT	0
#define CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_MASK	0xff
#define CLK_SOURCE_EMC_EMC_2X_CLK_SRC_SHIFT	29
#define CLK_SOURCE_EMC_EMC_2X_CLK_SRC_MASK	0x7

#define EMC_CFG_POWER_FEATURES_MASK		\
	(EMC_CFG_DYN_SREF | EMC_CFG_DRAM_ACPD | EMC_CFG_DRAM_CLKSTOP_SR | \
	EMC_CFG_DRAM_CLKSTOP_PD | EMC_CFG_DSR_VTTGEN_DRV_EN)
#define EMC_REFCTRL_DEV_SEL(n) (((n > 1) ? 0 : 2) << EMC_REFCTRL_DEV_SEL_SHIFT)
#define EMC_DRAM_DEV_SEL(n) ((n > 1) ? DRAM_DEV_SEL_ALL : DRAM_DEV_SEL_0)

#define EMC_STATUS_UPDATE_TIMEOUT		1000

enum emc_dram_type {
	DRAM_TYPE_DDR3 = 0,
	DRAM_TYPE_DDR1 = 1,
	DRAM_TYPE_LPDDR3 = 2,
	DRAM_TYPE_DDR2 = 3
};

enum emc_dll_change {
	DLL_CHANGE_NONE,
	DLL_CHANGE_ON,
	DLL_CHANGE_OFF
};

static int t124_emc_burst_regs[] = {
	EMC_RC,
	EMC_RFC,
	EMC_RFC_SLR,
	EMC_RAS,
	EMC_RP,
	EMC_R2W,
	EMC_W2R,
	EMC_R2P,
	EMC_W2P,
	EMC_RD_RCD,
	EMC_WR_RCD,
	EMC_RRD,
	EMC_REXT,
	EMC_WEXT,
	EMC_WDV,
	EMC_WDV_MASK,
	EMC_QUSE,
	EMC_QUSE_WIDTH,
	EMC_IBDLY,
	EMC_EINPUT,
	EMC_EINPUT_DURATION,
	EMC_PUTERM_EXTRA,
	EMC_PUTERM_WIDTH,
	EMC_PUTERM_ADJ,
	EMC_CDB_CNTL_1,
	EMC_CDB_CNTL_2,
	EMC_CDB_CNTL_3,
	EMC_QRST,
	EMC_QSAFE,
	EMC_RDV,
	EMC_RDV_MASK,
	EMC_REFRESH,
	EMC_BURST_REFRESH_NUM,
	EMC_PRE_REFRESH_REQ_CNT,
	EMC_PDEX2WR,
	EMC_PDEX2RD,
	EMC_PCHG2PDEN,
	EMC_ACT2PDEN,
	EMC_AR2PDEN,
	EMC_RW2PDEN,
	EMC_TXSR,
	EMC_TXSRDLL,
	EMC_TCKE,
	EMC_TCKESR,
	EMC_TPD,
	EMC_TFAW,
	EMC_TRPAB,
	EMC_TCLKSTABLE,
	EMC_TCLKSTOP,
	EMC_TREFBW,
	EMC_FBIO_CFG6,
	EMC_ODT_WRITE,
	EMC_ODT_READ,
	EMC_FBIO_CFG5,
	EMC_CFG_DIG_DLL,
	EMC_CFG_DIG_DLL_PERIOD,
	EMC_DLL_XFORM_DQS0,
	EMC_DLL_XFORM_DQS1,
	EMC_DLL_XFORM_DQS2,
	EMC_DLL_XFORM_DQS3,
	EMC_DLL_XFORM_DQS4,
	EMC_DLL_XFORM_DQS5,
	EMC_DLL_XFORM_DQS6,
	EMC_DLL_XFORM_DQS7,
	EMC_DLL_XFORM_DQS8,
	EMC_DLL_XFORM_DQS9,
	EMC_DLL_XFORM_DQS10,
	EMC_DLL_XFORM_DQS11,
	EMC_DLL_XFORM_DQS12,
	EMC_DLL_XFORM_DQS13,
	EMC_DLL_XFORM_DQS14,
	EMC_DLL_XFORM_DQS15,
	EMC_DLL_XFORM_QUSE0,
	EMC_DLL_XFORM_QUSE1,
	EMC_DLL_XFORM_QUSE2,
	EMC_DLL_XFORM_QUSE3,
	EMC_DLL_XFORM_QUSE4,
	EMC_DLL_XFORM_QUSE5,
	EMC_DLL_XFORM_QUSE6,
	EMC_DLL_XFORM_QUSE7,
	EMC_DLL_XFORM_ADDR0,
	EMC_DLL_XFORM_ADDR1,
	EMC_DLL_XFORM_ADDR2,
	EMC_DLL_XFORM_ADDR3,
	EMC_DLL_XFORM_ADDR4,
	EMC_DLL_XFORM_ADDR5,
	EMC_DLL_XFORM_QUSE8,
	EMC_DLL_XFORM_QUSE9,
	EMC_DLL_XFORM_QUSE10,
	EMC_DLL_XFORM_QUSE11,
	EMC_DLL_XFORM_QUSE12,
	EMC_DLL_XFORM_QUSE13,
	EMC_DLL_XFORM_QUSE14,
	EMC_DLL_XFORM_QUSE15,
	EMC_DLI_TRIM_TXDQS0,
	EMC_DLI_TRIM_TXDQS1,
	EMC_DLI_TRIM_TXDQS2,
	EMC_DLI_TRIM_TXDQS3,
	EMC_DLI_TRIM_TXDQS4,
	EMC_DLI_TRIM_TXDQS5,
	EMC_DLI_TRIM_TXDQS6,
	EMC_DLI_TRIM_TXDQS7,
	EMC_DLI_TRIM_TXDQS8,
	EMC_DLI_TRIM_TXDQS9,
	EMC_DLI_TRIM_TXDQS10,
	EMC_DLI_TRIM_TXDQS11,
	EMC_DLI_TRIM_TXDQS12,
	EMC_DLI_TRIM_TXDQS13,
	EMC_DLI_TRIM_TXDQS14,
	EMC_DLI_TRIM_TXDQS15,
	EMC_DLL_XFORM_DQ0,
	EMC_DLL_XFORM_DQ1,
	EMC_DLL_XFORM_DQ2,
	EMC_DLL_XFORM_DQ3,
	EMC_DLL_XFORM_DQ4,
	EMC_DLL_XFORM_DQ5,
	EMC_DLL_XFORM_DQ6,
	EMC_DLL_XFORM_DQ7,
	EMC_XM2CMDPADCTRL,
	EMC_XM2CMDPADCTRL4,
	EMC_XM2CMDPADCTRL5,
	EMC_XM2DQSPADCTRL2,
	EMC_XM2DQPADCTRL2,
	EMC_XM2DQPADCTRL3,
	EMC_XM2CLKPADCTRL,
	EMC_XM2CLKPADCTRL2,
	EMC_XM2COMPPADCTRL,
	EMC_XM2VTTGENPADCTRL,
	EMC_XM2VTTGENPADCTRL2,
	EMC_XM2VTTGENPADCTRL3,
	EMC_XM2DQSPADCTRL3,
	EMC_XM2DQSPADCTRL4,
	EMC_XM2DQSPADCTRL5,
	EMC_XM2DQSPADCTRL6,
	EMC_DSR_VTTGEN_DRV,
	EMC_TXDSRVTTGEN,
	EMC_FBIO_SPARE,
	EMC_ZCAL_INTERVAL,
	EMC_ZCAL_WAIT_CNT,
	EMC_MRS_WAIT_CNT,
	EMC_MRS_WAIT_CNT2,
	EMC_CTT,
	EMC_CTT_DURATION,
	EMC_CFG_PIPE,
	EMC_DYN_SELF_REF_CONTROL,
	EMC_QPOP
};

const char *emc_parent_clk_names[] = {
	"pll_m", "pll_c", "pll_p", "clk_m", "pll_m_ud",
	"pll_c2", "pll_c3", "pll_c_ud"
};

/* List of clock sources for various parents the EMC clock can have.
 * When we change the timing to a timing with a parent that has the same
 * clock source as the current parent, we must first change to a backup
 * timing that has a different clock source.
 */

#define EMC_SRC_PLL_M 0
#define EMC_SRC_PLL_C 1
#define EMC_SRC_PLL_P 2
#define EMC_SRC_CLK_M 3
#define EMC_SRC_PLL_C2 4
#define EMC_SRC_PLL_C3 5
const char emc_parent_clk_sources[] = {
	EMC_SRC_PLL_M, EMC_SRC_PLL_C, EMC_SRC_PLL_P, EMC_SRC_CLK_M,
	EMC_SRC_PLL_M, EMC_SRC_PLL_C2, EMC_SRC_PLL_C3, EMC_SRC_PLL_C
};

struct emc_timing {
	unsigned long rate, parent_rate;
	u8 parent_index;
	struct clk *parent;

	/* Store EMC burst data in a union to minimize mistakes. This allows
	 * us to use the same burst data lists as used by the downstream and
	 * ChromeOS kernels.
	 */
	union {
		u32 emc_burst_data[ARRAY_SIZE(t124_emc_burst_regs)];
		struct {
			u32 __pad0[121];
			u32 emc_xm2dqspadctrl2;
			u32 __pad1[15];
			u32 emc_zcal_interval;
			u32 __pad2[1];
			u32 emc_mrs_wait_cnt;
		};
	};

	u32 emc_zcal_cnt_long;
	u32 emc_auto_cal_interval;
	u32 emc_ctt_term_ctrl;
	u32 emc_cfg;
	u32 emc_cfg_2;
	u32 emc_sel_dpd_ctrl;
	u32 __emc_cfg_dig_dll;
	u32 emc_bgbias_ctl0;
	u32 emc_auto_cal_config2;
	u32 emc_auto_cal_config3;
	u32 emc_auto_cal_config;
	u32 emc_mode_reset;
	u32 emc_mode_1;
	u32 emc_mode_2;
	u32 emc_mode_4;
};

struct tegra_emc {
	struct platform_device *pdev;

	struct clk_hw hw;

	void __iomem *emc_regs;
	void __iomem *clk_regs;

	enum emc_dram_type dram_type;
	u8 dram_num;

	struct emc_timing last_timing;
	struct emc_timing *timings;
	unsigned int num_timings;

	struct clk *prev_parent;

	bool changing_timing;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * *
 * Timing change sequence functions                *
 * * * * * * * * * * * * * * * * * * * * * * * * * */

static void emc_ccfifo_writel(struct tegra_emc *tegra, u32 val,
			      unsigned long offs)
{
	writel(val, tegra->emc_regs + EMC_CCFIFO_DATA);
	writel(offs, tegra->emc_regs + EMC_CCFIFO_ADDR);
}

static void emc_seq_update_timing(struct tegra_emc *tegra)
{
	int i;

	writel(1, tegra->emc_regs + EMC_TIMING_CONTROL);

	for (i = 0; i < EMC_STATUS_UPDATE_TIMEOUT; ++i) {
		if (!(readl(tegra->emc_regs + EMC_STATUS) &
		    EMC_STATUS_TIMING_UPDATE_STALLED))
			return;
	}

	dev_err(&tegra->pdev->dev, "timing update failed\n");
}

static void emc_seq_disable_auto_cal(struct tegra_emc *tegra)
{
	int i;

	writel(0, tegra->emc_regs + EMC_AUTO_CAL_INTERVAL);

	for (i = 0; i < EMC_STATUS_UPDATE_TIMEOUT; ++i) {
		if (!(readl(tegra->emc_regs + EMC_AUTO_CAL_STATUS) &
		    EMC_AUTO_CAL_STATUS_ACTIVE))
			return;
	}

	dev_err(&tegra->pdev->dev, "auto cal disable failed\n");
}

static void emc_seq_wait_clkchange(struct tegra_emc *tegra)
{
	int i;

	for (i = 0; i < EMC_STATUS_UPDATE_TIMEOUT; ++i) {
		if (readl(tegra->emc_regs + EMC_INTSTATUS) &
		    EMC_INTSTATUS_CLKCHANGE_COMPLETE)
			return;
	}

	dev_err(&tegra->pdev->dev, "clkchange failed\n");
}

static void emc_change_timing(struct tegra_emc *tegra,
			      const struct emc_timing *timing,
			      u32 car_value)
{
	u32 val, val2;
	bool update = false;
	int pre_wait = 0;
	int i, err;
	enum emc_dll_change dll_change;

	BUG_ON(timing->rate == tegra->last_timing.rate);
	BUG_ON(ARRAY_SIZE(timing->emc_burst_data) !=
	       ARRAY_SIZE(t124_emc_burst_regs));

	if ((tegra->last_timing.emc_mode_1 & 0x1) == (timing->emc_mode_1 & 1))
		dll_change = DLL_CHANGE_NONE;
	else if (timing->emc_mode_1 & 1)
		dll_change = DLL_CHANGE_ON;
	else
		dll_change = DLL_CHANGE_OFF;

	/* Clear CLKCHANGE_COMPLETE interrupts */

	writel(EMC_INTSTATUS_CLKCHANGE_COMPLETE,
	       tegra->emc_regs + EMC_INTSTATUS);


	/* Disable dynamic self-refresh */

	val = readl(tegra->emc_regs + EMC_CFG);
	if (val & EMC_CFG_PWR_MASK) {
		val &= ~EMC_CFG_POWER_FEATURES_MASK;
		writel(val, tegra->emc_regs + EMC_CFG);

		pre_wait = 5;
	}

	/* Disable SEL_DPD_CTRL for clock change */

	val = readl(tegra->emc_regs + EMC_SEL_DPD_CTRL);
	if (val & (tegra->dram_type == DRAM_TYPE_DDR3 ?
	    EMC_SEL_DPD_CTRL_DDR3_MASK : EMC_SEL_DPD_CTRL_MASK)) {
		val &= ~(EMC_SEL_DPD_CTRL_DATA_SEL_DPD |
				EMC_SEL_DPD_CTRL_ODT_SEL_DPD |
				EMC_SEL_DPD_CTRL_CA_SEL_DPD |
				EMC_SEL_DPD_CTRL_CLK_SEL_DPD);
		if (tegra->dram_type == DRAM_TYPE_DDR3)
			val &= ~EMC_SEL_DPD_CTRL_RESET_SEL_DPD;
		writel(val, tegra->emc_regs + EMC_SEL_DPD_CTRL);
	}

	/* Prepare DQ/DQS for clock change */

	val = readl(tegra->emc_regs + EMC_BGBIAS_CTL0);
	val2 = tegra->last_timing.emc_bgbias_ctl0;
	if (!(timing->emc_bgbias_ctl0 &
	      EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_RX) &&
	    (val & EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_RX)) {
		val2 &= ~EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_RX;
		update = true;
	}

	if ((val & EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD) ||
	    (val & EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_VTTGEN)) {
		update = true;
	}

	if (update) {
		writel(val2, tegra->emc_regs + EMC_BGBIAS_CTL0);
		if (pre_wait < 5)
			pre_wait = 5;
	}

	update = false;
	val = readl(tegra->emc_regs + EMC_XM2DQSPADCTRL2);
	if (timing->emc_xm2dqspadctrl2 & EMC_XM2DQSPADCTRL2_VREF_ENABLE &&
	    !(val & EMC_XM2DQSPADCTRL2_VREF_ENABLE)) {
		val |= EMC_XM2DQSPADCTRL2_VREF_ENABLE;
		update = true;
	}

	if (timing->emc_xm2dqspadctrl2 & EMC_XM2DQSPADCTRL2_RX_FT_REC_ENABLE &&
	    !(val & EMC_XM2DQSPADCTRL2_RX_FT_REC_ENABLE)) {
		val |= EMC_XM2DQSPADCTRL2_RX_FT_REC_ENABLE;
		update = true;
	}

	if (update) {
		writel(val, tegra->emc_regs + EMC_XM2DQSPADCTRL2);
		if (pre_wait < 30)
			pre_wait = 30;
	}

	/* Wait to settle */

	if (pre_wait) {
		emc_seq_update_timing(tegra);
		udelay(pre_wait);
	}

	/* Program CTT_TERM control */

	if (tegra->last_timing.emc_ctt_term_ctrl != timing->emc_ctt_term_ctrl) {
		emc_seq_disable_auto_cal(tegra);
		writel(timing->emc_ctt_term_ctrl,
			tegra->emc_regs + EMC_CTT_TERM_CTRL);
		emc_seq_update_timing(tegra);
	}

	/* Program burst shadow registers */

	for (i = 0; i < ARRAY_SIZE(timing->emc_burst_data); ++i)
		__raw_writel(timing->emc_burst_data[i],
			     tegra->emc_regs + t124_emc_burst_regs[i]);

	err = tegra_mc_write_emem_configuration(timing->rate);
	if (err)
		dev_warn(&tegra->pdev->dev,
			 "writing emem configuration failed: %d. \n"
			 "expect reduced performance", err);

	val = timing->emc_cfg & ~EMC_CFG_POWER_FEATURES_MASK;
	emc_ccfifo_writel(tegra, val, EMC_CFG);

	/* Program AUTO_CAL_CONFIG */

	if (timing->emc_auto_cal_config2 !=
	    tegra->last_timing.emc_auto_cal_config2)
		emc_ccfifo_writel(tegra, timing->emc_auto_cal_config2,
				  EMC_AUTO_CAL_CONFIG2);
	if (timing->emc_auto_cal_config3 !=
	    tegra->last_timing.emc_auto_cal_config3)
		emc_ccfifo_writel(tegra, timing->emc_auto_cal_config3,
				  EMC_AUTO_CAL_CONFIG3);
	if (timing->emc_auto_cal_config !=
	    tegra->last_timing.emc_auto_cal_config) {
		val = timing->emc_auto_cal_config;
		val &= EMC_AUTO_CAL_CONFIG_AUTO_CAL_START;
		emc_ccfifo_writel(tegra, val, EMC_AUTO_CAL_CONFIG);
	}

	/* DDR3: predict MRS long wait count */

	if (tegra->dram_type == DRAM_TYPE_DDR3 && dll_change == DLL_CHANGE_ON) {
		u32 cnt = 32;

		if (timing->emc_zcal_interval != 0 &&
		    tegra->last_timing.emc_zcal_interval == 0)
			cnt -= tegra->dram_num * 256;

		val = timing->emc_mrs_wait_cnt
			& EMC_MRS_WAIT_CNT_SHORT_WAIT_MASK
			>> EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT;
		if (cnt < val)
			cnt = val;

		val = timing->emc_mrs_wait_cnt
			& ~EMC_MRS_WAIT_CNT_LONG_WAIT_MASK;
		val |= (cnt << EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT)
			& EMC_MRS_WAIT_CNT_LONG_WAIT_MASK;

		writel(val, tegra->emc_regs + EMC_MRS_WAIT_CNT);
	}

	val = timing->emc_cfg_2;
	val &= ~EMC_CFG_2_DIS_STP_OB_CLK_DURING_NON_WR;
	emc_ccfifo_writel(tegra, val, EMC_CFG_2);

	/* DDR3: Turn off DLL and enter self-refresh */

	if (tegra->dram_type == DRAM_TYPE_DDR3 && dll_change == DLL_CHANGE_OFF)
		emc_ccfifo_writel(tegra, timing->emc_mode_1, EMC_EMRS);

	/* Disable refresh controller */

	emc_ccfifo_writel(tegra, EMC_REFCTRL_DEV_SEL(tegra->dram_num),
			  EMC_REFCTRL);
	if (tegra->dram_type == DRAM_TYPE_DDR3)
		emc_ccfifo_writel(tegra,
				  EMC_DRAM_DEV_SEL(tegra->dram_num)
					| EMC_SELF_REF_CMD_ENABLED,
				  EMC_SELF_REF);

	/* Flow control marker */

	emc_ccfifo_writel(tegra, 1, EMC_STALL_THEN_EXE_AFTER_CLKCHANGE);

	/* DDR3: Exit self-refresh */

	if (tegra->dram_type == DRAM_TYPE_DDR3)
		emc_ccfifo_writel(tegra,
				  EMC_DRAM_DEV_SEL(tegra->dram_num),
				  EMC_SELF_REF);
	emc_ccfifo_writel(tegra,
			  EMC_REFCTRL_DEV_SEL(tegra->dram_num)
				| EMC_REFCTRL_ENABLE,
			  EMC_REFCTRL);

	/* Set DRAM mode registers */

	if (tegra->dram_type == DRAM_TYPE_DDR3) {
		if (timing->emc_mode_1 != tegra->last_timing.emc_mode_1)
			emc_ccfifo_writel(tegra, timing->emc_mode_1, EMC_EMRS);
		if (timing->emc_mode_2 != tegra->last_timing.emc_mode_2)
			emc_ccfifo_writel(tegra, timing->emc_mode_2, EMC_EMRS2);

		if ((timing->emc_mode_reset !=
		     tegra->last_timing.emc_mode_reset) ||
		    dll_change == DLL_CHANGE_ON) {
			val = timing->emc_mode_reset;
			if (dll_change == DLL_CHANGE_ON) {
				val |= EMC_MODE_SET_DLL_RESET;
				val |= EMC_MODE_SET_LONG_CNT;
			} else {
				val &= ~EMC_MODE_SET_DLL_RESET;
			}
			emc_ccfifo_writel(tegra, val, EMC_MRS);
		}
	} else {
		if (timing->emc_mode_2 != tegra->last_timing.emc_mode_2)
			emc_ccfifo_writel(tegra, timing->emc_mode_2, EMC_MRW2);
		if (timing->emc_mode_1 != tegra->last_timing.emc_mode_1)
			emc_ccfifo_writel(tegra, timing->emc_mode_1, EMC_MRW);
		if (timing->emc_mode_4 != tegra->last_timing.emc_mode_4)
			emc_ccfifo_writel(tegra, timing->emc_mode_4, EMC_MRW4);
	}

	/*  Issue ZCAL command if turning ZCAL on */

	if (timing->emc_zcal_interval != 0 &&
	    tegra->last_timing.emc_zcal_interval == 0) {
		emc_ccfifo_writel(tegra, EMC_ZQ_CAL_LONG_CMD_DEV0, EMC_ZQ_CAL);
		if (tegra->dram_num > 1)
			emc_ccfifo_writel(tegra, EMC_ZQ_CAL_LONG_CMD_DEV1,
					  EMC_ZQ_CAL);
	}

	/*  Write to RO register to remove stall after change */

	emc_ccfifo_writel(tegra, 0, EMC_CCFIFO_STATUS);

	if (timing->emc_cfg_2 & EMC_CFG_2_DIS_STP_OB_CLK_DURING_NON_WR)
		emc_ccfifo_writel(tegra, timing->emc_cfg_2, EMC_CFG_2);

	/* Disable AUTO_CAL for clock change */

	emc_seq_disable_auto_cal(tegra);

	/* Read MC register to wait until programming has settled */

	//readl(tegra->mc_regs + MC_EMEM_ADR_CFG); likely unnecessary // FIXME
	{
	u8 tmp;
	tegra_mc_get_emem_device_count(&tmp);
	}
	//since we are not doing any writes to mc
	readl(tegra->emc_regs + EMC_INTSTATUS);

	/* Program new parent and divisor. This triggers the EMC state machine
	 * to change timings.
	 */

	writel(car_value, tegra->clk_regs + CLK_SOURCE_EMC);
	readl(tegra->clk_regs + CLK_SOURCE_EMC);

	/* Wait until the state machine has settled */

	emc_seq_wait_clkchange(tegra);

	/* Restore AUTO_CAL */

	if (timing->emc_ctt_term_ctrl != tegra->last_timing.emc_ctt_term_ctrl)
		writel(timing->emc_auto_cal_interval,
		       tegra->emc_regs + EMC_AUTO_CAL_INTERVAL);

	/* Restore dynamic self-refresh */

	if (timing->emc_cfg & EMC_CFG_PWR_MASK)
		writel(timing->emc_cfg, tegra->emc_regs + EMC_CFG);

	/* Set ZCAL wait count */

	writel(timing->emc_zcal_cnt_long, tegra->emc_regs + EMC_ZCAL_WAIT_CNT);

	/* LPDDR3: Turn off BGBIAS if low frequency */

	if (tegra->dram_type == DRAM_TYPE_LPDDR3 &&
	    timing->emc_bgbias_ctl0 &
	      EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_RX) {
		val = timing->emc_bgbias_ctl0;
		val |= EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD_IBIAS_VTTGEN;
		val |= EMC_BGBIAS_CTL0_BIAS0_DSC_E_PWRD;
		writel(val, tegra->emc_regs + EMC_BGBIAS_CTL0);
	} else {
		if (tegra->dram_type == DRAM_TYPE_DDR3 &&
		    readl(tegra->emc_regs + EMC_BGBIAS_CTL0) !=
		      timing->emc_bgbias_ctl0) {
			writel(timing->emc_bgbias_ctl0,
			       tegra->emc_regs + EMC_BGBIAS_CTL0);
		}

		writel(timing->emc_auto_cal_interval,
		       tegra->emc_regs + EMC_AUTO_CAL_INTERVAL);
	}

	/* Wait for timing to settle */

	udelay(2);

	/* Reprogram SEL_DPD_CTRL */

	writel(timing->emc_sel_dpd_ctrl, tegra->emc_regs + EMC_SEL_DPD_CTRL);
	emc_seq_update_timing(tegra);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * *
 * Common clock framework callback implementations *
 * * * * * * * * * * * * * * * * * * * * * * * * * */

unsigned long emc_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct tegra_emc *tegra = container_of(hw, struct tegra_emc, hw);
	u32 val;
	u32 div;

	/* CCF wrongly assumes that the parent won't change during set_rate,
	 * so get the parent rate explicitly.
	 */
	parent_rate = __clk_get_rate(__clk_get_parent(hw->clk));

	val = readl(tegra->clk_regs + CLK_SOURCE_EMC);
	div = val & CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_MASK;

	return parent_rate / (div + 2) * 2;
}

/* Rounds up unless no higher rate exists, in which case down. This way is
 * safer since things have EMC rate floors. Also don't touch parent_rate
 * since we don't want the CCF to play with our parent clocks.
 */
long emc_round_rate(struct clk_hw *hw, unsigned long rate,
		    unsigned long *parent_rate)
{
	struct tegra_emc *tegra = container_of(hw, struct tegra_emc, hw);
	int i;

	/* Returning the original rate will lead to a more sensible error
	 * message when emc_set_rate fails.
	 */
	if (tegra->num_timings == 0)
		return rate;

	for (i = 0; i < tegra->num_timings; ++i) {
		struct emc_timing *timing = tegra->timings + i;

		if (timing->rate >= rate)
			return timing->rate;
	}

	return tegra->timings[tegra->num_timings - 1].rate;
}

u8 emc_get_parent(struct clk_hw *hw)
{
	struct tegra_emc *tegra = container_of(hw, struct tegra_emc, hw);
	u32 val;

	val = readl(tegra->clk_regs + CLK_SOURCE_EMC);

	return (val >> CLK_SOURCE_EMC_EMC_2X_CLK_SRC_SHIFT)
		& CLK_SOURCE_EMC_EMC_2X_CLK_SRC_MASK;
}

static int emc_set_timing(struct tegra_emc *tegra, struct emc_timing *timing)
{
	int err;
	u8 div;
	u32 car_value;
	unsigned long parent_rate;

	dev_dbg(&tegra->pdev->dev, "going to rate %ld prate %ld p %s\n",
		timing->rate, timing->parent_rate,
		__clk_get_name(timing->parent));

	if (emc_get_parent(&tegra->hw) == timing->parent_index &&
	    clk_get_rate(timing->parent) != timing->parent_rate) {
		BUG();
		return -EINVAL;
	}

	tegra->changing_timing = true;

	parent_rate = timing->parent_rate;
	div = parent_rate / (timing->rate / 2) - 2;

	car_value = 0;
	car_value |= timing->parent_index <<
		CLK_SOURCE_EMC_EMC_2X_CLK_SRC_SHIFT;
	car_value |= div << CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_SHIFT;

	err = clk_set_rate(timing->parent, timing->parent_rate);
	if (err) {
		dev_err(&tegra->pdev->dev,
			"cannot change parent %s rate to %ld: %d\n",
			__clk_get_name(timing->parent),
			timing->parent_rate, err);

		return err;
	}

	err = clk_prepare_enable(timing->parent);
	if (err) {
		dev_err(&tegra->pdev->dev,
			"cannot enable parent clock: %d\n", err);
		return err;
	}

	emc_change_timing(tegra, timing, car_value);

	__clk_reparent(tegra->hw.clk, timing->parent);
	clk_disable_unprepare(tegra->prev_parent);

	tegra->last_timing = *timing;
	tegra->prev_parent = timing->parent;
	tegra->changing_timing = false;

	return 0;
}

/* Get backup timing to use as an intermediate step when a change between
 * two timings with the same clock source has been requested. First try to
 * find a timing with a higher clock rate to avoid a rate below any set rate
 * floors. If that is not possible, find a lower rate.
 */
static struct emc_timing *get_backup_timing(struct tegra_emc *tegra,
					    int timing_index)
{
	int i;

	for (i = timing_index+1; i < tegra->num_timings; ++i) {
		struct emc_timing *timing = tegra->timings + i;

		if (emc_parent_clk_sources[timing->parent_index] !=
		    emc_parent_clk_sources[
		      tegra->timings[timing_index].parent_index])
			return timing;
	}

	for (i = timing_index-1; i >= 0; --i) {
		struct emc_timing *timing = tegra->timings + i;

		if (emc_parent_clk_sources[timing->parent_index] !=
		    emc_parent_clk_sources[
		      tegra->timings[timing_index].parent_index])
			return timing;
	}

	return NULL;
}

int emc_set_rate(struct clk_hw *hw, unsigned long rate,
		 unsigned long parent_rate)
{
	struct tegra_emc *tegra = container_of(hw, struct tegra_emc, hw);
	struct emc_timing *timing = NULL;
	int i, err;

	/* When emc_set_timing changes the parent rate, CCF will propagate
	 * that downward to us, so ignore any set_rate calls while a rate
	 * change is already going on.
	 */
	if (tegra->changing_timing)
		return 0;

	for (i = 0; i < tegra->num_timings; ++i) {
		if (tegra->timings[i].rate == rate) {
			timing = tegra->timings + i;
			break;
		}
	}

	if (!timing) {
		dev_err(&tegra->pdev->dev,
			"cannot switch to rate %ld without emc table\n", rate);
		return -EINVAL;
	}

	if (emc_parent_clk_sources[emc_get_parent(hw)] ==
	    emc_parent_clk_sources[timing->parent_index] &&
	    clk_get_rate(timing->parent) != timing->parent_rate) {
		/* Parent clock source not changed but parent rate has changed,
		 * need to temporarily switch to another parent
		 */

		struct emc_timing *backup_timing;

		backup_timing = get_backup_timing(tegra, i);
		if (!backup_timing) {
			dev_err(&tegra->pdev->dev,
				"cannot find backup timing\n");
			return -EINVAL;
		}

		dev_dbg(&tegra->pdev->dev,
			"using %ld as backup rate when going to %ld\n",
			backup_timing->rate, rate);

		err = emc_set_timing(tegra, backup_timing);
		if (err) {
			dev_err(&tegra->pdev->dev,
				"cannot set backup timing: %d\n",
				err);
			return err;
		}
	}

	return emc_set_timing(tegra, timing);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * *
 * Debugfs entry                                   *
 * * * * * * * * * * * * * * * * * * * * * * * * * */

static int emc_debug_rate_get(void *data, u64 *rate)
{
	struct tegra_emc *tegra = data;

	*rate = clk_get_rate(tegra->hw.clk);

	return 0;
}

static int emc_debug_rate_set(void *data, u64 rate)
{
	struct tegra_emc *tegra = data;

	return clk_set_rate(tegra->hw.clk, rate);
}

DEFINE_SIMPLE_ATTRIBUTE(emc_debug_rate_fops, emc_debug_rate_get,
			emc_debug_rate_set, "%lld\n");

const struct clk_ops tegra_clk_emc_ops = {
	.recalc_rate = emc_recalc_rate,
	.round_rate = emc_round_rate,
	.set_rate = emc_set_rate,
	.get_parent = emc_get_parent,
};

void emc_debugfs_init(struct tegra_emc *tegra)
{
	struct dentry *d;

	d = debugfs_create_file("emc_rate", S_IRUGO | S_IWUSR, NULL, tegra,
				&emc_debug_rate_fops);
	if (!d)
		dev_warn(&tegra->pdev->dev,
			 "failed to create debugfs entries\n");
}

/* * * * * * * * * * * * * * * * * * * * * * * * * *
 * Initialization and deinitialization             *
 * * * * * * * * * * * * * * * * * * * * * * * * * */

static void emc_read_current_timing(struct tegra_emc *tegra,
				    struct emc_timing *timing)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(t124_emc_burst_regs); ++i)
		timing->emc_burst_data[i] =
			readl(tegra->emc_regs + t124_emc_burst_regs[i]);

	timing->rate = clk_get_rate(tegra->hw.clk);
	timing->emc_cfg = readl(tegra->emc_regs + EMC_CFG);

	timing->emc_auto_cal_interval = 0;
	timing->emc_zcal_cnt_long = 0;
	timing->emc_mode_1 = 0;
	timing->emc_mode_2 = 0;
	timing->emc_mode_4 = 0;
	timing->emc_mode_reset = 0;
}

static int emc_init(struct tegra_emc *tegra)
{
	int err;

	tegra->dram_type = readl(tegra->emc_regs + EMC_FBIO_CFG5)
		& EMC_FBIO_CFG5_DRAM_TYPE_MASK >> EMC_FBIO_CFG5_DRAM_TYPE_SHIFT;
	err = tegra_mc_get_emem_device_count(&tegra->dram_num);
	if (err)
		return err;

	emc_read_current_timing(tegra, &tegra->last_timing);

	tegra->prev_parent = clk_get_parent_by_index(
		tegra->hw.clk, emc_get_parent(&tegra->hw));
	tegra->changing_timing = false;

	return 0;
}

static int load_one_timing_from_dt(struct tegra_emc *tegra,
				   struct emc_timing *timing,
				   struct device_node *node)
{
	int err, i;
	u32 tmp;

	err = of_property_read_u32(node, "clock-frequency", &tmp);
	if (err) {
		dev_err(&tegra->pdev->dev,
			"timing %s: failed to read rate\n", node->name);
		return err;
	}

	timing->rate = tmp;

	err = of_property_read_u32(node, "nvidia,parent-clock-frequency", &tmp);
	if (err) {
		dev_err(&tegra->pdev->dev,
			"timing %s: failed to read parent rate\n", node->name);
		return err;
	}

	timing->parent_rate = tmp;

	err = of_property_read_u32_array(node, "nvidia,emc-configuration",
					 timing->emc_burst_data,
					 ARRAY_SIZE(timing->emc_burst_data));
	if (err) {
		dev_err(&tegra->pdev->dev,
			"timing %s: failed to read emc burst data\n",
			node->name);
		return err;
	}

#define EMC_READ_PROP(prop, dtprop) { \
	err = of_property_read_u32(node, dtprop, &timing->prop); \
	if (err) { \
		dev_err(&tegra->pdev->dev, \
			"timing %s: failed to read " #prop "\n", \
			node->name); \
		return err; \
	} \
}

	EMC_READ_PROP(emc_zcal_cnt_long, "nvidia,emc-zcal-cnt-long")
	EMC_READ_PROP(emc_auto_cal_interval, "nvidia,emc-auto-cal-interval")
	EMC_READ_PROP(emc_ctt_term_ctrl, "nvidia,emc-ctt-term-ctrl")
	EMC_READ_PROP(emc_cfg, "nvidia,emc-cfg")
	EMC_READ_PROP(emc_cfg_2, "nvidia,emc-cfg-2")
	EMC_READ_PROP(emc_sel_dpd_ctrl, "nvidia,emc-sel-dpd-ctrl")
	EMC_READ_PROP(emc_bgbias_ctl0, "nvidia,emc-bgbias-ctl0")
	EMC_READ_PROP(emc_auto_cal_config2, "nvidia,emc-auto-cal-config2")
	EMC_READ_PROP(emc_auto_cal_config3, "nvidia,emc-auto-cal-config3")
	EMC_READ_PROP(emc_auto_cal_config, "nvidia,emc-auto-cal-config")
	EMC_READ_PROP(emc_mode_reset, "nvidia,emc-mode-reset")
	EMC_READ_PROP(emc_mode_1, "nvidia,emc-mode-1")
	EMC_READ_PROP(emc_mode_2, "nvidia,emc-mode-2")
	EMC_READ_PROP(emc_mode_4, "nvidia,emc-mode-4")

#undef EMC_READ_PROP

	timing->parent = of_clk_get_by_name(node, "emc-parent");
	if (IS_ERR(timing->parent)) {
		dev_err(&tegra->pdev->dev,
			"timing %s: failed to get parent clock\n",
			node->name);
		return PTR_ERR(timing->parent);
	}

	timing->parent_index = 0xff;
	for (i = 0; i < ARRAY_SIZE(emc_parent_clk_names); ++i) {
		if (!strcmp(emc_parent_clk_names[i],
			    __clk_get_name(timing->parent))) {
			timing->parent_index = i;
			break;
		}
	}
	if (timing->parent_index == 0xff) {
		dev_err(&tegra->pdev->dev,
			"timing %s: %s is not a valid parent\n",
			node->name, __clk_get_name(timing->parent));
		clk_put(timing->parent);
		return -EINVAL;
	}

	return 0;
}

static int cmp_timings(const void *_a, const void *_b) {
	const struct emc_timing *a = _a;
	const struct emc_timing *b = _b;

	if (a->rate < b->rate)
		return -1;
	else if (a->rate == b->rate)
		return 0;
	else
		return 1;
}

static int load_timings_from_dt(struct tegra_emc *tegra,
				struct device_node *node)
{
	struct device_node *child;
	int child_count = of_get_child_count(node);
	int i = 0, err;

	tegra->timings = devm_kzalloc(&tegra->pdev->dev,
				      sizeof(struct emc_timing) * child_count,
				      GFP_KERNEL);
	if (!tegra->timings)
		return -ENOMEM;

	tegra->num_timings = child_count;

	for_each_child_of_node(node, child) {
		struct emc_timing *timing = tegra->timings + (i++);

		err = load_one_timing_from_dt(tegra, timing, child);
		if (err)
			return err;
	}

	sort(tegra->timings, tegra->num_timings, sizeof(struct emc_timing),
	     cmp_timings, NULL);

	return 0;
}

static void unload_timings(struct tegra_emc *tegra)
{
	int i;

	for (i = 0; i < tegra->num_timings; ++i)
		clk_put(tegra->timings[i].parent);
}

static int tegra_emc_remove(struct platform_device *pdev)
{
	struct tegra_emc *tegra = platform_get_drvdata(pdev);

	unload_timings(tegra);

	return 0;
}

static const struct of_device_id tegra_emc_of_match[] = {
	{ .compatible = "nvidia,tegra124-emc" },
	{}
};
MODULE_DEVICE_TABLE(of, tegra_emc_of_match);

static const struct of_device_id tegra_car_of_match[] = {
	{ .compatible = "nvidia,tegra124-car" },
	{}
};

static int tegra_emc_probe(struct platform_device *pdev)
{
	struct tegra_emc *tegra;
	struct clk *clk;
	struct clk_init_data init;
	struct device_node *node;
	struct resource node_res;
	struct resource *res;
	u32 ram_code, node_ram_code;
	int err;

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	tegra->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	tegra->emc_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tegra->emc_regs)) {
		dev_err(&pdev->dev, "failed to map EMC regs\n");
		return PTR_ERR(tegra->emc_regs);
	}

	node = of_find_matching_node(NULL, tegra_car_of_match);
	err = of_address_to_resource(node, 0, &node_res);
	if (err)
		dev_err(&pdev->dev, "failed to get CAR registers\n");

	tegra->clk_regs = devm_ioremap(&pdev->dev, node_res.start,
				       resource_size(&node_res));
	if (!tegra->clk_regs) {
		dev_err(&pdev->dev, "could not map CAR registers\n");
		return -ENOMEM;
	}

	node = of_parse_phandle(pdev->dev.of_node,
				"nvidia,memory-controller", 0);
	if (!node) {
		dev_err(&pdev->dev, "could not get memory controller\n");
		return -ENOENT;
	}
	of_node_put(node);

	ram_code = tegra_read_ram_code();

	tegra->num_timings = 0;

	for_each_child_of_node(pdev->dev.of_node, node) {
		if (strcmp(node->name, "timings"))
			continue;

		err = of_property_read_u32(node, "nvidia,ram-code",
						&node_ram_code);
		if (err) {
			dev_warn(&pdev->dev,
				 "skipping timing without ram-code\n");
			continue;
		}

		if (node_ram_code != ram_code)
			continue;

		err = load_timings_from_dt(tegra, node);
		if (err)
			return err;
		break;
	}

	if (tegra->num_timings == 0)
		dev_warn(&pdev->dev, "no memory timings registered\n");

	init.name = "emc";
	init.ops = &tegra_clk_emc_ops;
	init.flags = 0;
	init.parent_names = emc_parent_clk_names;
	init.num_parents = ARRAY_SIZE(emc_parent_clk_names);

	tegra->hw.init = &init;

	clk = devm_clk_register(&pdev->dev, &tegra->hw);
	if (IS_ERR(clk)) {
		unload_timings(tegra);
		return PTR_ERR(clk);
	}

	err = emc_init(tegra);
	if (err) {
		dev_err(&pdev->dev, "initialization failed: %d\n", err);
		return err;
	}

	emc_debugfs_init(tegra);

	/* Allow debugging tools to see the EMC clock */
	clk_register_clkdev(clk, "emc", "tegra-clk-debug");

	clk_prepare_enable(clk);

	platform_set_drvdata(pdev, tegra);

	return 0;
};

static struct platform_driver tegra_emc_driver = {
	.probe = tegra_emc_probe,
	.remove = tegra_emc_remove,
	.driver = {
		.name = "tegra-emc",
		.of_match_table = tegra_emc_of_match,
	},
};
module_platform_driver(tegra_emc_driver);

MODULE_AUTHOR("Mikko Perttunen <mperttunen@nvidia.com>");
MODULE_DESCRIPTION("Tegra124 EMC driver");
MODULE_LICENSE("GPL v2");
