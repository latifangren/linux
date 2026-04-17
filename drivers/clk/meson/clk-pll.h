/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#ifndef __MESON_CLK_PLL_H
#define __MESON_CLK_PLL_H

#include <linux/clk-provider.h>
#include <linux/regmap.h>
#include "parm.h"

struct pll_mult_range {
	unsigned int	min;
	unsigned int	max;
};

struct pll_rate_range {
	unsigned long min_rate;
	unsigned long max_rate;
};

#if defined CONFIG_AMLOGIC_MODIFY && defined CONFIG_ARM
struct pll_params_table {
	u16		m;
	u16		n;
	u16		od;
	const struct	reg_sequence *regs;
	unsigned int	regs_count;
};

#define PLL_PARAMS(_m, _n, _od)						\
	{								\
		.m		= (_m),					\
		.n		= (_n),					\
		.od		= (_od),				\
	}
#else
struct pll_params_table {
	unsigned int	m;
	unsigned int	n;
	u16		od;
	const struct	reg_sequence *regs;
	unsigned int	regs_count;
};

#define PLL_PARAMS(_m, _n)						\
	{								\
		.m		= (_m),					\
		.n		= (_n),					\
		.od		= (0),				\
	}
#endif

#define PLL_PARAMS_v4(_m, _n, _od)						\
	{								\
		.m		= (_m),					\
		.n		= (_n),					\
		.od		= (_od),				\
	}

#define CLK_MESON_PLL_ROUND_CLOSEST			BIT(0)
#define CLK_MESON_PLL_IGNORE_INIT			BIT(1)
#define CLK_MESON_PLL_FIXED_FRAC_WEIGHT_PRECISION	BIT(2)
#define CLK_MESON_PLL_POWER_OF_TWO			BIT(3)
#define CLK_MESON_PLL_FIXED_N				BIT(4)
#define CLK_MESON_PLL_FIXED_EN0P5			BIT(5)
#define CLK_MESON_PLL_READ_ONLY				BIT(6)
#define CLK_MESON_PLL_RSTN				BIT(7)
/* Backward-compatible alias used by older clk-pll.c code paths. */
#define CLK_MESON_PLL_NOINIT_ENABLED			CLK_MESON_PLL_IGNORE_INIT

struct meson_clk_pll_data {
	struct parm en;
	struct parm m;
	struct parm n;
	struct parm frac;
	unsigned int frac_max;
	struct parm l;
	struct parm rst;
	struct parm current_en;
	struct parm l_detect;
	struct parm l_rst;
	struct parm th; /* threshold */
	struct parm fl; /* force lock */
	/* for 32bit dco overflow */
	struct parm od;
	/*for pcie*/
	struct parm pcie_hcsl;
	struct parm pcie_exen;
	const struct reg_sequence *init_regs;
	unsigned int init_count;
	const struct pll_params_table *table;
	const struct pll_mult_range *range;
	struct pll_rate_range pll_range;
	unsigned long od_max;
	unsigned int smc_id;
	u16 flags;
	u8 secid_disable;
	u8 secid;
	u8 fixed_n;
};

extern const struct clk_ops meson_clk_pll_ro_ops;
extern const struct clk_ops meson_clk_pll_ops;
extern const struct clk_ops meson_clk_pcie_pll_ops;
extern const struct clk_ops meson_secure_pll_v2_ops;
extern const struct clk_ops meson_clk_pll_v3_ops;
extern const struct clk_ops meson_clk_pll_v4_ops;

#endif /* __MESON_CLK_PLL_H */
