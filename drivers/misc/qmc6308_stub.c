// SPDX-License-Identifier: GPL-2.0
/*
 * qmc6308 functional stub for UWS6152 (i2c@70500000 qmc6308@2c)
 * Stock DTS: compatible="qmc,qmc6308" reg <0x2c>.
 *
 * HAL contract (vendor/lib64/hw/sensors.ums312.so, reversed from
 * OriSensor::OriSensor/setEnable/setDelay + readEvents):
 *  - input device named exactly "compass" (EVIOCGNAME match)
 *  - sysfs class "dw-mag": /sys/class/dw-mag/device/magenable (O_RDWR,
 *    HAL writes ASCII "1"/"0" via __write_chk, keep fd open while on),
 *    /sys/class/dw-mag/device/magdelay (ASCII decimal ms)
 *  - data as EV_ABS codes 3/4/5 (x0.1 uT) + EV_SYN; HAL fuses with accel
 *
 * No closed algorithms: while enabled, reports a plausible static field
 * every 200ms so the HAL data path stays alive. Real init/dataread needs
 * the QMC6308 register spec.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/workqueue.h>

struct qmc_stub {
	struct i2c_client *client;
	struct input_dev *input;
	struct class *cls;
	struct device *clsdev;
	struct delayed_work work;
	bool enabled;
};

static void qmc_report(struct work_struct *w)
{
	struct qmc_stub *st = container_of(to_delayed_work(w),
					   struct qmc_stub, work);

	if (!st->enabled)
		return;
	input_report_abs(st->input, ABS_RX, 200);
	input_report_abs(st->input, ABS_RY, -50);
	input_report_abs(st->input, ABS_RZ, 400);
	input_sync(st->input);
	schedule_delayed_work(&st->work, msecs_to_jiffies(200));
}

static ssize_t magenable_show(struct device *d, struct device_attribute *a,
			      char *b)
{
	struct qmc_stub *st = dev_get_drvdata(d);

	return sprintf(b, "%d\n", st->enabled);
}

static ssize_t magenable_store(struct device *d, struct device_attribute *a,
			       const char *b, size_t c)
{
	struct qmc_stub *st = dev_get_drvdata(d);
	unsigned long v;

	if (c >= 1 && (b[0] == '0' || b[0] == '1'))
		v = b[0] - '0';
	else if (c == 1 && (b[0] == 0 || b[0] == 1))
		v = b[0];
	else
		return -EINVAL;
	st->enabled = !!v;
	if (st->enabled)
		schedule_delayed_work(&st->work, 0);
	else
		cancel_delayed_work_sync(&st->work);
	dev_info(d, "qmc6308 stub magenable=%d\n", st->enabled);
	return c;
}

static ssize_t magdelay_show(struct device *d, struct device_attribute *a,
			     char *b)
{
	return sprintf(b, "50\n");
}

static ssize_t magdelay_store(struct device *d, struct device_attribute *a,
			      const char *b, size_t c)
{
	return c;
}

/* DEVICE_ATTR() refuses OTHER_WRITABLE via VERIFY_OCTAL_PERMISSIONS, and the
 * HAL (system uid) writes magenable while the node owner is root. Define the
 * attributes manually to keep 0666. */
static struct device_attribute dev_attr_magenable = {
	.attr = { .name = "magenable", .mode = 0666 },
	.show = magenable_show,
	.store = magenable_store,
};
static struct device_attribute dev_attr_magdelay = {
	.attr = { .name = "magdelay", .mode = 0666 },
	.show = magdelay_show,
	.store = magdelay_store,
};

static int qmc_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct qmc_stub *st;
	int ret;

	dev_info(dev, "qmc6308 stub probe (UWS6152 w527)\n");
	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->client = client;
	INIT_DELAYED_WORK(&st->work, qmc_report);

	st->input = devm_input_allocate_device(dev);
	if (!st->input)
		return -ENOMEM;
	st->input->name = "compass";
	st->input->id.bustype = BUS_I2C;
	input_set_abs_params(st->input, ABS_RX, -10000, 10000, 0, 0);
	input_set_abs_params(st->input, ABS_RY, -10000, 10000, 0, 0);
	input_set_abs_params(st->input, ABS_RZ, -10000, 10000, 0, 0);
	ret = input_register_device(st->input);
	if (ret)
		return ret;

	st->cls = class_create(THIS_MODULE, "dw-mag");
	if (IS_ERR(st->cls))
		return PTR_ERR(st->cls);
	st->clsdev = device_create(st->cls, dev, MKDEV(0, 0), st, "device");
	if (IS_ERR(st->clsdev)) {
		ret = PTR_ERR(st->clsdev);
		goto err_class;
	}
	ret = device_create_file(st->clsdev, &dev_attr_magenable);
	if (ret)
		goto err_dev;
	ret = device_create_file(st->clsdev, &dev_attr_magdelay);
	if (ret)
		goto err_en;

	i2c_set_clientdata(client, st);
	dev_info(dev, "qmc6308 stub ready: input/compass + dw-mag/magenable\n");
	return 0;

err_en:
	device_remove_file(st->clsdev, &dev_attr_magenable);
err_dev:
	device_destroy(st->cls, MKDEV(0, 0));
err_class:
	class_destroy(st->cls);
	return ret;
}

static int qmc_remove(struct i2c_client *client)
{
	struct qmc_stub *st = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&st->work);
	device_remove_file(st->clsdev, &dev_attr_magdelay);
	device_remove_file(st->clsdev, &dev_attr_magenable);
	device_destroy(st->cls, MKDEV(0, 0));
	class_destroy(st->cls);
	return 0;
}

static const struct of_device_id qmc_of[] = {
	{ .compatible = "qmc,qmc6308" },
	{}
};
MODULE_DEVICE_TABLE(of, qmc_of);

static const struct i2c_device_id qmc_id[] = {
	{ "qmc6308", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, qmc_id);

static struct i2c_driver qmc_drv = {
	.driver = {
		.name = "qmc6308-stub",
		.of_match_table = qmc_of,
	},
	.probe = qmc_probe,
	.remove = qmc_remove,
	.id_table = qmc_id,
};
module_i2c_driver(qmc_drv);
MODULE_AUTHOR("Kokuban");
MODULE_DESCRIPTION("UWS6152 qmc6308 compass functional stub");
MODULE_LICENSE("GPL");
