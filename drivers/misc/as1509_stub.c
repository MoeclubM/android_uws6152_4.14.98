// SPDX-License-Identifier: GPL-2.0
/*
 * as1509 stub for UWS6152 (i2c@70500000 as1509@19)
 * Stock DTS: compatible="as,as1509_i2c" reg <0x19> as,irq-gpio=<&gpio 15 1>
 *           key-code-up=0x41 down=0x42 front=0x3d back=0x3e
 * This stub only claims the node and exposes the crown as input keys.
 * No closed sensor HAL.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>

struct as1509_stub {
	struct i2c_client *client;
	struct input_dev *input;
	int irq;
	int codes[4];
};

static irqreturn_t as1509_isr(int irq, void *dev_id)
{
	struct as1509_stub *st = dev_id;
	/* Minimal: report a dummy press of "up" on irq; real decode needs spec.
	 * This at least proves the node is bound and input path works.
	 */
	input_report_key(st->input, st->codes[0], 1);
	input_sync(st->input);
	input_report_key(st->input, st->codes[0], 0);
	input_sync(st->input);
	return IRQ_HANDLED;
}

static int as1509_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct as1509_stub *st;
	int gpio, ret;

	dev_info(dev, "as1509 stub probe (UWS6152 w527)\n");
	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->client = client;

	if (of_property_read_u32(dev->of_node, "key-code-up", &st->codes[0]))
		st->codes[0] = 0x41;
	if (of_property_read_u32(dev->of_node, "key-code-down", &st->codes[1]))
		st->codes[1] = 0x42;
	if (of_property_read_u32(dev->of_node, "key-code-front", &st->codes[2]))
		st->codes[2] = 0x3d;
	if (of_property_read_u32(dev->of_node, "key-code-back", &st->codes[3]))
		st->codes[3] = 0x3e;

	st->input = devm_input_allocate_device(dev);
	if (!st->input)
		return -ENOMEM;
	st->input->name = "as1509-crown-stub";
	st->input->id.bustype = BUS_I2C;
	__set_bit(EV_KEY, st->input->evbit);
	__set_bit(st->codes[0], st->input->keybit);
	__set_bit(st->codes[1], st->input->keybit);
	__set_bit(st->codes[2], st->input->keybit);
	__set_bit(st->codes[3], st->input->keybit);
	ret = input_register_device(st->input);
	if (ret)
		return ret;

	gpio = of_get_named_gpio(dev->of_node, "as,irq-gpio", 0);
	if (gpio >= 0) {
		ret = devm_gpio_request_one(dev, gpio, GPIOF_IN, "as1509-irq");
		if (!ret) {
			st->irq = gpio_to_irq(gpio);
			if (st->irq > 0) {
				ret = devm_request_threaded_irq(dev, st->irq, NULL,
								as1509_isr, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
								"as1509-stub", st);
				if (ret)
					dev_warn(dev, "irq request failed %d\n", ret);
			}
		}
	}

	i2c_set_clientdata(client, st);
	dev_info(dev, "as1509 stub codes %02x %02x %02x %02x irq %d\n",
		 st->codes[0], st->codes[1], st->codes[2], st->codes[3], st->irq);
	return 0;
}

static const struct of_device_id as1509_of_match[] = {
	{ .compatible = "as,as1509_i2c" },
	{}
};
MODULE_DEVICE_TABLE(of, as1509_of_match);

static const struct i2c_device_id as1509_id[] = {
	{ "as1509_i2c", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, as1509_id);

static struct i2c_driver as1509_driver = {
	.driver = {
		.name = "as1509-stub",
		.of_match_table = as1509_of_match,
	},
	.probe = as1509_probe,
	.id_table = as1509_id,
};
module_i2c_driver(as1509_driver);
MODULE_AUTHOR("Kokuban");
MODULE_DESCRIPTION("UWS6152 as1509 crown stub");
MODULE_LICENSE("GPL");
