// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTD interrupt mux controller
 */

#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define RTD_IRQS_PER_MUX	32
#define RTD_MAX_MUXES		2
#define RTD_INT_INVALID		0xff
#define RTD_INT_NO_ENABLE	0xfe

struct rtd_irq_mux_data;

struct rtd_irq_mux {
	struct rtd_irq_mux_data *data;
	void __iomem *base;
	unsigned int parent_irq;
	u32 status_offset;
	u32 enable_offset;
	unsigned int irq_offset;
	const u8 *enable_map;
};

struct rtd_irq_mux_data {
	struct irq_domain *domain;
	struct rtd_irq_mux muxes[RTD_MAX_MUXES];
	unsigned int nr_muxes;
	raw_spinlock_t lock;
};

static const u8 rtd1619_misc_irq_map[RTD_IRQS_PER_MUX] = {
	RTD_INT_INVALID, RTD_INT_INVALID, RTD_INT_NO_ENABLE, 3,
	RTD_INT_INVALID, 5, 6, 7,
	RTD_INT_INVALID, RTD_INT_INVALID, 10, 11,
	12, RTD_INT_INVALID, 14, 15,
	RTD_INT_INVALID, RTD_INT_INVALID, RTD_INT_INVALID, RTD_INT_INVALID,
	RTD_INT_INVALID, RTD_INT_INVALID, RTD_INT_INVALID, 28,
	24, 25, RTD_INT_INVALID, 27,
	RTD_INT_INVALID, 29, RTD_INT_INVALID, RTD_INT_INVALID,
};

static const u8 rtd1619_iso_irq_map[RTD_IRQS_PER_MUX] = {
	RTD_INT_INVALID, RTD_INT_NO_ENABLE, 2, 3,
	4, 5, RTD_INT_INVALID, RTD_INT_NO_ENABLE,
	8, RTD_INT_NO_ENABLE, RTD_INT_INVALID, 11,
	12, 13, 14, 15,
	16, 17, 18, 19,
	20, RTD_INT_NO_ENABLE, RTD_INT_NO_ENABLE, RTD_INT_NO_ENABLE,
	RTD_INT_NO_ENABLE, RTD_INT_INVALID, RTD_INT_INVALID, RTD_INT_INVALID,
	28, 29, 30, 31,
};

static struct rtd_irq_mux *rtd_irq_data_to_mux(struct irq_data *d)
{
	struct rtd_irq_mux_data *data = irq_data_get_irq_chip_data(d);

	return &data->muxes[d->hwirq / RTD_IRQS_PER_MUX];
}

static void rtd_irq_mux_ack(struct irq_data *d)
{
	struct rtd_irq_mux_data *data = irq_data_get_irq_chip_data(d);
	struct rtd_irq_mux *mux = rtd_irq_data_to_mux(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);
	writel(BIT(d->hwirq % RTD_IRQS_PER_MUX), mux->base + mux->status_offset);
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

static void rtd_irq_mux_mask(struct irq_data *d)
{
	struct rtd_irq_mux_data *data = irq_data_get_irq_chip_data(d);
	struct rtd_irq_mux *mux = rtd_irq_data_to_mux(d);
	u8 enable_bit = mux->enable_map[d->hwirq % RTD_IRQS_PER_MUX];
	unsigned long flags;
	u32 val;

	if (enable_bit >= RTD_IRQS_PER_MUX)
		return;

	raw_spin_lock_irqsave(&data->lock, flags);
	val = readl(mux->base + mux->enable_offset);
	writel(val & ~BIT(enable_bit), mux->base + mux->enable_offset);
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

static void rtd_irq_mux_unmask(struct irq_data *d)
{
	struct rtd_irq_mux_data *data = irq_data_get_irq_chip_data(d);
	struct rtd_irq_mux *mux = rtd_irq_data_to_mux(d);
	u8 enable_bit = mux->enable_map[d->hwirq % RTD_IRQS_PER_MUX];
	unsigned long flags;
	u32 val;

	if (enable_bit >= RTD_IRQS_PER_MUX)
		return;

	raw_spin_lock_irqsave(&data->lock, flags);
	val = readl(mux->base + mux->enable_offset);
	writel(val | BIT(enable_bit), mux->base + mux->enable_offset);
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

static struct irq_chip rtd_irq_mux_chip = {
	.name		= "RTD-IRQ-MUX",
	.irq_ack	= rtd_irq_mux_ack,
	.irq_mask	= rtd_irq_mux_mask,
	.irq_unmask	= rtd_irq_mux_unmask,
};

static int rtd_irq_mux_domain_map(struct irq_domain *domain, unsigned int virq,
				  irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(virq, &rtd_irq_mux_chip, handle_level_irq);
	irq_set_chip_data(virq, domain->host_data);
	irq_set_probe(virq);

	return 0;
}

static const struct irq_domain_ops rtd_irq_mux_domain_ops = {
	.map = rtd_irq_mux_domain_map,
	.xlate = irq_domain_xlate_twocell,
};

static void rtd_irq_mux_handle(struct irq_desc *desc)
{
	struct rtd_irq_mux *mux = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 status, enable;
	unsigned int bit;

	chained_irq_enter(chip, desc);

	status = readl(mux->base + mux->status_offset);
	enable = readl(mux->base + mux->enable_offset);

	for (bit = 0; bit < RTD_IRQS_PER_MUX; bit++) {
		u8 enable_bit = mux->enable_map[bit];

		if (!(status & BIT(bit)))
			continue;
		if (enable_bit < RTD_IRQS_PER_MUX && !(enable & BIT(enable_bit)))
			continue;

		generic_handle_domain_irq(mux->data->domain, mux->irq_offset + bit);
	}

	chained_irq_exit(chip, desc);
}

static int __init rtd_irq_mux_init(struct device_node *node,
				   struct device_node *parent)
{
	struct rtd_irq_mux_data *data;
	unsigned int i;
	u32 nr_muxes;
	int ret;

	ret = of_property_read_u32(node, "realtek,mux-nr", &nr_muxes);
	if (ret)
		return ret;

	if (!nr_muxes || nr_muxes > RTD_MAX_MUXES)
		return -EINVAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	raw_spin_lock_init(&data->lock);
	data->nr_muxes = nr_muxes;
	data->domain = irq_domain_add_linear(node, nr_muxes * RTD_IRQS_PER_MUX,
						 &rtd_irq_mux_domain_ops, data);
	if (!data->domain) {
		kfree(data);
		return -ENOMEM;
	}

	for (i = 0; i < nr_muxes; i++) {
		struct rtd_irq_mux *mux = &data->muxes[i];

		mux->base = of_iomap(node, i);
		if (!mux->base)
			return -ENODEV;

		mux->parent_irq = irq_of_parse_and_map(node, i);
		if (!mux->parent_irq)
			return -EINVAL;

		ret = of_property_read_u32_index(node, "realtek,intr-status", i,
						     &mux->status_offset);
		if (ret)
			return ret;

		ret = of_property_read_u32_index(node, "realtek,intr-en", i,
						     &mux->enable_offset);
		if (ret)
			return ret;

		mux->data = data;
		mux->irq_offset = i * RTD_IRQS_PER_MUX;
		mux->enable_map = i ? rtd1619_iso_irq_map : rtd1619_misc_irq_map;

		irq_set_chained_handler_and_data(mux->parent_irq,
						     rtd_irq_mux_handle, mux);
	}

	return 0;
}

IRQCHIP_DECLARE(realtek_rtd1619_irq_mux, "realtek,rtd1619-irq-mux",
		rtd_irq_mux_init);
