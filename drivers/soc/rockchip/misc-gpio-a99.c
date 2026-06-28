// SPDX-License-Identifier: GPL-2.0-only
/*
 * @sib0ndt
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/workqueue.h>

struct a99_misc {
	struct device *dev;
	struct gpio_desc *usb_3g;
	struct gpio_desc *usb_3g_wake;
	struct gpio_desc *usb_eth_led;
	struct gpio_desc *red_led;
	struct gpio_desc *func_io1;
	struct gpio_desc *func_io2;
	struct gpio_desc *mcu;
	struct delayed_work mcu_work;
	u32 heartbeat_interval_ms;
};

#define A99_MCU_PULSE_WIDTH_MS	20
#define A99_MCU_STARTUP_DELAY_MS	20000

static void a99_set_gpio(struct device *dev, const char *name,
			 struct gpio_desc *desc, int value)
{
	if (IS_ERR_OR_NULL(desc))
		return;

	gpiod_set_raw_value_cansleep(desc, value);
	dev_dbg(dev, "%s raw=%d logical=%d\n",
		name,
		gpiod_get_raw_value_cansleep(desc),
		gpiod_get_value_cansleep(desc));
}

static void a99_mcu_ping(struct a99_misc *a99)
{
	if (IS_ERR_OR_NULL(a99->mcu))
		return;

	a99_set_gpio(a99->dev, "mcu", a99->mcu, 1);
	msleep(A99_MCU_PULSE_WIDTH_MS);
	a99_set_gpio(a99->dev, "mcu", a99->mcu, 0);
}

static void a99_mcu_workfn(struct work_struct *work)
{
	struct a99_misc *a99 = container_of(to_delayed_work(work),
					     struct a99_misc, mcu_work);

	a99_set_gpio(a99->dev, "func_io1", a99->func_io1, 1);
	a99_set_gpio(a99->dev, "func_io2", a99->func_io2, 1);
	a99_set_gpio(a99->dev, "usb_3g", a99->usb_3g, 1);
	a99_set_gpio(a99->dev, "usb_3g_wake", a99->usb_3g_wake, 1);
	a99_set_gpio(a99->dev, "usb_eth_led", a99->usb_eth_led, 1);
	a99_set_gpio(a99->dev, "red_led", a99->red_led, 1);
	a99_mcu_ping(a99);

	if (a99->heartbeat_interval_ms)
		schedule_delayed_work(&a99->mcu_work,
				      msecs_to_jiffies(a99->heartbeat_interval_ms));
}

static ssize_t mcu_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);

	if (IS_ERR_OR_NULL(a99->mcu))
		return sysfs_emit(buf, "-1\n");

	return sysfs_emit(buf, "%d\n", gpiod_get_value_cansleep(a99->mcu));
}

static ssize_t mcu_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);
	bool pulse;

	if (IS_ERR_OR_NULL(a99->mcu))
		return -ENODEV;

	if (kstrtobool(buf, &pulse))
		return -EINVAL;

	if (pulse)
		a99_mcu_ping(a99);
	else
		a99_set_gpio(dev, "mcu", a99->mcu, 0);

	return count;
}

static DEVICE_ATTR_RW(mcu);

static int a99_request_named_gpio(struct device *dev,
				  const char *prop,
				  const char *label,
				  enum gpiod_flags flags,
				  struct gpio_desc **out_desc)
{
	struct gpio_desc *desc;

	desc = devm_gpiod_get_optional(dev, prop, flags);
	if (IS_ERR(desc))
		return dev_err_probe(dev, PTR_ERR(desc),
				     "failed to get %s\n", prop);
	*out_desc = desc;
	return 0;
}

static void a99_misc_cancel(void *data)
{
	struct a99_misc *a99 = data;

	cancel_delayed_work_sync(&a99->mcu_work);
}

static int misc_gpio_a99_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a99_misc *a99;
	struct pinctrl *pinctrl;
	int ret;

	a99 = devm_kzalloc(dev, sizeof(*a99), GFP_KERNEL);
	if (!a99)
		return -ENOMEM;

	a99->dev = dev;
	platform_set_drvdata(pdev, a99);

	pinctrl = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(pinctrl) && PTR_ERR(pinctrl) != -ENODEV)
		return dev_err_probe(dev, PTR_ERR(pinctrl),
				     "failed to select default pinctrl\n");

	ret = a99_request_named_gpio(dev, "func_io1", "func_io1",
				     GPIOD_OUT_HIGH, &a99->func_io1);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, "3g", "usb_3g",
				     GPIOD_OUT_HIGH, &a99->usb_3g);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, "3g_wake", "usb_3g_wake",
				     GPIOD_OUT_HIGH, &a99->usb_3g_wake);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, "eth_led", "usb_eth_led",
				     GPIOD_OUT_HIGH, &a99->usb_eth_led);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, "red_led", "red_led",
				     GPIOD_OUT_HIGH, &a99->red_led);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, "func_io2", "func_io2",
				     GPIOD_OUT_HIGH, &a99->func_io2);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, "mcu", "mcu",
				     GPIOD_OUT_HIGH, &a99->mcu);
	if (ret)
		return ret;

	device_property_read_u32(dev, "heartbeat-interval-ms",
				 &a99->heartbeat_interval_ms);

	ret = device_create_file(dev, &dev_attr_mcu);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create mcu sysfs\n");

	INIT_DELAYED_WORK(&a99->mcu_work, a99_mcu_workfn);
	ret = devm_add_action_or_reset(dev, a99_misc_cancel, a99);
	if (ret)
		return ret;

	a99_set_gpio(dev, "mcu", a99->mcu, 0);

	if (a99->heartbeat_interval_ms)
		schedule_delayed_work(&a99->mcu_work,
				      msecs_to_jiffies(max_t(u32,
				      a99->heartbeat_interval_ms,
				      A99_MCU_STARTUP_DELAY_MS)));

	dev_info(dev, "misc-gpio-a99 initialized (heartbeat=%ums)  <- @sib0ndt\n",
		 a99->heartbeat_interval_ms);

	return 0;
}

static const struct of_device_id misc_gpio_a99_of_match[] = {
	{ .compatible = "misc-gpio-a99" },
	{ }
};
MODULE_DEVICE_TABLE(of, misc_gpio_a99_of_match);

static struct platform_driver misc_gpio_a99_driver = {
	.probe = misc_gpio_a99_probe,
	.driver = {
		.name = "misc-gpio-a99",
		.of_match_table = misc_gpio_a99_of_match,
	},
};
module_platform_driver(misc_gpio_a99_driver);

MODULE_AUTHOR("sibondt");
MODULE_DESCRIPTION("YS-A99 misc GPIO helper");
MODULE_LICENSE("GPL");
