// SPDX-License-Identifier: GPL-2.0-only
/*
 * RDA I2C master controller driver
 *
 * Ported from vendor kernel and adapted for newer kernels/DT boot.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <mach/regulator.h>
#include <linux/soc/rda/i2c.h>

#ifndef RDA_CLK_APB1
#define RDA_CLK_APB1 "apb1"
#endif

struct rda_i2c_device_data {
	u32 speed;
};

#define HAL_I2C_OPERATE_TIME_MS 200
#define RDA_I2C_DEFAULT_BUS_CLK 200000000UL

#ifndef RDA_I2C_DRV_NAME
#define RDA_I2C_DRV_NAME "rda-i2c"
#endif

#define HAL_ERR_RESOURCE_TIMEOUT     1
#define HAL_ERR_COMMUNICATION_FAILED 3

struct rda_i2c_dev {
	struct device *dev;
	void __iomem *base;
	struct clk *master_clk;
	unsigned long bus_clk_rate;
	u32 speed_khz;
	struct i2c_adapter adapter;
	struct regulator *i2c_regulator;
};

static void hal_i2c_set_clock(struct rda_i2c_dev *dev)
{
	unsigned long mclk = dev->bus_clk_rate;
	unsigned long clock = dev->speed_khz * 1000;
	unsigned long clk_div;
	unsigned long ctrl;
	HWP_I2C_MASTER_T *i2c = (HWP_I2C_MASTER_T *)dev->base;

	if (!mclk)
		mclk = RDA_I2C_DEFAULT_BUS_CLK;

	clk_div = mclk / (5 * clock);
	if (mclk % (5 * clock))
		clk_div++;
	if (clk_div >= 1)
		clk_div--;
	if (clk_div > 0xffff)
		clk_div = 0xffff;

	ctrl = i2c->CTRL & ~I2C_MASTER_CLOCK_PRESCALE_MASK;
	ctrl |= I2C_MASTER_CLOCK_PRESCALE(clk_div);

	i2c->CTRL = 0;
	i2c->CTRL = ctrl;

	dev_dbg(dev->dev, "set clk = %u KHz, bus_clk = %lu, divider = %lu\n",
		 dev->speed_khz, mclk, clk_div);
}

static int hal_i2c_stop(struct rda_i2c_dev *dev)
{
	unsigned long timeout;
	HWP_I2C_MASTER_T *i2c = (HWP_I2C_MASTER_T *)dev->base;

	i2c->CMD = I2C_MASTER_STO;

	timeout = jiffies + msecs_to_jiffies(HAL_I2C_OPERATE_TIME_MS);
	while (!(i2c->STATUS & I2C_MASTER_IRQ_STATUS)) {
		if (time_after(jiffies, timeout)) {
			dev_err(dev->dev, "%s timeout\n", __func__);
			return HAL_ERR_RESOURCE_TIMEOUT;
		}
	}

	i2c->IRQ_CLR = I2C_MASTER_IRQ_CLR;
	return 0;
}

static int hal_i2c_raw_send_byte(struct rda_i2c_dev *dev, u8 data,
					 int start, int stop)
{
	unsigned long timeout;
	u32 cmd = I2C_MASTER_WR;
	HWP_I2C_MASTER_T *i2c = (HWP_I2C_MASTER_T *)dev->base;

	if (start)
		cmd |= I2C_MASTER_STA;
	if (stop)
		cmd |= I2C_MASTER_STO;

	i2c->TXRX_BUFFER = data;
	i2c->CMD = cmd;

	timeout = jiffies + msecs_to_jiffies(HAL_I2C_OPERATE_TIME_MS);
	while (!(i2c->STATUS & I2C_MASTER_IRQ_STATUS)) {
		if (time_after(jiffies, timeout)) {
			i2c->CMD = I2C_MASTER_STO;
			return HAL_ERR_RESOURCE_TIMEOUT;
		}
	}

	i2c->IRQ_CLR = I2C_MASTER_IRQ_CLR;

	timeout = jiffies + msecs_to_jiffies(HAL_I2C_OPERATE_TIME_MS);
	while (i2c->STATUS & I2C_MASTER_RXACK) {
		if (time_after(jiffies, timeout)) {
			hal_i2c_stop(dev);
			return HAL_ERR_COMMUNICATION_FAILED;
		}
	}

	return 0;
}

static int hal_i2c_raw_get_byte(struct rda_i2c_dev *dev, u8 *data,
					int start, int stop)
{
	unsigned long timeout;
	u32 cmd = I2C_MASTER_RD;
	HWP_I2C_MASTER_T *i2c = (HWP_I2C_MASTER_T *)dev->base;

	if (start)
		cmd |= I2C_MASTER_STA;
	if (stop)
		cmd |= I2C_MASTER_ACK | I2C_MASTER_STO;

	i2c->CMD = cmd;

	timeout = jiffies + msecs_to_jiffies(HAL_I2C_OPERATE_TIME_MS);
	while (!(i2c->STATUS & I2C_MASTER_IRQ_STATUS)) {
		if (time_after(jiffies, timeout)) {
			i2c->CMD = I2C_MASTER_STO;
			return HAL_ERR_RESOURCE_TIMEOUT;
		}
	}

	i2c->IRQ_CLR = I2C_MASTER_IRQ_CLR;
	*data = (u8)(i2c->TXRX_BUFFER & 0xff);

	return 0;
}

static void hal_i2c_enable(struct rda_i2c_dev *dev)
{
	HWP_I2C_MASTER_T *i2c = (HWP_I2C_MASTER_T *)dev->base;

	i2c->CTRL |= I2C_MASTER_EN;
}

static int rda_i2c_init(struct rda_i2c_dev *dev)
{
	hal_i2c_set_clock(dev);
	hal_i2c_enable(dev);
	return 0;
}

static void rda_i2c_idle(struct rda_i2c_dev *dev)
{
	hal_i2c_stop(dev);
}

static int rda_i2c_send_bytes(struct rda_i2c_dev *dev, u8 addr,
			      u8 *data, int len, bool stop)
{
	int ret;
	int i;

	if (len < 1)
		return -EINVAL;

	dev_dbg(dev->dev, "%s, addr=0x%x len=%d stop=%d\n", __func__, addr,
		len, stop);

	ret = hal_i2c_raw_send_byte(dev, (addr << 1) & 0xfe, 1, 0);
	if (ret)
		return ret;

	for (i = 0; i < len - 1; i++) {
		ret = hal_i2c_raw_send_byte(dev, data[i], 0, 0);
		if (ret)
			return ret;
	}

	ret = hal_i2c_raw_send_byte(dev, data[len - 1], 0, stop);
	if (ret)
		return ret;

	return 0;
}

static int rda_i2c_get_bytes(struct rda_i2c_dev *dev, u8 addr,
			     u8 *data, int len, bool stop)
{
	int ret;
	int i;

	if (len < 1)
		return -EINVAL;

	dev_dbg(dev->dev, "%s, addr=0x%x len=%d stop=%d\n", __func__, addr,
		len, stop);

	ret = hal_i2c_raw_send_byte(dev, (addr << 1) | 0x01, 1, 0);
	if (ret)
		return ret;

	for (i = 0; i < len - 1; i++) {
		ret = hal_i2c_raw_get_byte(dev, &data[i], 0, 0);
		if (ret)
			return ret;
	}

	ret = hal_i2c_raw_get_byte(dev, &data[len - 1], 0, stop);
	if (ret)
		return ret;

	return 0;
}

static int rda_i2c_xfer_msg(struct i2c_adapter *adap, struct i2c_msg *msg,
			    bool stop)
{
	struct rda_i2c_dev *dev = i2c_get_adapdata(adap);
	int ret = 0;
	int retry;

	for (retry = 0; retry < adap->retries; retry++) {
		if (!(msg->flags & I2C_M_RD))
			ret = rda_i2c_send_bytes(dev, msg->addr, msg->buf, msg->len,
						 stop);
		else
			ret = rda_i2c_get_bytes(dev, msg->addr, msg->buf, msg->len,
						stop);

		if (!ret)
			break;
	}

	return ret;
}

static int rda_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			int num)
{
	struct rda_i2c_dev *dev = i2c_get_adapdata(adap);
	int i;
	int ret = 0;

	for (i = 0; i < num; i++) {
		ret = rda_i2c_xfer_msg(adap, &msgs[i], i == (num - 1));
		if (ret) {
			ret = -EIO;
			break;
		}
	}

	if (!ret)
		return num;

	rda_i2c_idle(dev);
	return ret;
}

static u32 rda_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm rda_i2c_algo = {
	.master_xfer = rda_i2c_xfer,
	.functionality = rda_i2c_func,
};

static int rda_i2c_probe(struct platform_device *pdev)
{
	struct rda_i2c_dev *dev;
	struct i2c_adapter *adap;
	struct resource *mem;
	struct rda_i2c_device_data *pdata;
	int bus_id;
	int ret;
	u32 speed_hz = 0;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -ENODEV;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;

	if (pdev->dev.of_node) {
		if (!of_property_read_u32(pdev->dev.of_node, "clock-frequency",
						 &speed_hz))
			dev->speed_khz = speed_hz / 1000;
		if (!dev->speed_khz)
			dev->speed_khz = 100;
		bus_id = of_alias_get_id(pdev->dev.of_node, "i2c");
	} else {
		pdata = dev_get_platdata(&pdev->dev);
		dev->speed_khz = pdata ? pdata->speed : 100;
		bus_id = pdev->id;
	}

	dev->i2c_regulator = regulator_get_optional(NULL, LDO_I2C);
	if (!IS_ERR(dev->i2c_regulator)) {
		ret = regulator_enable(dev->i2c_regulator);
		if (ret)
			dev_warn(&pdev->dev, "failed to enable %s regulator: %d\n",
				 LDO_I2C, ret);
	} else {
		ret = PTR_ERR(dev->i2c_regulator);
		if (ret != -ENODEV && ret != -ENOENT)
			dev_warn(&pdev->dev, "failed to get %s regulator: %d\n",
				 LDO_I2C, ret);
		dev->i2c_regulator = NULL;
	}

	dev->master_clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(dev->master_clk))
		dev->master_clk = clk_get(NULL, RDA_CLK_APB1);
	if (IS_ERR(dev->master_clk)) {
		ret = PTR_ERR(dev->master_clk);
		dev_warn(&pdev->dev,
			 "no clock handler for %s: %d, fallback to fixed %lu Hz\n",
			 RDA_CLK_APB1, ret, RDA_I2C_DEFAULT_BUS_CLK);
		dev->master_clk = NULL;
		dev->bus_clk_rate = RDA_I2C_DEFAULT_BUS_CLK;
	} else {
		ret = clk_prepare_enable(dev->master_clk);
		if (ret) {
			dev_warn(&pdev->dev,
				 "failed to enable %s: %d, fallback to fixed %lu Hz\n",
				 RDA_CLK_APB1, ret, RDA_I2C_DEFAULT_BUS_CLK);
			clk_put(dev->master_clk);
			dev->master_clk = NULL;
			dev->bus_clk_rate = RDA_I2C_DEFAULT_BUS_CLK;
		} else {
			dev->bus_clk_rate = clk_get_rate(dev->master_clk);
		}
	}

	dev->base = ioremap(mem->start, resource_size(mem));
	if (!dev->base) {
		ret = -ENOMEM;
		goto err_put_clk;
	}

	platform_set_drvdata(pdev, dev);

	rda_i2c_init(dev);
	rda_i2c_idle(dev);

	adap = &dev->adapter;
	i2c_set_adapdata(adap, dev);
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_HWMON;
	strscpy(adap->name, "RDA I2C adapter", sizeof(adap->name));
	adap->algo = &rda_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;
	adap->retries = 3;

	if (bus_id < 0)
		bus_id = pdev->id;

	if (bus_id >= 0) {
		adap->nr = bus_id;
		ret = i2c_add_numbered_adapter(adap);
	} else {
		ret = i2c_add_adapter(adap);
	}
	if (ret) {
		dev_err(&pdev->dev, "failure adding adapter: %d\n", ret);
		goto err_unmap;
	}

	dev_dbg(&pdev->dev, "rda_i2c, adapter %d, at %u kHz\n", adap->nr,
		 dev->speed_khz);

	return 0;

err_unmap:
	rda_i2c_idle(dev);
	platform_set_drvdata(pdev, NULL);
	iounmap(dev->base);
err_put_clk:
	if (dev->master_clk) {
		clk_disable_unprepare(dev->master_clk);
		clk_put(dev->master_clk);
	}
	if (dev->i2c_regulator) {
		regulator_disable(dev->i2c_regulator);
		regulator_put(dev->i2c_regulator);
	}
	kfree(dev);

	return ret;
}

static void rda_i2c_remove(struct platform_device *pdev)
{
	struct rda_i2c_dev *dev = platform_get_drvdata(pdev);

	if (!dev)
		return;

	platform_set_drvdata(pdev, NULL);
	i2c_del_adapter(&dev->adapter);
	rda_i2c_idle(dev);
	iounmap(dev->base);
	if (dev->master_clk) {
		clk_disable_unprepare(dev->master_clk);
		clk_put(dev->master_clk);
	}
	if (dev->i2c_regulator) {
		regulator_disable(dev->i2c_regulator);
		regulator_put(dev->i2c_regulator);
	}
	kfree(dev);
}

static const struct of_device_id rda_i2c_of_match[] = {
	{ .compatible = "rda,8810pl-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, rda_i2c_of_match);

static struct platform_driver rda_i2c_driver = {
	.probe = rda_i2c_probe,
	.remove = rda_i2c_remove,
	.driver = {
		.name = RDA_I2C_DRV_NAME,
		.of_match_table = rda_i2c_of_match,
	},
};

module_platform_driver(rda_i2c_driver);

MODULE_ALIAS("platform:rda-i2c");
MODULE_AUTHOR("Wang Lei <leiwang@rdamicro.com>");
MODULE_DESCRIPTION("RDA I2C bus");
MODULE_LICENSE("GPL");