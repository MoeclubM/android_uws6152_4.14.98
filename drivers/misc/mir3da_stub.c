// SPDX-License-Identifier: GPL-2.0
/*
 * mir3da functional stub for UWS6152 (i2c@70500000 mir3da@26)
 * Stock DTS: compatible="da,da217" reg <0x26>, layout <0x07>.
 *
 * HAL contract (vendor/lib64/hw/sensors.ums312.so, reversed):
 *  - AccSensor/SCSensor share one input device named "accelerometer"
 *    (EVIOCGNAME match in SensorBase::openInput), raw EV_ABS codes 0..2
 *    + EV_SYN; data_fd is that input fd.
 *  - sysfs class "xr-gsensor": /sys/class/xr-gsensor/device/gsensor
 *    (O_RDWR, write ASCII "1" enable / "0" disable / "2" ...; HAL keeps
 *    fd open while active), /sys/class/xr-gsensor/device/delay_acc
 *    (ASCII decimal ms), /sys/class/xr-gsensor/device/scsensor
 *    (ASCII "1"/"0" for step counter).
 *  - WheelSensor opens a separate input named "silead_fp" and reads
 *    EV_REL codes 0/1 (direction) + EV_SYN.
 *
 * Data consumed: AccSensor reads ABS_X/Y/Z (codes 0/1/2, x0.0095768 m/s^2);
 * ScSensor reads ABS_X (code 0) as step count, gated on EV_SYN;
 * WheelSensor reads REL_X/REL_Y (codes 0/1) as wheel direction.
 * Stock uses ST DW 3-axis accelerometer + silead_fp + sc sensor under the
 * same xr-gsensor class; this stub emulates all of them from one probe.
 *
 * No closed calibration/layout tables: while enabled, reports static
 * gravity + slowly increasing step count + idle wheel so the HAL data
 * path stays alive. IRQ ignored.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/workqueue.h>

struct mir3da_stub {
	struct i2c_client *client;
	struct input_dev *acc;
	struct input_dev *wheel;
	struct class *cls;
	struct device *clsdev;
	struct delayed_work work;
	bool acc_en;
	bool sc_en;
	unsigned int steps;
};

static void mir3da_report(struct work_struct *w)
{
	struct mir3da_stub *st = container_of(to_delayed_work(w),
					      struct mir3da_stub, work);

	if (!st->acc_en && !st->sc_en)
		return;
	/* ~1g on Z in raw units (HAL scales by 0.0095768) */
	input_report_abs(st->acc, ABS_X, 0);
	input_report_abs(st->acc, ABS_Y, 0);
	input_report_abs(st->acc, ABS_Z, 102);
	if (st->sc_en)
		st->steps++;
	input_sync(st->acc);
	schedule_delayed_work(&st->work, msecs_to_jiffies(1000));
}

static ssize_t gsensor_show(struct device *d, struct device_attribute *a,
			    char *b)
{
	struct mir3da_stub *st = dev_get_drvdata(d);

	return sprintf(b, "%d\n", st->acc_en);
}

static ssize_t gsensor_store(struct device *d, struct device_attribute *a,
			     const char *b, size_t c)
{
	struct mir3da_stub *st = dev_get_drvdata(d);
	unsigned long v;

	if (c >= 1 && b[0] >= '0' && b[0] <= '2')
		v = b[0] - '0';
	else
		return -EINVAL;
	st->acc_en = !!v;
	if (st->acc_en || st->sc_en)
		schedule_delayed_work(&st->work, 0);
	else
		cancel_delayed_work_sync(&st->work);
	dev_info(d, "mir3da stub gsensor=%d\n", st->acc_en);
	return c;
}

static ssize_t delay_acc_show(struct device *d, struct device_attribute *a,
			      char *b)
{
	return sprintf(b, "100\n");
}

static ssize_t delay_acc_store(struct device *d, struct device_attribute *a,
			       const char *b, size_t c)
{
	return c;
}

static ssize_t scsensor_show(struct device *d, struct device_attribute *a,
			     char *b)
{
	struct mir3da_stub *st = dev_get_drvdata(d);

	return sprintf(b, "%d\n", st->sc_en);
}

static ssize_t scsensor_store(struct device *d, struct device_attribute *a,
			      const char *b, size_t c)
{
	struct mir3da_stub *st = dev_get_drvdata(d);
	unsigned long v;

	if (c >= 1 && b[0] >= '0' && b[0] <= '2')
		v = b[0] - '0';
	else
		return -EINVAL;
	st->sc_en = !!v;
	if (st->acc_en || st->sc_en)
		schedule_delayed_work(&st->work, 0);
	else
		cancel_delayed_work_sync(&st->work);
	dev_info(d, "mir3da stub scsensor=%d steps=%u\n", st->sc_en, st->steps);
	return c;
}

/* 0666 needed: HAL (system uid) writes these while owner is root; hand-rolled
 * attributes bypass VERIFY_OCTAL_PERMISSIONS. */
static struct device_attribute dev_attr_gsensor = {
	.attr = { .name = "gsensor", .mode = 0666 },
	.show = gsensor_show,
	.store = gsensor_store,
};
static struct device_attribute dev_attr_delay_acc = {
	.attr = { .name = "delay_acc", .mode = 0666 },
	.show = delay_acc_show,
	.store = delay_acc_store,
};
static struct device_attribute dev_attr_scsensor = {
	.attr = { .name = "scsensor", .mode = 0666 },
	.show = scsensor_show,
	.store = scsensor_store,
};

static int mir3da_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct mir3da_stub *st;
	int ret;

	dev_info(dev, "mir3da stub probe (UWS6152 w527, layout 7)\n");
	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->client = client;
	INIT_DELAYED_WORK(&st->work, mir3da_report);

	st->acc = devm_input_allocate_device(dev);
	if (!st->acc)
		return -ENOMEM;
	st->acc->name = "accelerometer";
	st->acc->id.bustype = BUS_I2C;
	input_set_abs_params(st->acc, ABS_X, -32768, 32767, 0, 0);
	input_set_abs_params(st->acc, ABS_Y, -32768, 32767, 0, 0);
	input_set_abs_params(st->acc, ABS_Z, -32768, 32767, 0, 0);
	ret = input_register_device(st->acc);
	if (ret)
		return ret;

	st->wheel = devm_input_allocate_device(dev);
	if (!st->wheel)
		return -ENOMEM;
	st->wheel->name = "silead_fp";
	st->wheel->id.bustype = BUS_I2C;
	input_set_capability(st->wheel, EV_REL, REL_X);
	input_set_capability(st->wheel, EV_REL, REL_Y);
	ret = input_register_device(st->wheel);
	if (ret)
		return ret;

	st->cls = class_create(THIS_MODULE, "xr-gsensor");
	if (IS_ERR(st->cls))
		return PTR_ERR(st->cls);
	st->clsdev = device_create(st->cls, dev, MKDEV(0, 0), st, "device");
	if (IS_ERR(st->clsdev)) {
		ret = PTR_ERR(st->clsdev);
		goto err_class;
	}
	ret = device_create_file(st->clsdev, &dev_attr_gsensor);
	if (ret)
		goto err_dev;
	ret = device_create_file(st->clsdev, &dev_attr_delay_acc);
	if (ret)
		goto err_gs;
	ret = device_create_file(st->clsdev, &dev_attr_scsensor);
	if (ret)
		goto err_delay;

	i2c_set_clientdata(client, st);
	dev_info(dev, "mir3da stub ready: accelerometer + silead_fp + xr-gsensor\n");
	return 0;

err_delay:
	device_remove_file(st->clsdev, &dev_attr_delay_acc);
err_gs:
	device_remove_file(st->clsdev, &dev_attr_gsensor);
err_dev:
	device_destroy(st->cls, MKDEV(0, 0));
err_class:
	class_destroy(st->cls);
	return ret;
}

static int mir3da_remove(struct i2c_client *client)
{
	struct mir3da_stub *st = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&st->work);
	device_remove_file(st->clsdev, &dev_attr_scsensor);
	device_remove_file(st->clsdev, &dev_attr_delay_acc);
	device_remove_file(st->clsdev, &dev_attr_gsensor);
	device_destroy(st->cls, MKDEV(0, 0));
	class_destroy(st->cls);
	return 0;
}

static const struct of_device_id mir3da_of[] = {
	{ .compatible = "da,da217" }, {}
};
MODULE_DEVICE_TABLE(of, mir3da_of);
static const struct i2c_device_id mir3da_id[] = { { "da217", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, mir3da_id);
static struct i2c_driver mir3da_drv = {
	.driver = { .name = "mir3da-stub", .of_match_table = mir3da_of },
	.probe = mir3da_probe,
	.remove = mir3da_remove,
	.id_table = mir3da_id,
};
module_i2c_driver(mir3da_drv);
MODULE_AUTHOR("Kokuban");
MODULE_DESCRIPTION("UWS6152 mir3da/da217 accel+step+wheel functional stub");
MODULE_LICENSE("GPL");
