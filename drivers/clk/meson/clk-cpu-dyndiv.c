// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2019 BayLibre, SAS.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/arm-smccc.h>
#include <linux/module.h>

#include "clk-regmap.h"
#include "clk-cpu-dyndiv.h"

static inline struct meson_clk_cpu_dyndiv_data *
meson_clk_cpu_dyndiv_data(struct clk_regmap *clk)
{
	return (struct meson_clk_cpu_dyndiv_data *)clk->data;
}

static unsigned long meson_clk_cpu_dyndiv_recalc_rate(struct clk_hw *hw,
						      unsigned long prate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyndiv_data *data = meson_clk_cpu_dyndiv_data(clk);

	return divider_recalc_rate(hw, prate,
				   meson_parm_read(clk->map, &data->div),
				   NULL, 0, data->div.width);
}

static int meson_clk_cpu_dyndiv_determine_rate(struct clk_hw *hw,
					       struct clk_rate_request *req)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyndiv_data *data = meson_clk_cpu_dyndiv_data(clk);

	return divider_determine_rate(hw, req, NULL, data->div.width, 0);
}

static int meson_clk_cpu_dyndiv_set_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long parent_rate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyndiv_data *data = meson_clk_cpu_dyndiv_data(clk);
	unsigned int val;
	int ret;

	ret = divider_get_val(rate, parent_rate, NULL, data->div.width, 0);
	if (ret < 0)
		return ret;

	val = (unsigned int)ret << data->div.shift;

	/* Write the SYS_CPU_DYN_ENABLE bit before changing the divider */
	meson_parm_write(clk->map, &data->dyn, 1);

	/* Update the divider while removing the SYS_CPU_DYN_ENABLE bit */
	return regmap_update_bits(clk->map, data->div.reg_off,
				  SETPMASK(data->div.width, data->div.shift) |
				  SETPMASK(data->dyn.width, data->dyn.shift),
				  val);
};

const struct clk_ops meson_clk_cpu_dyndiv_ops = {
	.init = clk_regmap_init,
	.recalc_rate = meson_clk_cpu_dyndiv_recalc_rate,
	.determine_rate = meson_clk_cpu_dyndiv_determine_rate,
	.set_rate = meson_clk_cpu_dyndiv_set_rate,
};
EXPORT_SYMBOL_NS_GPL(meson_clk_cpu_dyndiv_ops, "CLK_MESON");

MODULE_DESCRIPTION("Amlogic CPU Dynamic Clock divider");
MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_MESON");

#define CPU_DYN_SEL_MASK	BIT(10)
#define SYS_CLK_SEL_MASK	BIT(15)

static inline struct meson_clk_cpu_dyn_data *
meson_clk_cpu_dyn_data(struct clk_regmap *clk)
{
	return (struct meson_clk_cpu_dyn_data *)clk->data;
}

static unsigned long meson_clk_cpu_dyn_recalc_rate(struct clk_hw *hw,
						   unsigned long prate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	struct arm_smccc_res res;
	unsigned int val, pindex, div;
	unsigned long parent;

	if (data->smc_id) {
		arm_smccc_smc(data->smc_id, data->secid_dyn_rd,
			      0, 0, 0, 0, 0, 0, &res);
		val = res.a0;
	} else {
		regmap_read(clk->map, data->offset, &val);
	}

	if (val & CPU_DYN_SEL_MASK) {
		pindex = (val >> 16) & 0x3;
		parent = clk_hw_get_rate(clk_hw_get_parent_by_index(hw, pindex));
		if ((val >> 18) & 0x1) {
			div = (val >> 20) & 0x3f;
			return DIV_ROUND_UP_ULL((u64)parent, div + 1);
		}
		return parent;
	}

	pindex = val & 0x3;
	parent = clk_hw_get_rate(clk_hw_get_parent_by_index(hw, pindex));
	if ((val >> 2) & 0x1) {
		div = (val >> 4) & 0x3f;
		return DIV_ROUND_UP_ULL((u64)parent, div + 1);
	}

	return parent;
}

static long meson_clk_cpu_dyn_round_rate(struct clk_hw *hw,
					 unsigned long rate,
					 unsigned long *prate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	const struct cpu_dyn_table *table = data->table;
	unsigned long min, max;
	unsigned int i;

	if (!table || !data->table_cnt)
		return rate;

	min = table[0].rate;
	max = table[data->table_cnt - 1].rate;

	if (rate < min)
		return min;
	if (rate > max)
		return max;

	for (i = 0; i < data->table_cnt; i++) {
		if (rate <= table[i].rate)
			return table[i].rate;
	}

	return min;
}

static int meson_cpu_dyn_set(struct clk_hw *hw, u16 dyn_pre_mux,
			     u16 dyn_post_mux, u16 dyn_div)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	unsigned int control;
	unsigned int cnt = 0;

	do {
		regmap_read(clk->map, data->offset, &control);
		udelay(1);
		cnt++;
		if (cnt > 100)
			break;
	} while (control & BIT(28));

	control |= BIT(26);

	if (control & BIT(10)) {
		control = (control & ~(BIT(10) | (0x3f << 4) | BIT(2) | (0x3 << 0))) |
				  ((0 << 10) |
				   (dyn_div << 4) |
				   (dyn_post_mux << 2) |
				   (dyn_pre_mux << 0));
	} else {
		control = (control & ~(BIT(10) | (0x3f << 20) | BIT(18) | (0x3 << 16))) |
				  (BIT(10) |
				   (dyn_div << 20) |
				   (dyn_post_mux << 18) |
				   (dyn_pre_mux << 16));
	}

	regmap_write(clk->map, data->offset, control);

	return 0;
}

static int meson_clk_cpu_dyn_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	const struct cpu_dyn_table *table = data->table;
	struct arm_smccc_res res;
	unsigned long nrate;
	unsigned int i;

	if (!table || !data->table_cnt)
		return -EINVAL;

	for (i = 0; i < data->table_cnt; i++) {
		if (rate <= table[i].rate)
			break;
	}

	if (i >= data->table_cnt)
		i = data->table_cnt - 1;

	nrate = table[i].rate;

	if (nrate > 1000000000 && !strcmp(clk_hw_get_name(hw), "dsu_dyn_clk") &&
	    clk_hw_get_num_parents(hw) > 3) {
		if (clk_get_rate(hw->clk) > 1000000000) {
			if (data->smc_id)
				arm_smccc_smc(data->smc_id, data->secid_dyn,
					      1, 0, 0, 0, 0, 0, &res);
			else
				meson_cpu_dyn_set(hw, 1, 0, 0);
		}
		clk_set_rate(clk_hw_get_parent_by_index(hw, 3)->clk, nrate);
	}

	if (data->smc_id)
		arm_smccc_smc(data->smc_id, data->secid_dyn,
			      table[i].dyn_pre_mux, table[i].dyn_post_mux,
			      table[i].dyn_div, 0, 0, 0, &res);
	else
		meson_cpu_dyn_set(hw, table[i].dyn_pre_mux,
				  table[i].dyn_post_mux, table[i].dyn_div);

	return 0;
}

static u8 meson_clk_cpu_dyn_get_parent(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	struct arm_smccc_res res;
	u32 pre_shift, val;

	if (data->smc_id) {
		arm_smccc_smc(data->smc_id, data->secid_dyn_rd,
			      0, 0, 0, 0, 0, 0, &res);
		val = res.a0;
	} else {
		regmap_read(clk->map, data->offset, &val);
	}

	pre_shift = (val & CPU_DYN_SEL_MASK) ? 16 : 0;
	val = (val >> pre_shift) & 0x3;

	if (val >= clk_hw_get_num_parents(hw))
		return -EINVAL;

	return val;
}

const struct clk_ops meson_clk_cpu_dyn_ops = {
	.recalc_rate = meson_clk_cpu_dyn_recalc_rate,
	.round_rate = meson_clk_cpu_dyn_round_rate,
	.set_rate = meson_clk_cpu_dyn_set_rate,
	.get_parent = meson_clk_cpu_dyn_get_parent,
};
EXPORT_SYMBOL_NS_GPL(meson_clk_cpu_dyn_ops, "CLK_MESON");

static u8 meson_sec_sys_clk_get_parent(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	struct arm_smccc_res res;
	u32 pre_shift, val;

	arm_smccc_smc(data->smc_id, data->secid_dyn_rd,
		      0, 0, 0, 0, 0, 0, &res);
	val = res.a0;

	pre_shift = (val & SYS_CLK_SEL_MASK) ? 26 : 10;
	val = (val >> pre_shift) & 0x7;

	if (val >= clk_hw_get_num_parents(hw))
		return -EINVAL;

	return val;
}

static unsigned long meson_sec_sys_clk_recalc_rate(struct clk_hw *hw,
						   unsigned long prate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_cpu_dyn_data *data = meson_clk_cpu_dyn_data(clk);
	struct arm_smccc_res res;
	unsigned int val, pindex, div;
	unsigned long parent;

	arm_smccc_smc(data->smc_id, data->secid_dyn_rd,
		      0, 0, 0, 0, 0, 0, &res);
	val = res.a0;

	if (val & SYS_CLK_SEL_MASK) {
		pindex = (val >> 26) & 0x7;
		parent = clk_hw_get_rate(clk_hw_get_parent_by_index(hw, pindex));
		div = (val >> 16) & 0x3ff;
	} else {
		pindex = (val >> 10) & 0x7;
		parent = clk_hw_get_rate(clk_hw_get_parent_by_index(hw, pindex));
		div = val & 0x3ff;
	}

	return DIV_ROUND_UP_ULL((u64)parent, div + 1);
}

const struct clk_ops meson_sec_sys_clk_ops = {
	.recalc_rate = meson_sec_sys_clk_recalc_rate,
	.round_rate = meson_clk_cpu_dyn_round_rate,
	.set_rate = meson_clk_cpu_dyn_set_rate,
	.get_parent = meson_sec_sys_clk_get_parent,
};
EXPORT_SYMBOL_NS_GPL(meson_sec_sys_clk_ops, "CLK_MESON");
