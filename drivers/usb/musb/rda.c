/*
 * Copyright (C) 2012 RDA Microelectronics
 *
 *
 * Based on omap2430.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/rda/iomap.h>
#include <linux/soc/rda/reg_cfg_regs.h>
#include <linux/soc/rda/reg_md_sysctrl.h>
#include <linux/usb/phy.h>
#include <linux/usb/of.h>
#include <linux/usb/gpio_vbus.h>
#include <plat/md_sys.h>
#include "musb_core.h"

static struct musb_hdrc_config rda_musb_config = {
	.multipoint = false,
	.dyn_fifo = false,
	.num_eps = 16,
	.ram_bits = 12,
};

static struct musb_hdrc_platform_data rda_musb_pdata = {
	.mode = MUSB_OTG,
	.config = &rda_musb_config,
	.power = 250,
};

static int rda_get_named_gpio(struct platform_device *pdev,
			      const char *prop_name,
			      const char *res_name)
{
	int gpio;

	if (pdev->dev.of_node) {
		gpio = of_get_named_gpio(pdev->dev.of_node, prop_name, 0);
		if (gpio_is_valid(gpio))
			return gpio;
		return gpio;
	}

	gpio = platform_get_irq_byname(pdev, res_name);
	if (gpio_is_valid(gpio))
		return gpio;

	return gpio;
}

struct rda_glue {
	struct device		*dev;
	struct platform_device	*musb;
};
#define glue_to_musb(g)	platform_get_drvdata(g->musb)

#define UDC_PHY_CLK_REG (0x8c)

static void rda_musb_apply_phy_defaults(struct musb *musb);

static irqreturn_t rda_musb_interrupt(int irq, void *__hci)
{
	unsigned long	flags;
	irqreturn_t	retval = IRQ_NONE;
	struct musb	*musb = __hci;

	spin_lock_irqsave(&musb->lock, flags);

	musb->int_usb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb->int_tx = musb_readw(musb->mregs, MUSB_INTRTX);
	musb->int_rx = musb_readw(musb->mregs, MUSB_INTRRX);

	musb->int_usb &= ~MUSB_INTR_SOF;
	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

static int rda_musb_init(struct musb *musb)
{
	musb->xceiv = usb_get_phy(USB_PHY_TYPE_USB2);
	if (IS_ERR_OR_NULL(musb->xceiv)) {
		pr_err("HS USB OTG: no transceiver configured\n");
		return -EPROBE_DEFER;
	}
	musb->isr = rda_musb_interrupt;
	rda_musb_apply_phy_defaults(musb);
	return 0;
}

static int rda_musb_exit(struct musb *musb)
{
	if(!IS_ERR_OR_NULL(musb->xceiv))
		usb_put_phy(musb->xceiv);
	return 0;
}

static void rda_musb_enable(struct musb *musb)
{
	pr_info("platform enable musb\n");
}

static void rda_musb_disable(struct musb *musb)
{
	pr_info("platform disable musb\n");
}

static void rda_musb_set_vbus(struct musb *musb, int is_on);
static void rda_musb_set_vbus_level(int level);
static void rda_set_plugin_level(int level);
void rda_usbid_set(int value);
static void musb_rda_generate_SE0(struct musb *musb);
static void rda_log_usbid_fallback_state(const char *tag);

static int rda_vbus_on_level = 1;
static int rda_usbid_host_level = 0;
static int rda_plugin_on_level = 1;
static bool rda_levels_autodetected;
static struct regulator *rda_vbus_reg;
static struct msys_device *rda_musb_charger_msys;
static bool rda_musb_charger_msys_registered;

static void rda_musb_charger_msys_release(void)
{
	if (!rda_musb_charger_msys)
		return;

	if (rda_musb_charger_msys_registered)
		rda_msys_unregister_device(rda_musb_charger_msys);

	rda_msys_free_device(rda_musb_charger_msys);
	rda_musb_charger_msys = NULL;
	rda_musb_charger_msys_registered = false;
}

static void rda_musb_charger_msys_setup(struct device *dev)
{
	int ret;

	if (rda_musb_charger_msys)
		return;

	rda_musb_charger_msys = rda_msys_alloc_device();
	if (!rda_musb_charger_msys)
		return;

	rda_musb_charger_msys->module = SYS_PM_MOD;
	rda_musb_charger_msys->name = "musb-rda";
	rda_musb_charger_msys->private = dev;

	ret = rda_msys_register_device(rda_musb_charger_msys);
	if (ret) {
		dev_dbg(dev, "failed to register msys charger client: %d\n", ret);
		rda_musb_charger_msys_release();
		return;
	}

	rda_musb_charger_msys_registered = true;
}

int rda_modem_charger_enable(int enable)
{
	struct client_cmd cmd_set;
	unsigned int ret;
	int value = !!enable;

	if (!rda_musb_charger_msys)
		return -ENODEV;

	memset(&cmd_set, 0, sizeof(cmd_set));
	cmd_set.pmsys_dev = rda_musb_charger_msys;
	cmd_set.mod_id = SYS_PM_MOD;
	cmd_set.mesg_id = SYS_PM_CMD_ENABLE_CHARGER;
	cmd_set.pdata = &value;
	cmd_set.data_size = sizeof(value);

	ret = rda_msys_send_cmd(&cmd_set);
	if (ret > 0)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL_GPL(rda_modem_charger_enable);

static void rda_musb_autodetect_host_levels(struct musb *musb)
{
	u8 devctl;
	bool plugin_found = false;
	bool usbid_found = false;
	bool vbus_found = false;
	int i;
	int j;
	const int plugin_probe[] = { 1, 0 };
	const int usbid_probe[] = { 0, 1 };
	const int vbus_probe[] = { 1, 0 };

	if (rda_levels_autodetected || !musb || !musb->mregs)
		return;

	for (i = 0; i < ARRAY_SIZE(plugin_probe); i++) {
		rda_set_plugin_level(plugin_probe[i]);
		udelay(20);

		for (j = 0; j < ARRAY_SIZE(usbid_probe); j++) {
			rda_usbid_set(usbid_probe[j]);
			udelay(20);
			devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
			pr_info("probe plugin=%d usbid=%d DEVCTL=0x%02x\n",
				plugin_probe[i], usbid_probe[j], devctl);
			if (!(devctl & MUSB_DEVCTL_BDEVICE)) {
				rda_plugin_on_level = plugin_probe[i];
				rda_usbid_host_level = usbid_probe[j];
				plugin_found = true;
				usbid_found = true;
				break;
			}
		}

		if (usbid_found)
			break;
	}

	if (!usbid_found)
		pr_info("unable to clear BDEVICE via usbid, keep host level %d\n",
			rda_usbid_host_level);
	if (!plugin_found)
		pr_info("unable to infer plugin polarity, keep level %d\n",
			rda_plugin_on_level);

	for (i = 0; i < ARRAY_SIZE(vbus_probe); i++) {
		rda_musb_set_vbus_level(vbus_probe[i]);
		udelay(20);
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		pr_info("probe vbus-level=%d DEVCTL=0x%02x\n",
			vbus_probe[i], devctl);
		if ((devctl & MUSB_DEVCTL_VBUS) == MUSB_DEVCTL_VBUS) {
			rda_vbus_on_level = vbus_probe[i];
			vbus_found = true;
			break;
		}
	}

	if (!vbus_found)
		pr_info("unable to detect full VBUS, keep vbus on level %d\n",
			rda_vbus_on_level);

	rda_levels_autodetected = true;
	rda_set_plugin_level(rda_plugin_on_level);
	rda_usbid_set(rda_usbid_host_level);
	rda_musb_set_vbus_level(rda_vbus_on_level);
	pr_info("autodetect levels: plugin_on=%d usbid_host=%d vbus_on=%d\n",
		rda_plugin_on_level, rda_usbid_host_level, rda_vbus_on_level);
}

static int rda_musb_wait_host_role(struct musb *musb, unsigned int timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);
	u8 devctl;

	while (1) {
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		/* HM can indicate effective host mode on some broken ID paths. */
		if ((devctl & MUSB_DEVCTL_SESSION) &&
		    (!(devctl & MUSB_DEVCTL_BDEVICE) ||
		     (devctl & MUSB_DEVCTL_HM)))
			return 0;

		cpu_relax();
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
	}
}

static int rda_musb_force_host_testmode(struct musb *musb)
{
	unsigned long flags;
	u8 devctl;
	u8 test;
	int ret;

	spin_lock_irqsave(&musb->lock, flags);
	test = musb_readb(musb->mregs, MUSB_TESTMODE);
	if (test != MUSB_TEST_FORCE_HOST)
		musb_writeb(musb->mregs, MUSB_TESTMODE, MUSB_TEST_FORCE_HOST);

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	devctl |= MUSB_DEVCTL_SESSION;
	devctl &= ~MUSB_DEVCTL_HR;
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
	MUSB_HST_MODE(musb);
	spin_unlock_irqrestore(&musb->lock, flags);

	ret = rda_musb_wait_host_role(musb, 300);
	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	test = musb_readb(musb->mregs, MUSB_TESTMODE);
	pr_info("host mode: test-force DEVCTL=0x%02x TESTMODE=0x%02x ret=%d\n",
		devctl, test, ret);

	return ret;
}

static void rda_musb_try_release_force_host_testmode(struct musb *musb)
{
	unsigned long flags;
	u8 devctl;
	u8 test;

	test = musb_readb(musb->mregs, MUSB_TESTMODE);
	if (!(test & MUSB_TEST_FORCE_HOST))
		return;

	spin_lock_irqsave(&musb->lock, flags);
	musb_writeb(musb->mregs, MUSB_TESTMODE, 0);
	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	devctl |= MUSB_DEVCTL_SESSION;
	devctl &= ~MUSB_DEVCTL_HR;
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
	MUSB_HST_MODE(musb);
	spin_unlock_irqrestore(&musb->lock, flags);

	udelay(80);
	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	test = musb_readb(musb->mregs, MUSB_TESTMODE);
	if ((devctl & MUSB_DEVCTL_BDEVICE) && !(devctl & MUSB_DEVCTL_HM)) {
		spin_lock_irqsave(&musb->lock, flags);
		musb_writeb(musb->mregs, MUSB_TESTMODE, MUSB_TEST_FORCE_HOST);
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		devctl |= MUSB_DEVCTL_SESSION;
		devctl &= ~MUSB_DEVCTL_HR;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		MUSB_HST_MODE(musb);
		spin_unlock_irqrestore(&musb->lock, flags);

		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		test = musb_readb(musb->mregs, MUSB_TESTMODE);
		pr_info("host mode: keep TESTMODE force-host DEVCTL=0x%02x TESTMODE=0x%02x\n",
			devctl, test);
	} else {
		pr_info("host mode: released TESTMODE force-host DEVCTL=0x%02x TESTMODE=0x%02x\n",
			devctl, test);
	}
}

static int rda_musb_force_host_request(struct musb *musb)
{
	unsigned long flags;
	u8 devctl;
	int i;

	for (i = 0; i < 3; i++) {
		spin_lock_irqsave(&musb->lock, flags);
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		devctl |= MUSB_DEVCTL_SESSION;
		devctl &= ~MUSB_DEVCTL_HR;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		if (musb->xceiv && musb->xceiv->otg) {
			musb->xceiv->otg->default_a = 1;
			musb->xceiv->otg->state = OTG_STATE_A_WAIT_BCON;
		}
		MUSB_HST_MODE(musb);
		spin_unlock_irqrestore(&musb->lock, flags);

		if (!rda_musb_wait_host_role(musb, 120))
			return 0;

		if (i == 2)
			break;

		spin_lock_irqsave(&musb->lock, flags);
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		spin_unlock_irqrestore(&musb->lock, flags);

		udelay(80);
		rda_set_plugin_level(rda_plugin_on_level);
		rda_usbid_set(rda_usbid_host_level);
		rda_musb_set_vbus_level(rda_vbus_on_level);
	}

	/* Keep host request asserted so late attach still has a chance. */
	rda_set_plugin_level(rda_plugin_on_level);
	rda_usbid_set(rda_usbid_host_level);
	rda_musb_set_vbus_level(rda_vbus_on_level);
	spin_lock_irqsave(&musb->lock, flags);
	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
	devctl |= MUSB_DEVCTL_SESSION;
	devctl &= ~MUSB_DEVCTL_HR;
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
	MUSB_HST_MODE(musb);
	spin_unlock_irqrestore(&musb->lock, flags);

	return -ETIMEDOUT;
}

static int rda_musb_set_mode(struct musb *musb, u8 mode)
{
	unsigned long flags;
	u8 devctl;
	u8 power;
	int charger_ret;
	int host_ret = 0;
	int vbus_ret;

	/*
	 * On this platform, host/peripheral role needs explicit strap control
	 * in addition to generic MUSB core role switching.
	 */
	if (mode == MUSB_HOST) {
		if (!IS_ERR_OR_NULL(rda_vbus_reg)) {
			vbus_ret = regulator_enable(rda_vbus_reg);
			if (vbus_ret)
				pr_info("host mode vbus regulator enable ret=%d\n",
					vbus_ret);
		}

		charger_ret = rda_modem_charger_enable(0);
		pr_info("host mode charger switch ret=%d\n", charger_ret);
		rda_musb_apply_phy_defaults(musb);
		rda_musb_autodetect_host_levels(musb);
		rda_set_plugin_level(rda_plugin_on_level);
		rda_usbid_set(rda_usbid_host_level);
		rda_musb_set_vbus_level(rda_vbus_on_level);
		rda_log_usbid_fallback_state("host-prep");

		spin_lock_irqsave(&musb->lock, flags);
		power = musb_readb(musb->mregs, MUSB_POWER);
		power &= ~MUSB_POWER_HSENAB;
		musb_writeb(musb->mregs, MUSB_POWER, power);
		musb_writeb(musb->mregs, MUSB_TESTMODE, 0);

		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		devctl |= MUSB_DEVCTL_SESSION;
		devctl &= ~MUSB_DEVCTL_HR;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		MUSB_HST_MODE(musb);
		spin_unlock_irqrestore(&musb->lock, flags);

		host_ret = rda_musb_wait_host_role(musb, 1000);
		if (host_ret) {
			pr_info("host mode: BDEVICE stuck, retry host request\n");
			host_ret = rda_musb_force_host_request(musb);
			if (host_ret) {
				pr_info("host mode: retry with TESTMODE force-host\n");
				host_ret = rda_musb_force_host_testmode(musb);
			}
			if (host_ret)
				pr_info("host mode: keep session asserted despite BDEVICE timeout\n");
		}

		rda_musb_try_release_force_host_testmode(musb);

		musb_rda_generate_SE0(musb);
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		power = musb_readb(musb->mregs, MUSB_POWER);
		pr_info("host mode: DEVCTL=0x%02x POWER=0x%02x TESTMODE=0x%02x host_force_ret=%d\n",
			devctl, power, musb_readb(musb->mregs, MUSB_TESTMODE), host_ret);
	} else {
		charger_ret = rda_modem_charger_enable(1);
		pr_info("device mode charger switch ret=%d\n", charger_ret);

		if (!IS_ERR_OR_NULL(rda_vbus_reg) &&
		    regulator_is_enabled(rda_vbus_reg) > 0) {
			vbus_ret = regulator_disable(rda_vbus_reg);
			if (vbus_ret)
				pr_info("device mode vbus regulator disable ret=%d\n",
					vbus_ret);
		}

		spin_lock_irqsave(&musb->lock, flags);
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		devctl &= ~(MUSB_DEVCTL_SESSION | MUSB_DEVCTL_HR);
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		musb_writeb(musb->mregs, MUSB_TESTMODE, 0);
		if (musb->xceiv && musb->xceiv->otg) {
			musb->xceiv->otg->default_a = 0;
			musb->xceiv->otg->state = OTG_STATE_B_IDLE;
		}
		MUSB_DEV_MODE(musb);
		power = musb_readb(musb->mregs, MUSB_POWER);
		power |= MUSB_POWER_HSENAB;
		musb_writeb(musb->mregs, MUSB_POWER, power);
		spin_unlock_irqrestore(&musb->lock, flags);

		rda_usbid_set(!rda_usbid_host_level);
		rda_musb_set_vbus_level(!rda_vbus_on_level);
		rda_log_usbid_fallback_state("device-prep");
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		power = musb_readb(musb->mregs, MUSB_POWER);
		pr_info("device mode: DEVCTL=0x%02x POWER=0x%02x\n", devctl, power);
	}

	pr_info("set musb mode %u\n", mode);
	return 0;
}

static int gpio_vbus_switch = -1;
static int gpio_usbid_ctrl = -1;
static int gpio_plugin_ctrl = -1;
static bool rda_vbus_always_on;

struct rda_gpio_fallback {
	void __iomem *base;
	int bit;
};

static struct rda_gpio_fallback rda_vbus_fallback = { .bit = -1 };
static struct rda_gpio_fallback rda_plugin_fallback = { .bit = -1 };

static void __iomem *rda_usbid_gpo_base;
static int rda_usbid_gpo_bit = -1;
static bool rda_usbid_dual_fallback;
static void __iomem *rda_cfg_regs_base;
static void __iomem *rda_md_sysctrl_base;

#define RDA_GPIO_OEN_SET_OUT_REG	0x04
#define RDA_GPIO_SET_REG		0x10
#define RDA_GPIO_CLR_REG		0x14
#define RDA_GPIO_GPO_SET_REG	0x30
#define RDA_GPIO_GPO_CLR_REG	0x34

static void rda_log_usbid_fallback_state(const char *tag)
{
	u32 gpo_set;
	u32 gpio_set;

	if (!rda_usbid_gpo_base || rda_usbid_gpo_bit < 0)
		return;

	gpo_set = readl(rda_usbid_gpo_base + RDA_GPIO_GPO_SET_REG);
	gpio_set = readl(rda_usbid_gpo_base + RDA_GPIO_SET_REG);
	pr_info("usbid %s: gpo_set=0x%08x gpio_set=0x%08x bit=%d\n",
		tag, gpo_set, gpio_set, rda_usbid_gpo_bit);
}

static void rda_cleanup_gpio_fallback(struct rda_gpio_fallback *fallback)
{
	if (fallback->base)
		iounmap(fallback->base);

	fallback->base = NULL;
	fallback->bit = -1;
}

static void rda_set_gpio_fallback_output(struct rda_gpio_fallback *fallback,
					 int value)
{
	if (!fallback->base || fallback->bit < 0)
		return;

	writel(BIT(fallback->bit), fallback->base + RDA_GPIO_OEN_SET_OUT_REG);
	writel(BIT(fallback->bit), fallback->base +
	       (value ? RDA_GPIO_SET_REG : RDA_GPIO_CLR_REG));
}

static void rda_setup_gpio_fallback(struct platform_device *pdev,
				    const char *prop_name,
				    struct rda_gpio_fallback *fallback,
				    const char *label)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *gpio_np;
	u32 bit;

	if (!np)
		return;

	rda_cleanup_gpio_fallback(fallback);

	gpio_np = of_parse_phandle(np, prop_name, 0);
	if (!gpio_np)
		return;

	fallback->base = of_iomap(gpio_np, 0);
	of_node_put(gpio_np);
	if (!fallback->base)
		return;

	if (of_property_read_u32_index(np, prop_name, 1, &bit) ||
	    bit >= BITS_PER_TYPE(u32)) {
		dev_warn(&pdev->dev, "invalid %s bit in %s\n", label, prop_name);
		rda_cleanup_gpio_fallback(fallback);
		return;
	}

	fallback->bit = bit;
	dev_info(&pdev->dev, "using %s register fallback, bit %u\n",
		 label, bit);
}

static void rda_cleanup_usbid_gpo(void)
{
	if (rda_usbid_gpo_base)
		iounmap(rda_usbid_gpo_base);

	rda_usbid_gpo_base = NULL;
	rda_usbid_gpo_bit = -1;
	rda_usbid_dual_fallback = false;
}

static void rda_cleanup_cfg_regs(void)
{
	if (rda_cfg_regs_base)
		iounmap(rda_cfg_regs_base);

	rda_cfg_regs_base = NULL;
}

static void rda_cleanup_md_sysctrl(void)
{
	if (rda_md_sysctrl_base)
		iounmap(rda_md_sysctrl_base);

	rda_md_sysctrl_base = NULL;
}

static HWP_SYS_CTRL_T __iomem *rda_get_md_sysctrl(struct platform_device *pdev)
{
	if (!rda_md_sysctrl_base) {
		rda_md_sysctrl_base = ioremap(RDA_MD_SYSCTRL_PHYS,
					      RDA_MD_SYSCTRL_SIZE);
		if (!rda_md_sysctrl_base) {
			dev_warn(&pdev->dev,
				 "failed to map md sysctrl for cfg unlock\n");
			return NULL;
		}
	}

	return (HWP_SYS_CTRL_T __iomem *)rda_md_sysctrl_base;
}

static void rda_force_usbid_gpo_mux(struct platform_device *pdev)
{
	HWP_SYS_CTRL_T __iomem *mdsys;
	HWP_CFG_REGS_T __iomem *cfg;
	u32 val;
	u32 new_val;

	if (!rda_cfg_regs_base) {
		rda_cfg_regs_base = ioremap(RDA_CONFIG_REGS_PHYS,
					   RDA_CONFIG_REGS_SIZE);
		if (!rda_cfg_regs_base) {
			dev_warn(&pdev->dev,
				 "failed to map cfg regs for usbid mux\n");
			return;
		}
	}

	cfg = (HWP_CFG_REGS_T __iomem *)rda_cfg_regs_base;
	mdsys = rda_get_md_sysctrl(pdev);
	if (!mdsys)
		return;

	writel(SYS_CTRL_PROTECT_UNLOCK, &mdsys->REG_DBG);
	val = readl(&cfg->Alt_mux_select);
	new_val = (val & ~CFG_REGS_GPO_1_MASK) | CFG_REGS_GPO_1_GPO_1;
	dev_info(&pdev->dev,
		 "usbid mux Alt_mux_select current=0x%08x target=0x%08x\n",
		 val, new_val);
	if (new_val != val) {
		writel(new_val, &cfg->Alt_mux_select);
		val = readl(&cfg->Alt_mux_select);
		dev_info(&pdev->dev,
			 "forced usbid mux Alt_mux_select now=0x%08x\n", val);
	} else {
		dev_info(&pdev->dev, "usbid mux already on GPO_1\n");
	}
	writel(SYS_CTRL_PROTECT_LOCK, &mdsys->REG_DBG);
}

static void rda_cleanup_optional_gpios(void)
{
	rda_cleanup_gpio_fallback(&rda_vbus_fallback);
	rda_cleanup_gpio_fallback(&rda_plugin_fallback);
	rda_cleanup_usbid_gpo();
	rda_cleanup_cfg_regs();
	rda_cleanup_md_sysctrl();
}

static void rda_setup_usbid_gpo(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *gpio_np;
	u32 gpo_bit = 1;

	if (!np || gpio_is_valid(gpio_usbid_ctrl))
		return;

	if (!device_property_read_bool(&pdev->dev, "rda,usbid-gpo-enable"))
		return;

	gpio_np = of_parse_phandle(np, "rda,usbid-gpo-controller", 0);
	if (!gpio_np)
		return;

	rda_usbid_gpo_base = of_iomap(gpio_np, 0);
	of_node_put(gpio_np);
	if (!rda_usbid_gpo_base)
		return;

	device_property_read_u32(&pdev->dev, "rda,usbid-gpo-bit", &gpo_bit);
	if (gpo_bit >= BITS_PER_TYPE(u32)) {
		dev_warn(&pdev->dev, "invalid usbid GPO bit %u\n", gpo_bit);
		rda_cleanup_usbid_gpo();
		return;
	}

	rda_usbid_gpo_bit = gpo_bit;
	rda_usbid_dual_fallback = device_property_read_bool(&pdev->dev,
						      "rda,usbid-dual-fallback");
	rda_force_usbid_gpo_mux(pdev);
	dev_info(&pdev->dev, "using usbid GPO fallback, bit %u\n", gpo_bit);
	if (rda_usbid_dual_fallback)
		dev_info(&pdev->dev,
			 "using dual usbid fallback (GPO + GPIO output path)\n");
}

static int rda_musb_vbus_status(struct musb *musb)
{
	int value = 0;

	if (gpio_is_valid(gpio_vbus_switch))
		value = gpio_get_value(gpio_vbus_switch);

	return !!value;
}

static void rda_musb_set_vbus_level(int level)
{
	if (gpio_is_valid(gpio_vbus_switch))
		gpio_direction_output(gpio_vbus_switch, !!level);

	rda_set_gpio_fallback_output(&rda_vbus_fallback, !!level);
}

static void rda_set_plugin_level(int level)
{
	if (gpio_is_valid(gpio_plugin_ctrl))
		gpio_direction_output(gpio_plugin_ctrl, !!level);

	rda_set_gpio_fallback_output(&rda_plugin_fallback, !!level);
}

static void rda_musb_apply_phy_defaults(struct musb *musb)
{
	musb_writel(musb->mregs, UDC_PHY_CLK_REG, 0x5900f000);
}

static void rda_musb_set_vbus(struct musb *musb, int is_on)
{
	int value;

	if (rda_vbus_always_on)
		value = rda_vbus_on_level;
	else
		value = is_on ? rda_vbus_on_level : !rda_vbus_on_level;

	rda_musb_set_vbus_level(value);

	return;
}

/* RDA glue currently runs in PIO mode; provide no-op DMA hooks for musb core. */
static struct dma_controller *rda_musb_dma_init(struct musb *musb,
						 void __iomem *base)
{
	return NULL;
}

static void rda_musb_dma_exit(struct dma_controller *c)
{
}

static const struct musb_platform_ops rda_ops = {
	.init		= rda_musb_init,
	.exit		= rda_musb_exit,
	.dma_init	= rda_musb_dma_init,
	.dma_exit	= rda_musb_dma_exit,
	.enable		= rda_musb_enable,
	.disable	= rda_musb_disable,
	.set_mode	= rda_musb_set_mode,
	.vbus_status	= rda_musb_vbus_status,
	.set_vbus	= rda_musb_set_vbus,
};


void rda_start_host(struct usb_bus *host)
{
	void __iomem *regs;
	u8 devctl;
	u8 power;
	struct usb_hcd * hcd;
	struct musb *musb;
	unsigned long flags;
	struct usb_otg	*otg;
	unsigned long timeout;

	hcd = bus_to_hcd(host);
	musb = hcd_to_musb(hcd);

	musb_platform_set_vbus(musb, 1);
	msleep(1);

	regs = musb->mregs;
	otg = musb->xceiv->otg;
	spin_lock_irqsave(&musb->lock, flags);
	/* Match vendor behavior: keep host link in FS mode on this platform. */
	power = musb_readb(regs, MUSB_POWER);
	power &= ~MUSB_POWER_HSENAB;
	musb_writeb(regs, MUSB_POWER, power);

	devctl = MUSB_DEVCTL_SESSION;
	musb_writeb(regs, MUSB_DEVCTL, devctl);

	if (otg) {
		otg->default_a = 1;
		otg->state = OTG_STATE_A_WAIT_BCON;
	}
	devctl |= MUSB_DEVCTL_SESSION;
	MUSB_HST_MODE(musb);
	spin_unlock_irqrestore(&musb->lock, flags);

	timeout = jiffies + msecs_to_jiffies(1000);
	while (musb_readb(musb->mregs, MUSB_DEVCTL) & 0x80) {

		cpu_relax();

		if (time_after(jiffies, timeout)) {
			dev_err(musb->controller,
			"configured as A device timeout\n");
			break;
		}
	}


}
EXPORT_SYMBOL_GPL(rda_start_host);

void rda_stop_host(struct usb_bus *host)
{
	void __iomem *regs;
	u8 devctl;
	struct usb_hcd * hcd;
	struct musb *musb;
	struct usb_otg	*otg;
	u8 power;
	unsigned long flags;

	hcd = bus_to_hcd(host);
	musb = hcd_to_musb(hcd);

	musb_platform_set_vbus(musb, 0);
	regs = musb->mregs;
	otg = musb->xceiv->otg;

	spin_lock_irqsave(&musb->lock, flags);

	/* enable high speed when device*/
	power = musb_readb(regs, MUSB_POWER);
	power |= MUSB_POWER_HSENAB;
	musb_writeb(regs, MUSB_POWER, power);

	musb->is_active = 0;
	if (otg) {
		otg->default_a = 0;
		otg->state = OTG_STATE_B_IDLE;
	}
	MUSB_DEV_MODE(musb);

	devctl = musb_readb(regs, MUSB_DEVCTL);
	devctl &= ~MUSB_DEVCTL_SESSION;
	musb_writeb(regs, MUSB_DEVCTL, devctl);
	spin_unlock_irqrestore(&musb->lock, flags);
}
EXPORT_SYMBOL_GPL(rda_stop_host);

static void musb_rda_generate_SE0(struct musb *musb)
{
	int i;
	int cnt = 5;

	for (i = 0; i < cnt; i++) {
		musb_writel(musb->mregs, 0x8c, 0x7100f000);
		udelay(1);
		musb_writel(musb->mregs, 0x8c, 0x5900f000);
	}
}

void rda_usbid_set(int value)
{
	if (gpio_is_valid(gpio_usbid_ctrl)) {
		gpio_direction_output(gpio_usbid_ctrl, !!value);
	} else if (rda_usbid_gpo_base && rda_usbid_gpo_bit >= 0) {
		if (rda_usbid_dual_fallback) {
			writel(BIT(rda_usbid_gpo_bit),
			       rda_usbid_gpo_base + RDA_GPIO_OEN_SET_OUT_REG);
			writel(BIT(rda_usbid_gpo_bit),
			       rda_usbid_gpo_base +
			       (value ? RDA_GPIO_SET_REG : RDA_GPIO_CLR_REG));
		}

		writel(BIT(rda_usbid_gpo_bit),
		       rda_usbid_gpo_base +
		       (value ? RDA_GPIO_GPO_SET_REG : RDA_GPIO_GPO_CLR_REG));
		rda_log_usbid_fallback_state(value ? "set-1" : "set-0");
	}
}
EXPORT_SYMBOL_GPL(rda_usbid_set);

static int rda_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data	*pdata = pdev->dev.platform_data;
	struct platform_device		*musb;
	struct rda_glue		*glue;
	struct resource *mem;
	struct resource resources[6];
	int num_res = 0;
	int mc_irq;
	int dma_irq;
	int				ret = -ENOMEM;
	int host_enable = 0;
	enum usb_dr_mode dr_mode;

	if (!pdata) {
		pdata = &rda_musb_pdata;
		dr_mode = usb_get_dr_mode(&pdev->dev);
		if (dr_mode == USB_DR_MODE_HOST)
			pdata->mode = MUSB_HOST;
		else if (dr_mode == USB_DR_MODE_PERIPHERAL)
			pdata->mode = MUSB_PERIPHERAL;
		else
			pdata->mode = MUSB_OTG;
	}

	rda_cleanup_optional_gpios();
	rda_levels_autodetected = false;
	rda_vbus_on_level = 1;
	rda_usbid_host_level = 0;
	rda_plugin_on_level = 1;
	rda_vbus_reg = ERR_PTR(-ENODEV);
	rda_musb_charger_msys_setup(&pdev->dev);

	glue = kzalloc(sizeof(*glue), GFP_KERNEL);
	if (!glue) {
		dev_err(&pdev->dev, "failed to allocate glue context\n");
		goto err0;
	}

	musb = platform_device_alloc("musb-hdrc", -1);
	if (!musb) {
		dev_err(&pdev->dev, "failed to allocate musb device\n");
		goto err1;
	}

	rda_vbus_reg = devm_regulator_get_optional(&pdev->dev, "vbus");
	if (IS_ERR(rda_vbus_reg)) {
		if (PTR_ERR(rda_vbus_reg) != -ENODEV)
			dev_dbg(&pdev->dev, "optional vbus regulator unavailable: %ld\n",
				PTR_ERR(rda_vbus_reg));
	}

	rda_setup_gpio_fallback(pdev, "rda,vbus-switch-gpios",
			       &rda_vbus_fallback, "vbus-switch");
	rda_setup_gpio_fallback(pdev, "rda,plugin-ctrl-gpios",
			       &rda_plugin_fallback, "plugin_ctrl");

	gpio_vbus_switch = rda_get_named_gpio(pdev,
					     "rda,vbus-switch-gpios",
					     "vbus-switch");
	if (gpio_is_valid(gpio_vbus_switch) || rda_vbus_fallback.base) {
		if (gpio_is_valid(gpio_vbus_switch))
			gpio_request(gpio_vbus_switch, "vbus-switch");
		rda_vbus_always_on = device_property_read_bool(&pdev->dev,
			"rda,vbus-always-on");
		if (gpio_is_valid(gpio_vbus_switch))
			gpio_direction_output(gpio_vbus_switch,
				rda_vbus_always_on ? 1 : 0);
		rda_set_gpio_fallback_output(&rda_vbus_fallback,
				rda_vbus_always_on ? 1 : 0);
		host_enable = 1;
	} else {
		pr_info("cannot get vbus switch gpio, usb host disable\n ");
	}

	if (host_enable) {
		gpio_usbid_ctrl = rda_get_named_gpio(pdev,
					    "rda,usbid-ctrl-gpios",
					    "usbid_ctrl");
		if (gpio_is_valid(gpio_usbid_ctrl)) {
			gpio_request(gpio_usbid_ctrl, "usbid_ctrl");
			gpio_direction_output(gpio_usbid_ctrl,
				pdata->mode == MUSB_HOST ? 0 : 1);
		} else {
			rda_setup_usbid_gpo(pdev);
			rda_usbid_set(pdata->mode == MUSB_HOST ? 0 : 1);
			dev_dbg(&pdev->dev,
				"optional usbid control GPIO is missing\n");
		}

		gpio_plugin_ctrl = rda_get_named_gpio(pdev,
					     "rda,plugin-ctrl-gpios",
					     "plugin_ctrl");
		if (gpio_is_valid(gpio_plugin_ctrl)) {
			gpio_request(gpio_plugin_ctrl, "plugin_ctrl");
			gpio_direction_output(gpio_plugin_ctrl, rda_plugin_on_level);
			rda_set_gpio_fallback_output(&rda_plugin_fallback,
						     rda_plugin_on_level);
		} else {
			rda_set_gpio_fallback_output(&rda_plugin_fallback,
						     rda_plugin_on_level);
			if (!rda_plugin_fallback.base)
				dev_dbg(&pdev->dev,
					"optional plugin control GPIO is missing\n");
		}
	}

	musb->dev.parent		= &pdev->dev;
	musb->dev.dma_mask		= pdev->dev.dma_mask;
	musb->dev.coherent_dma_mask	= pdev->dev.coherent_dma_mask;

	glue->dev			= &pdev->dev;
	glue->musb			= musb;
	//glue->clk			= clk;

	pdata->platform_ops		= &rda_ops;

	platform_set_drvdata(pdev, glue);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		ret = -ENXIO;
		dev_err(&pdev->dev, "failed to get USB memory resource\n");
		goto err2;
	}

	resources[num_res].start = mem->start;
	resources[num_res].end = mem->end;
	resources[num_res].name = "usb-mem";
	resources[num_res].flags = IORESOURCE_MEM;
	num_res++;

	mc_irq = platform_get_irq_byname(pdev, "mc");
	if (mc_irq < 0)
		mc_irq = platform_get_irq(pdev, 0);
	if (mc_irq < 0) {
		ret = mc_irq;
		dev_err(&pdev->dev, "failed to get MUSB mc IRQ\n");
		goto err2;
	}

	dma_irq = platform_get_irq_byname(pdev, "dma");
	if (dma_irq < 0)
		dma_irq = mc_irq;

	resources[num_res].start = mc_irq;
	resources[num_res].end = mc_irq;
	resources[num_res].name = "mc";
	resources[num_res].flags = IORESOURCE_IRQ;
	num_res++;

	resources[num_res].start = dma_irq;
	resources[num_res].end = dma_irq;
	resources[num_res].name = "dma";
	resources[num_res].flags = IORESOURCE_IRQ;
	num_res++;

	if (host_enable && gpio_is_valid(gpio_vbus_switch)) {
		resources[num_res].start = gpio_vbus_switch;
		resources[num_res].end = gpio_vbus_switch;
		resources[num_res].name = "vbus-switch";
		resources[num_res].flags = IORESOURCE_IRQ;
		num_res++;
	}

	if (host_enable && gpio_is_valid(gpio_usbid_ctrl)) {
		resources[num_res].start = gpio_usbid_ctrl;
		resources[num_res].end = gpio_usbid_ctrl;
		resources[num_res].name = "usbid_ctrl";
		resources[num_res].flags = IORESOURCE_IRQ;
		num_res++;
	}

	if (host_enable && gpio_is_valid(gpio_plugin_ctrl)) {
		resources[num_res].start = gpio_plugin_ctrl;
		resources[num_res].end = gpio_plugin_ctrl;
		resources[num_res].name = "plugin_ctrl";
		resources[num_res].flags = IORESOURCE_IRQ;
		num_res++;
	}

	ret = platform_device_add_resources(musb, resources, num_res);
	if (ret) {
		dev_err(&pdev->dev, "failed to add resources\n");
		goto err2;
	}

	ret = platform_device_add_data(musb, pdata, sizeof(*pdata));
	if (ret) {
		dev_err(&pdev->dev, "failed to add platform_data\n");
		goto err2;
	}

	ret = platform_device_add(musb);
	if (ret) {
		dev_err(&pdev->dev, "failed to register musb device\n");
		goto err2;
	}

	return 0;
err2:
	rda_cleanup_optional_gpios();
	rda_musb_charger_msys_release();
	platform_device_put(musb);

err1:
	kfree(glue);

err0:
	return ret;
}

static void rda_remove(struct platform_device *pdev)
{
	struct rda_glue	*glue = platform_get_drvdata(pdev);

	platform_device_del(glue->musb);
	platform_device_put(glue->musb);
	rda_cleanup_optional_gpios();
	rda_musb_charger_msys_release();
	kfree(glue);
}

#ifdef CONFIG_PM
static int rda_suspend(struct device *dev)
{
	struct rda_glue	*glue = dev_get_drvdata(dev);
	struct musb		*musb = glue_to_musb(glue);

	usb_phy_set_suspend(musb->xceiv, 1);

	return 0;
}

static int rda_resume(struct device *dev)
{
	struct rda_glue	        *glue = dev_get_drvdata(dev);
	struct musb		*musb = glue_to_musb(glue);

	usb_phy_set_suspend(musb->xceiv, 0);

	return 0;
}

static const struct dev_pm_ops rda_pm_ops = {
	.suspend	= rda_suspend,
	.resume		= rda_resume,
};

#define DEV_PM_OPS	(&rda_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif

static const struct of_device_id rda_musb_of_match[] = {
	{ .compatible = "rda,8810pl-musb" },
	{ }
};
MODULE_DEVICE_TABLE(of, rda_musb_of_match);

static struct platform_driver rda_driver = {
	.probe		= rda_probe,
	.remove		= rda_remove,
	.driver		= {
		.name	= "musb-rda",
		.pm	= DEV_PM_OPS,
		.of_match_table = rda_musb_of_match,
	},
};

MODULE_DESCRIPTION("RDA MUSB Glue Layer");
MODULE_LICENSE("GPL v2");

static int __init rda_init(void)
{
	return platform_driver_register(&rda_driver);
}
module_init(rda_init);

static void __exit rda_exit(void)
{
	platform_driver_unregister(&rda_driver);
}
module_exit(rda_exit);

