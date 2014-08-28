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
#include <soc/tegra/mc.h>

#define CLK_SOURCE_EMC 0x19c
#define CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_SHIFT 0
#define CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_MASK 0xff
#define CLK_SOURCE_EMC_EMC_2X_CLK_SRC_SHIFT 29
#define CLK_SOURCE_EMC_EMC_2X_CLK_SRC_MASK 0x7

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
	u32 ram_code;
};

struct tegra_emc {
	struct clk_hw hw;
	void __iomem *clk_regs;
	struct clk *prev_parent;
	bool changing_timing;

	int num_timings;
	struct emc_timing *timings;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * *
 * Common clock framework callback implementations *
 * * * * * * * * * * * * * * * * * * * * * * * * * */

unsigned long emc_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct tegra_emc *tegra = container_of(hw, struct tegra_emc, hw);
	u32 val, div;

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
	u8 ram_code = tegra_read_ram_code();
	int i;

	/* Returning the original rate will lead to a more sensible error
	 * message when emc_set_rate fails.
	 */
	if (tegra->num_timings == 0)
		return rate;

	for (i = 0; i < tegra->num_timings; ++i) {
		struct emc_timing *timing = tegra->timings + i;
		if (timing->ram_code != ram_code)
			continue;

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

void tegra_emc_prepare_timing_change(unsigned long rate);
void tegra_emc_complete_timing_change(unsigned long rate);

static int emc_set_timing(struct tegra_emc *tegra, struct emc_timing *timing)
{
	int err;
	u8 div;
	u32 car_value;

	pr_debug("going to rate %ld prate %ld p %s\n",
		timing->rate, timing->parent_rate,
		__clk_get_name(timing->parent));

	if (emc_get_parent(&tegra->hw) == timing->parent_index &&
	    clk_get_rate(timing->parent) != timing->parent_rate) {
		BUG();
		return -EINVAL;
	}

	tegra->changing_timing = true;

	err = clk_set_rate(timing->parent, timing->parent_rate);
	if (err) {
		pr_err("cannot change parent %s rate to %ld: %d\n",
		       __clk_get_name(timing->parent),
		       timing->parent_rate, err);

		return err;
	}

	err = clk_prepare_enable(timing->parent);
	if (err) {
		pr_err("cannot enable parent clock: %d\n", err);
		return err;
	}

	div = timing->parent_rate / (timing->rate / 2) - 2;

	car_value = 0;
	car_value |= timing->parent_index <<
		CLK_SOURCE_EMC_EMC_2X_CLK_SRC_SHIFT;
	car_value |= div << CLK_SOURCE_EMC_EMC_2X_CLK_DIVISOR_SHIFT;

	tegra_emc_prepare_timing_change(timing->rate);

	writel(car_value, tegra->clk_regs + CLK_SOURCE_EMC);

	tegra_emc_complete_timing_change(timing->rate);

	__clk_reparent(tegra->hw.clk, timing->parent);
	clk_disable_unprepare(tegra->prev_parent);

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
	u32 ram_code = tegra_read_ram_code();

	for (i = timing_index+1; i < tegra->num_timings; ++i) {
		struct emc_timing *timing = tegra->timings + i;
		if (timing->ram_code != ram_code)
			continue;

		if (emc_parent_clk_sources[timing->parent_index] !=
		    emc_parent_clk_sources[
		      tegra->timings[timing_index].parent_index])
			return timing;
	}

	for (i = timing_index-1; i >= 0; --i) {
		struct emc_timing *timing = tegra->timings + i;
		if (timing->ram_code != ram_code)
			continue;

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
	u32 ram_code = tegra_read_ram_code();

	/* When emc_set_timing changes the parent rate, CCF will propagate
	 * that downward to us, so ignore any set_rate calls while a rate
	 * change is already going on.
	 */
	if (tegra->changing_timing)
		return 0;

	for (i = 0; i < tegra->num_timings; ++i) {
		if (tegra->timings[i].rate == rate &&
		    tegra->timings[i].ram_code == ram_code) {
			timing = tegra->timings + i;
			break;
		}
	}

	if (!timing) {
		pr_err("cannot switch to rate %ld without emc table\n", rate);
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
			pr_err("cannot find backup timing\n");
			return -EINVAL;
		}

		pr_debug("using %ld as backup rate when going to %ld\n",
			 backup_timing->rate, rate);

		err = emc_set_timing(tegra, backup_timing);
		if (err) {
			pr_err("cannot set backup timing: %d\n", err);
			return err;
		}
	}

	return emc_set_timing(tegra, timing);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * *
 * Initialization and deinitialization             *
 * * * * * * * * * * * * * * * * * * * * * * * * * */

static int load_one_timing_from_dt(struct tegra_emc *tegra,
				   struct emc_timing *timing,
				   struct device_node *node)
{
	int err, i;
	u32 tmp;

	err = of_property_read_u32(node, "clock-frequency", &tmp);
	if (err) {
		pr_err("timing %s: failed to read rate\n", node->full_name);
		return err;
	}

	timing->rate = tmp;

	err = of_property_read_u32(node, "nvidia,parent-clock-frequency", &tmp);
	if (err) {
		pr_err("timing %s: failed to read parent rate\n",
		       node->full_name);
		return err;
	}

	timing->parent_rate = tmp;

	timing->parent = of_clk_get_by_name(node, "emc-parent");
	if (IS_ERR(timing->parent)) {
		pr_err("timing %s: failed to get parent clock\n",
		       node->full_name);
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
		pr_err("timing %s: %s is not a valid parent\n",
		       node->full_name, __clk_get_name(timing->parent));
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
				struct device_node *node,
				u32 ram_code)
{
	struct device_node *child;
	int child_count = of_get_child_count(node);
	int i = 0, err;

	tegra->timings = kcalloc(child_count, sizeof(struct emc_timing),
				 GFP_KERNEL);
	if (!tegra->timings)
		return -ENOMEM;

	tegra->num_timings = child_count;

	for_each_child_of_node(node, child) {
		struct emc_timing *timing = tegra->timings + (i++);

		err = load_one_timing_from_dt(tegra, timing, child);
		if (err)
			return err;

		timing->ram_code = ram_code;
	}

	sort(tegra->timings, tegra->num_timings, sizeof(struct emc_timing),
	     cmp_timings, NULL);

	return 0;
}

const struct clk_ops tegra_clk_emc_ops = {
	.recalc_rate = emc_recalc_rate,
	.round_rate = emc_round_rate,
	.set_rate = emc_set_rate,
	.get_parent = emc_get_parent,
};

int tegra_emc_init(struct device_node *np, void __iomem *clk_regs)
{
	struct tegra_emc *tegra;
	struct clk *clk;
	struct clk_init_data init;
	struct device_node *node;
	u32 node_ram_code;
	int err;

	tegra = kcalloc(1, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	tegra->clk_regs = clk_regs;

	tegra->num_timings = 0;

	for_each_child_of_node(np, node) {
		if (strcmp(node->name, "emc-timings"))
			continue;

		err = of_property_read_u32(node, "nvidia,ram-code",
						&node_ram_code);
		if (err) {
			pr_warn("skipping timing without ram-code\n");
			continue;
		}

		err = load_timings_from_dt(tegra, node, node_ram_code);
		if (err)
			return err;
		break;
	}

	if (tegra->num_timings == 0)
		pr_warn("no memory timings registered\n");

	init.name = "emc";
	init.ops = &tegra_clk_emc_ops;
	init.flags = 0;
	init.parent_names = emc_parent_clk_names;
	init.num_parents = ARRAY_SIZE(emc_parent_clk_names);

	tegra->hw.init = &init;

	clk = clk_register(NULL, &tegra->hw);

	tegra->prev_parent = clk_get_parent_by_index(
		tegra->hw.clk, emc_get_parent(&tegra->hw));
	tegra->changing_timing = false;

	/* Allow debugging tools to see the EMC clock */
	clk_register_clkdev(clk, "emc", "tegra-clk-debug");

	clk_prepare_enable(clk);

	return 0;
};
