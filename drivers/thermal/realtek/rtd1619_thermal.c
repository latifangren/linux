// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTD1619 thermal sensor driver.
 *
 * Based on Realtek vendor register programming for RTD1619 thermal sensor.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define RTD1619_TM_SENSOR_CTRL0		0x00
#define RTD1619_TM_SENSOR_CTRL1		0x04
#define RTD1619_TM_SENSOR_CTRL2		0x08
#define RTD1619_TM_SENSOR_STATUS0	0x40
#define RTD1619_TM_SENSOR_STATUS1	0x44

#define RTD1619_TM_SENSOR_RSTB		BIT(17)

struct rtd1619_thermal {
	struct device *dev;
	void __iomem *base;
	struct thermal_zone_device *tz;
};

static void rtd1619_thermal_reset(struct rtd1619_thermal *data)
{
	u32 val;

	val = readl(data->base + RTD1619_TM_SENSOR_CTRL2);
	writel(val & ~RTD1619_TM_SENSOR_RSTB,
	       data->base + RTD1619_TM_SENSOR_CTRL2);
	writel(val | RTD1619_TM_SENSOR_RSTB,
	       data->base + RTD1619_TM_SENSOR_CTRL2);
	msleep(5);
}

static void rtd1619_thermal_init_hw(struct rtd1619_thermal *data)
{
	writel(0x07ce7ae1, data->base + RTD1619_TM_SENSOR_CTRL0);
	writel(0x00378228, data->base + RTD1619_TM_SENSOR_CTRL1);
	writel(0x00011114, data->base + RTD1619_TM_SENSOR_CTRL2);
	rtd1619_thermal_reset(data);
}

static int rtd1619_thermal_raw_to_mcelsius(u32 val)
{
	int temp;

	temp = sign_extend32(val, 18);

	return temp * 1000 / 1024;
}

static int rtd1619_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct rtd1619_thermal *data = thermal_zone_device_priv(tz);
	int t;

	t = rtd1619_thermal_raw_to_mcelsius(readl(data->base + RTD1619_TM_SENSOR_STATUS0));
	if (t < -3000 || t > 150000) {
		dev_dbg(data->dev, "reset: temp=%d status={%08x, %08x}\n",
			t, readl(data->base + RTD1619_TM_SENSOR_STATUS0),
			readl(data->base + RTD1619_TM_SENSOR_STATUS1));
		rtd1619_thermal_reset(data);
		t = rtd1619_thermal_raw_to_mcelsius(readl(data->base + RTD1619_TM_SENSOR_STATUS0));
	}

	*temp = t;

	return 0;
}

static const struct thermal_zone_device_ops rtd1619_thermal_ops = {
	.get_temp = rtd1619_thermal_get_temp,
};

static int rtd1619_thermal_probe(struct platform_device *pdev)
{
	struct rtd1619_thermal *data;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);

	rtd1619_thermal_init_hw(data);

	data->tz = devm_thermal_of_zone_register(&pdev->dev, 0, data,
						    &rtd1619_thermal_ops);
	if (IS_ERR(data->tz))
		return PTR_ERR(data->tz);

	platform_set_drvdata(pdev, data);

	return 0;
}

static int rtd1619_thermal_resume(struct device *dev)
{
	struct rtd1619_thermal *data = dev_get_drvdata(dev);

	rtd1619_thermal_init_hw(data);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rtd1619_thermal_pm_ops, NULL,
					 rtd1619_thermal_resume);

static const struct of_device_id rtd1619_thermal_of_match[] = {
	{ .compatible = "realtek,rtd1619-thermal-sensor" },
	{ .compatible = "realtek,rtd1619-thermal-sensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd1619_thermal_of_match);

static struct platform_driver rtd1619_thermal_driver = {
	.probe = rtd1619_thermal_probe,
	.driver = {
		.name = "rtd1619-thermal",
		.of_match_table = rtd1619_thermal_of_match,
		.pm = pm_sleep_ptr(&rtd1619_thermal_pm_ops),
	},
};
module_platform_driver(rtd1619_thermal_driver);

MODULE_DESCRIPTION("Realtek RTD1619 thermal sensor driver");
MODULE_AUTHOR("Realtek Semiconductor Corporation");
MODULE_LICENSE("GPL");
