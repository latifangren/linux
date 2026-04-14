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
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/usb/phy.h>
#include <linux/usb/of.h>
#include <linux/usb/gpio_vbus.h>
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

	//musb_writel(musb->mregs, UDC_PHY_CLK_REG, 0x5900f000);
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

static int rda_musb_set_mode(struct musb *musb, u8 mode)
{
	pr_info("set musb mode\n");
	return 0;
}

static int gpio_vbus_switch = -1;
static int gpio_usbid_ctrl = -1;
static int gpio_plugin_ctrl = -1;

static int rda_musb_vbus_status(struct musb *musb)
{
	int value = 0;

	if (gpio_is_valid(gpio_vbus_switch))
		value = gpio_get_value(gpio_vbus_switch);

	return !!value;
}

static void rda_musb_set_vbus(struct musb *musb, int is_on)
{
	if (gpio_is_valid(gpio_vbus_switch))
		gpio_set_value(gpio_vbus_switch, !!is_on);

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
	/* disable high speed when host*/
	//power = musb_readb(regs, MUSB_POWER);
	//power &= ~MUSB_POWER_HSENAB;
	//musb_writeb(regs, MUSB_POWER, power);

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

static void __maybe_unused musb_rda_generate_SE0(struct musb *musb)
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
		gpio_set_value(gpio_usbid_ctrl, value);
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

	gpio_vbus_switch = rda_get_named_gpio(pdev,
					     "rda,vbus-switch-gpios",
					     "vbus-switch");
	if (gpio_is_valid(gpio_vbus_switch)) {
		gpio_request(gpio_vbus_switch, "vbus-switch");
		gpio_direction_output(gpio_vbus_switch, 0);
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
			pr_info("can't get usbid control, maybe usbid is \
					connected to micro-A plug\n ");
		}

		gpio_plugin_ctrl = rda_get_named_gpio(pdev,
					     "rda,plugin-ctrl-gpios",
					     "plugin_ctrl");
		if (gpio_is_valid(gpio_plugin_ctrl)) {
			gpio_request(gpio_plugin_ctrl, "plugin_ctrl");
			gpio_direction_output(gpio_plugin_ctrl, 1);
		} else {
			pr_info("can't get plugin control, maybe plugin is \
				disconnected to usbid, only for force download\n ");
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

