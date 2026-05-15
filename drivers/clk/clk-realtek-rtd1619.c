// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Realtek RTD1619 clock/reset controller.
 *
 * Provides register-bank/bit based gate clocks and resets for early bring-up.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>

#define RTD1619_MAX_BANKS	8
#define RTD1619_BITS_PER_BANK	32
#define RTD1619_MAX_IDS		(RTD1619_MAX_BANKS * RTD1619_BITS_PER_BANK)

struct rtd1619_cc;

struct rtd1619_gate {
	struct clk_hw hw;
	struct rtd1619_cc *cc;
	u32 bank;
	u32 bit;
};

struct rtd1619_cc {
	struct device *dev;
	void __iomem *base;
	const u32 *clk_offsets;
	unsigned int num_clk_banks;
	const u32 *rst_offsets;
	unsigned int num_rst_banks;
	spinlock_t lock;
	struct clk_hw *hws[RTD1619_MAX_IDS];
	struct reset_controller_dev rcdev;
};

#define to_rtd1619_gate(_hw) container_of(_hw, struct rtd1619_gate, hw)

static int rtd1619_gate_enable(struct clk_hw *hw)
{
	struct rtd1619_gate *gate = to_rtd1619_gate(hw);
	struct rtd1619_cc *cc = gate->cc;
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&cc->lock, flags);
	val = readl(cc->base + cc->clk_offsets[gate->bank]);
	val |= BIT(gate->bit);
	writel(val, cc->base + cc->clk_offsets[gate->bank]);
	spin_unlock_irqrestore(&cc->lock, flags);

	return 0;
}

static void rtd1619_gate_disable(struct clk_hw *hw)
{
	struct rtd1619_gate *gate = to_rtd1619_gate(hw);
	struct rtd1619_cc *cc = gate->cc;
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&cc->lock, flags);
	val = readl(cc->base + cc->clk_offsets[gate->bank]);
	val &= ~BIT(gate->bit);
	writel(val, cc->base + cc->clk_offsets[gate->bank]);
	spin_unlock_irqrestore(&cc->lock, flags);
}

static int rtd1619_gate_is_enabled(struct clk_hw *hw)
{
	struct rtd1619_gate *gate = to_rtd1619_gate(hw);
	struct rtd1619_cc *cc = gate->cc;

	return !!(readl(cc->base + cc->clk_offsets[gate->bank]) & BIT(gate->bit));
}

static const struct clk_ops rtd1619_gate_ops = {
	.enable = rtd1619_gate_enable,
	.disable = rtd1619_gate_disable,
	.is_enabled = rtd1619_gate_is_enabled,
};

static struct clk_hw *rtd1619_clk_get(struct of_phandle_args *clkspec, void *data)
{
	struct rtd1619_cc *cc = data;
	u32 bank, bit, id;

	if (clkspec->args_count != 2)
		return ERR_PTR(-EINVAL);

	bank = clkspec->args[0];
	bit = clkspec->args[1];
	if (bank >= cc->num_clk_banks || bit >= RTD1619_BITS_PER_BANK)
		return ERR_PTR(-EINVAL);

	id = bank * RTD1619_BITS_PER_BANK + bit;

	return cc->hws[id] ?: ERR_PTR(-ENOENT);
}

static int rtd1619_reset_update(struct reset_controller_dev *rcdev,
				u32 id, bool assert)
{
	struct rtd1619_cc *cc = container_of(rcdev, struct rtd1619_cc, rcdev);
	u32 bank = id / RTD1619_BITS_PER_BANK;
	u32 bit = id % RTD1619_BITS_PER_BANK;
	unsigned long flags;
	u32 val;

	if (bank >= cc->num_rst_banks)
		return -EINVAL;

	spin_lock_irqsave(&cc->lock, flags);
	val = readl(cc->base + cc->rst_offsets[bank]);
	if (assert)
		val &= ~BIT(bit);
	else
		val |= BIT(bit);
	writel(val, cc->base + cc->rst_offsets[bank]);
	spin_unlock_irqrestore(&cc->lock, flags);

	return 0;
}

static int rtd1619_reset_assert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	return rtd1619_reset_update(rcdev, id, true);
}

static int rtd1619_reset_deassert(struct reset_controller_dev *rcdev,
				  unsigned long id)
{
	return rtd1619_reset_update(rcdev, id, false);
}

static int rtd1619_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct rtd1619_cc *cc = container_of(rcdev, struct rtd1619_cc, rcdev);
	u32 bank = id / RTD1619_BITS_PER_BANK;
	u32 bit = id % RTD1619_BITS_PER_BANK;

	if (bank >= cc->num_rst_banks)
		return -EINVAL;

	return !(readl(cc->base + cc->rst_offsets[bank]) & BIT(bit));
}

static int rtd1619_reset_of_xlate(struct reset_controller_dev *rcdev,
				  const struct of_phandle_args *reset_spec)
{
	u32 bank, bit;

	if (reset_spec->args_count != 2)
		return -EINVAL;

	bank = reset_spec->args[0];
	bit = reset_spec->args[1];
	if (bank >= RTD1619_MAX_BANKS || bit >= RTD1619_BITS_PER_BANK)
		return -EINVAL;

	return bank * RTD1619_BITS_PER_BANK + bit;
}

static const struct reset_control_ops rtd1619_reset_ops = {
	.assert = rtd1619_reset_assert,
	.deassert = rtd1619_reset_deassert,
	.status = rtd1619_reset_status,
};

static int rtd1619_register_gates(struct rtd1619_cc *cc)
{
	const char *parent = of_clk_get_parent_name(cc->dev->of_node, 0);
	struct clk_init_data init = {};
	unsigned int bank, bit;
	int ret;

	if (!parent)
		parent = "osc27M";

	init.ops = &rtd1619_gate_ops;
	init.parent_names = &parent;
	init.num_parents = 1;
	/* Keep firmware-enabled gates alive until the SoC clock tree is modeled. */
	init.flags = CLK_IGNORE_UNUSED;

	for (bank = 0; bank < cc->num_clk_banks; bank++) {
		for (bit = 0; bit < RTD1619_BITS_PER_BANK; bit++) {
			struct rtd1619_gate *gate;
			char *name;
			u32 id = bank * RTD1619_BITS_PER_BANK + bit;

			gate = devm_kzalloc(cc->dev, sizeof(*gate), GFP_KERNEL);
			if (!gate)
				return -ENOMEM;

			name = devm_kasprintf(cc->dev, GFP_KERNEL,
					    "%s-gate%u-%u", dev_name(cc->dev), bank, bit);
			if (!name)
				return -ENOMEM;

			init.name = name;
			gate->cc = cc;
			gate->bank = bank;
			gate->bit = bit;
			gate->hw.init = &init;

			ret = devm_clk_hw_register(cc->dev, &gate->hw);
			if (ret)
				return ret;

			cc->hws[id] = &gate->hw;
		}
	}

	return 0;
}

static int rtd1619_cc_probe(struct platform_device *pdev)
{
	struct rtd1619_cc *cc;
	struct resource *res;
	int ret;

	cc = devm_kzalloc(&pdev->dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;

	cc->dev = &pdev->dev;
	spin_lock_init(&cc->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOENT;

	/* RTD16xx syscon windows overlap between several early drivers. */
	cc->base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!cc->base)
		return -ENOMEM;

	cc->clk_offsets = device_get_match_data(&pdev->dev);
	if (!cc->clk_offsets)
		return -EINVAL;

	if (of_device_is_compatible(pdev->dev.of_node, "realtek,rtd1619-crt-clk-reset")) {
		static const u32 rst_offsets[] = { 0x00, 0x04, 0x08, 0x0c, 0x14, 0x68 };
		cc->num_clk_banks = 4;
		cc->rst_offsets = rst_offsets;
		cc->num_rst_banks = ARRAY_SIZE(rst_offsets);
	} else {
		static const u32 rst_offsets[] = { 0x88 };
		cc->num_clk_banks = 1;
		cc->rst_offsets = rst_offsets;
		cc->num_rst_banks = ARRAY_SIZE(rst_offsets);
	}

	ret = rtd1619_register_gates(cc);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(&pdev->dev, rtd1619_clk_get, cc);
	if (ret)
		return ret;

	cc->rcdev.owner = THIS_MODULE;
	cc->rcdev.nr_resets = cc->num_rst_banks * RTD1619_BITS_PER_BANK;
	cc->rcdev.ops = &rtd1619_reset_ops;
	cc->rcdev.of_node = pdev->dev.of_node;
	cc->rcdev.of_reset_n_cells = 2;
	cc->rcdev.of_xlate = rtd1619_reset_of_xlate;

	ret = devm_reset_controller_register(&pdev->dev, &cc->rcdev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, cc);

	return 0;
}

static const u32 rtd1619_crt_clk_offsets[] = { 0x50, 0x54, 0x58, 0x5c };
static const u32 rtd1619_iso_clk_offsets[] = { 0x8c };

static const struct of_device_id rtd1619_cc_of_match[] = {
	{ .compatible = "realtek,rtd1619-crt-clk-reset", .data = rtd1619_crt_clk_offsets },
	{ .compatible = "realtek,rtd1619-iso-clk-reset", .data = rtd1619_iso_clk_offsets },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1619_cc_of_match);

static struct platform_driver rtd1619_cc_driver = {
	.probe = rtd1619_cc_probe,
	.driver = {
		.name = "rtd1619-clk-reset",
		.of_match_table = rtd1619_cc_of_match,
	},
};
module_platform_driver(rtd1619_cc_driver);

MODULE_DESCRIPTION("Realtek RTD1619 minimal clock/reset controller");
MODULE_LICENSE("GPL");
