// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal YS-A99 MCU companion driver.
 *
 * Android probes an I2C MCU at 0x16 very early in boot, shortly before the
 * internal FE1.1S USB2 hub enumerates. Linux currently only touches the MCU
 * version read path so the device can be identified during boot.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#define MCU7502_INIT_RETRIES 60
#define MCU7502_RETRY_DELAY_MS 1000

struct mcu7502 {
	struct i2c_client *client;
	struct delayed_work init_work;
	unsigned int tries;
	bool ready;
	u8 version[3];
};

static int mcu7502_read_version(struct mcu7502 *mcu, u8 *version)
{
	static const u8 cmd[7] = { 0x00, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00 };
	int ret;

	ret = i2c_master_send(mcu->client, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;
	if (ret != sizeof(cmd))
		return -EIO;

	ret = i2c_master_recv(mcu->client, version, 3);
	if (ret < 0)
		return ret;
	if (ret != 3)
		return -EIO;

	return 0;
}

static void mcu7502_init_workfn(struct work_struct *work)
{
	struct mcu7502 *mcu =
		container_of(to_delayed_work(work), struct mcu7502, init_work);
	u8 version[3];
	int ret;

	ret = mcu7502_read_version(mcu, version);
	if (!ret) {
		mcu->ready = true;
		mcu->version[0] = version[0];
		mcu->version[1] = version[1];
		mcu->version[2] = version[2];
		dev_info(&mcu->client->dev,
			 "MCU ready, version bytes %02x %02x %02x\n",
			 version[0], version[1], version[2]);
		return;
	}

	if (++mcu->tries >= MCU7502_INIT_RETRIES) {
		dev_warn(&mcu->client->dev,
			 "MCU handshake did not complete after %u tries: %d\n",
			 mcu->tries, ret);
		return;
	}

	schedule_delayed_work(&mcu->init_work,
			      msecs_to_jiffies(MCU7502_RETRY_DELAY_MS));
}

static void mcu7502_cancel(void *data)
{
	struct mcu7502 *mcu = data;

	cancel_delayed_work_sync(&mcu->init_work);
}

static int mcu7502_probe(struct i2c_client *client)
{
	struct mcu7502 *mcu;
	int ret;

	mcu = devm_kzalloc(&client->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mcu->client = client;
	i2c_set_clientdata(client, mcu);

	INIT_DELAYED_WORK(&mcu->init_work, mcu7502_init_workfn);
	ret = devm_add_action_or_reset(&client->dev, mcu7502_cancel, mcu);
	if (ret)
		return ret;

	schedule_delayed_work(&mcu->init_work, 0);
	dev_info(&client->dev, "mcu_7502 probe scheduled\n");

	return 0;
}

static const struct of_device_id mcu7502_of_match[] = {
	{ .compatible = "mcu,mcu_7502" },
	{}
};
MODULE_DEVICE_TABLE(of, mcu7502_of_match);

static struct i2c_driver mcu7502_driver = {
	.driver = {
		.name = "mcu_7502",
		.of_match_table = mcu7502_of_match,
	},
	.probe = mcu7502_probe,
};
module_i2c_driver(mcu7502_driver);

MODULE_AUTHOR("sibondt");
MODULE_DESCRIPTION("Minimal YS-A99 MCU companion driver");
MODULE_LICENSE("GPL");
