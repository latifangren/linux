// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal board helper for YS-A99 Android GPIO defaults.
 *
 * Android exposes /sys/devices/platform/misc_power_en/mcu and repeatedly
 * touches it from userspace while an external MCU is active. Reproduce the
 * minimum observable behaviour:
 *  - func_io1 high
 *  - func_io2 high
 *  - hub reset high
 *  - red_led high (Android's active-low default 0 maps to raw high)
 *  - mcu low at idle
 *  - periodic keepalive that pulses mcu high then returns it low
 *
 * Keep these as raw GPIO levels to match the vendor Android driver logs.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

struct a99_misc {
	struct device *dev;
	struct gpio_desc *usb_3g;
	struct gpio_desc *usb_3g_wake;
	struct gpio_desc *usb_host_power;
	struct gpio_desc *usb_hub_reset;
	struct gpio_desc *usb_eth_led;
	struct gpio_desc *red_led;
	struct gpio_desc *func_io1;
	struct gpio_desc *func_io2;
	struct gpio_desc *mcu;
	struct delayed_work mcu_work;
	u32 heartbeat_interval_ms;
};

#define A99_MCU_PULSE_WIDTH_MS 20
#define A99_MCU_STARTUP_DELAY_MS 20000
#define A99_MCU_DEFAULT_HEARTBEAT_MS 500
#define A99_USB_3G_POWER_CYCLE_MS 1000
#define A99_USB_3G_POST_POWER_MS 2000

static void a99_set_gpio(struct device *dev, const char *name,
			 struct gpio_desc *desc, int value)
{
	int ret;

	if (IS_ERR_OR_NULL(desc))
		return;

	ret = gpiod_direction_output_raw(desc, value);
	if (ret) {
		dev_warn(dev, "%s direction_output_raw(%d) failed: %d\n", name,
			 value, ret);
		return;
	}

	gpiod_set_raw_value_cansleep(desc, value);
	dev_dbg(dev, "%s raw=%d logical=%d requested=%d\n", name,
		gpiod_get_raw_value_cansleep(desc),
		gpiod_get_value_cansleep(desc), value);
}

static void a99_mcu_ping(struct a99_misc *a99)
{
	if (IS_ERR_OR_NULL(a99->mcu))
		return;

	a99_set_gpio(a99->dev, "mcu", a99->mcu, 1);
	msleep(A99_MCU_PULSE_WIDTH_MS);
	a99_set_gpio(a99->dev, "mcu", a99->mcu, 0);
}

static void a99_set_helper_lines(struct a99_misc *a99, int value)
{
	a99_set_gpio(a99->dev, "func_io1", a99->func_io1, value);
	a99_set_gpio(a99->dev, "func_io2", a99->func_io2, value);
	a99_set_gpio(a99->dev, "usb_3g", a99->usb_3g, value);
	a99_set_gpio(a99->dev, "usb_3g_wake", a99->usb_3g_wake, value);
	a99_set_gpio(a99->dev, "usb_host_power", a99->usb_host_power, value);
	a99_set_gpio(a99->dev, "usb_hub_reset", a99->usb_hub_reset, value);
	a99_set_gpio(a99->dev, "usb_eth_led", a99->usb_eth_led, value);
}

static void a99_power_cycle_3g(struct a99_misc *a99, bool wake_high)
{
	a99_set_gpio(a99->dev, "usb_3g_wake", a99->usb_3g_wake, 0);
	a99_set_gpio(a99->dev, "usb_3g", a99->usb_3g, 0);
	a99_set_gpio(a99->dev, "usb_host_power", a99->usb_host_power, 0);
	msleep(A99_USB_3G_POWER_CYCLE_MS);
	a99_set_gpio(a99->dev, "usb_host_power", a99->usb_host_power, 1);
	msleep(A99_USB_3G_POWER_CYCLE_MS);
	a99_set_gpio(a99->dev, "usb_3g", a99->usb_3g, 1);
	msleep(A99_USB_3G_POST_POWER_MS);
	a99_set_gpio(a99->dev, "usb_3g_wake", a99->usb_3g_wake, wake_high);
}

static void a99_bootstrap_usb_helper(struct a99_misc *a99)
{
	/* Match the vendor Android steady state before EHCI probes the hub. */
	a99_set_helper_lines(a99, 1);
	a99_power_cycle_3g(a99, true);
	a99_set_gpio(a99->dev, "mcu", a99->mcu, 0);
	a99_set_gpio(a99->dev, "red_led", a99->red_led, 1);
	a99_mcu_ping(a99);
}

static void a99_shutdown_usb_helper(struct a99_misc *a99)
{
	cancel_delayed_work_sync(&a99->mcu_work);
	a99_set_helper_lines(a99, 1);
	a99_set_gpio(a99->dev, "red_led", a99->red_led, 1);
	a99_set_gpio(a99->dev, "mcu", a99->mcu, 0);
}

static void a99_mcu_workfn(struct work_struct *work)
{
	struct a99_misc *a99 =
		container_of(to_delayed_work(work), struct a99_misc, mcu_work);

	a99_set_gpio(a99->dev, "func_io1", a99->func_io1, 1);
	a99_set_gpio(a99->dev, "func_io2", a99->func_io2, 1);
	a99_set_gpio(a99->dev, "usb_host_power", a99->usb_host_power, 1);
	a99_set_gpio(a99->dev, "usb_hub_reset", a99->usb_hub_reset, 1);
	a99_set_gpio(a99->dev, "usb_eth_led", a99->usb_eth_led, 1);
	a99_set_gpio(a99->dev, "red_led", a99->red_led, 1);
	a99_mcu_ping(a99);

	if (a99->heartbeat_interval_ms)
		schedule_delayed_work(
			&a99->mcu_work,
			msecs_to_jiffies(a99->heartbeat_interval_ms));
}

static ssize_t mcu_show(struct device *dev, struct device_attribute *attr,
			char *buf)
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

static ssize_t usb_left_sequence_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(
		buf,
		"write left-host or left-host-wake-low to pulse the YS-A99 USB helper sequence\n");
}

static ssize_t usb_left_sequence_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);

	if (!sysfs_streq(buf, "left-host") &&
	    !sysfs_streq(buf, "left-host-wake-low"))
		return -EINVAL;

	a99_set_gpio(dev, "usb_hub_reset", a99->usb_hub_reset, 1);
	a99_set_gpio(dev, "func_io1", a99->func_io1, 1);
	a99_set_gpio(dev, "func_io2", a99->func_io2, 1);
	a99_set_gpio(dev, "usb_eth_led", a99->usb_eth_led, 1);
	a99_power_cycle_3g(a99, sysfs_streq(buf, "left-host"));
	a99_mcu_ping(a99);

	return count;
}

static DEVICE_ATTR_RW(usb_left_sequence);

static ssize_t usb_3g_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);

	if (IS_ERR_OR_NULL(a99->usb_3g))
		return sysfs_emit(buf, "-1\n");

	return sysfs_emit(buf, "%d\n",
			  gpiod_get_raw_value_cansleep(a99->usb_3g));
}

static ssize_t usb_3g_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);
	bool value;

	if (IS_ERR_OR_NULL(a99->usb_3g))
		return -ENODEV;

	if (kstrtobool(buf, &value))
		return -EINVAL;

	a99_set_gpio(dev, "usb_3g", a99->usb_3g, value);
	return count;
}

static DEVICE_ATTR_RW(usb_3g);

static ssize_t usb_3g_wake_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);

	if (IS_ERR_OR_NULL(a99->usb_3g_wake))
		return sysfs_emit(buf, "-1\n");

	return sysfs_emit(buf, "%d\n",
			  gpiod_get_raw_value_cansleep(a99->usb_3g_wake));
}

static ssize_t usb_3g_wake_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);
	bool value;

	if (IS_ERR_OR_NULL(a99->usb_3g_wake))
		return -ENODEV;

	if (kstrtobool(buf, &value))
		return -EINVAL;

	a99_set_gpio(dev, "usb_3g_wake", a99->usb_3g_wake, value);
	return count;
}

static DEVICE_ATTR_RW(usb_3g_wake);

static ssize_t usb_host_power_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);

	if (IS_ERR_OR_NULL(a99->usb_host_power))
		return sysfs_emit(buf, "-1\n");

	return sysfs_emit(buf, "%d\n",
			  gpiod_get_raw_value_cansleep(a99->usb_host_power));
}

static ssize_t usb_host_power_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct a99_misc *a99 = dev_get_drvdata(dev);
	bool value;

	if (IS_ERR_OR_NULL(a99->usb_host_power))
		return -ENODEV;

	if (kstrtobool(buf, &value))
		return -EINVAL;

	a99_set_gpio(dev, "usb_host_power", a99->usb_host_power, value);
	return count;
}

static DEVICE_ATTR_RW(usb_host_power);

static void a99_remove_mcu_attr(void *data)
{
	struct device *dev = data;

	device_remove_file(dev, &dev_attr_usb_host_power);
	device_remove_file(dev, &dev_attr_usb_3g_wake);
	device_remove_file(dev, &dev_attr_usb_3g);
	device_remove_file(dev, &dev_attr_usb_left_sequence);
	device_remove_file(dev, &dev_attr_mcu);
}

static int a99_request_named_gpio(struct device *dev, struct device_node *np,
				  const char *prop, const char *label,
				  int init_value, struct gpio_desc **out_desc)
{
	struct gpio_desc *desc;
	int ret;
	int gpio;

	gpio = of_get_named_gpio(np, prop, 0);
	if (gpio == -ENOENT || gpio == -EINVAL) {
		*out_desc = NULL;
		return 0;
	}
	if (gpio < 0)
		return dev_err_probe(dev, gpio, "failed to read %s\n", prop);

	ret = devm_gpio_request(dev, gpio, label);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request gpio %d\n",
				     gpio);

	desc = gpio_to_desc(gpio);
	if (!desc)
		return dev_err_probe(dev, -EINVAL,
				     "failed to map gpio %d to descriptor\n",
				     gpio);

	*out_desc = desc;
	a99_set_gpio(dev, label, desc, init_value);
	return 0;
}

static void a99_misc_cancel(void *data)
{
	struct a99_misc *a99 = data;

	cancel_delayed_work_sync(&a99->mcu_work);
}

static void misc_gpio_a99_shutdown(struct platform_device *pdev)
{
	struct a99_misc *a99 = platform_get_drvdata(pdev);

	if (!a99)
		return;

	a99_shutdown_usb_helper(a99);
}

static int misc_gpio_a99_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
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

	ret = a99_request_named_gpio(dev, np, "func_io1,gpio", "func_io1", 1,
				     &a99->func_io1);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "3g,gpio", "usb_3g", 1,
				     &a99->usb_3g);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "3g_wake,gpio", "usb_3g_wake", 1,
				     &a99->usb_3g_wake);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "host_power,gpio",
				     "usb_host_power", 1, &a99->usb_host_power);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "hub_reset,gpio", "usb_hub_reset",
				     1, &a99->usb_hub_reset);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "eth_led,gpio", "usb_eth_led", 1,
				     &a99->usb_eth_led);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "red_led,gpio", "red_led", 1,
				     &a99->red_led);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "func_io2,gpio", "func_io2", 1,
				     &a99->func_io2);
	if (ret)
		return ret;

	ret = a99_request_named_gpio(dev, np, "mcu,gpio", "mcu", 0, &a99->mcu);
	if (ret)
		return ret;

	if (device_property_read_u32(dev, "heartbeat-interval-ms",
				     &a99->heartbeat_interval_ms))
		a99->heartbeat_interval_ms = A99_MCU_DEFAULT_HEARTBEAT_MS;

	ret = device_create_file(dev, &dev_attr_mcu);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create mcu sysfs\n");

	ret = device_create_file(dev, &dev_attr_usb_left_sequence);
	if (ret)
		return dev_err_probe(
			dev, ret, "failed to create usb_left_sequence sysfs\n");

	ret = device_create_file(dev, &dev_attr_usb_3g);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to create usb_3g sysfs\n");

	ret = device_create_file(dev, &dev_attr_usb_3g_wake);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to create usb_3g_wake sysfs\n");

	ret = device_create_file(dev, &dev_attr_usb_host_power);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to create usb_host_power sysfs\n");

	ret = devm_add_action_or_reset(dev, a99_remove_mcu_attr, dev);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&a99->mcu_work, a99_mcu_workfn);
	ret = devm_add_action_or_reset(dev, a99_misc_cancel, a99);
	if (ret)
		return ret;

	a99_bootstrap_usb_helper(a99);

	if (a99->heartbeat_interval_ms)
		schedule_delayed_work(
			&a99->mcu_work,
			msecs_to_jiffies(max_t(u32, a99->heartbeat_interval_ms,
					       A99_MCU_STARTUP_DELAY_MS)));

	dev_info(dev, "misc-gpio-a99 initialized (heartbeat=%ums)\n",
		 a99->heartbeat_interval_ms);

	return 0;
}

static const struct of_device_id misc_gpio_a99_of_match[] = {
	{ .compatible = "misc-gpio-a99" },
	{}
};
MODULE_DEVICE_TABLE(of, misc_gpio_a99_of_match);

static struct platform_driver misc_gpio_a99_driver = {
	.probe = misc_gpio_a99_probe,
	.shutdown = misc_gpio_a99_shutdown,
	.driver = {
		.name = "misc-gpio-a99",
		.of_match_table = misc_gpio_a99_of_match,
	},
};
module_platform_driver(misc_gpio_a99_driver);

MODULE_AUTHOR("sibondt");
MODULE_DESCRIPTION("YS-A99 misc GPIO helper");
MODULE_LICENSE("GPL");
