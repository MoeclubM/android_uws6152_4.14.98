// SPDX-License-Identifier: GPL-2.0
/* mafp stub - spi fingerprint@2 compatible="microarray,mafp".
 * Claims spi2.0 and exposes /dev/madev + /dev/madev0 like stock so the
 * fingerprint HAL open() succeeds instead of spinning on ENOENT.
 * Also registers an input device named "madev" for the WheelSensor HAL
 * (sensors.ums312.so opens input "madev" for the 65597 wheel sensor).
 * No closed algorithms: read returns empty, ioctl returns ENOTTY,
 * no wheel events reported. */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/uaccess.h>

#define MAFP_DEV_NAME "madev"
#define MAFP_CLASS_NAME "madev"

static dev_t mafp_devno;
static struct cdev mafp_cdev;
static struct class *mafp_class;
static struct input_dev *mafp_input;

static int mafp_open(struct inode *i, struct file *f) { return 0; }
static int mafp_release(struct inode *i, struct file *f) { return 0; }
static ssize_t mafp_read(struct file *f, char __user *b, size_t c, loff_t *o)
{ return 0; }
static long mafp_ioctl(struct file *f, unsigned int c, unsigned long a)
{ return -ENOTTY; }

static const struct file_operations mafp_fops = {
	.owner = THIS_MODULE,
	.open = mafp_open,
	.release = mafp_release,
	.read = mafp_read,
	.unlocked_ioctl = mafp_ioctl,
	.compat_ioctl = mafp_ioctl,
};

static int mafp_probe(struct spi_device *s)
{
	int ret;

	ret = alloc_chrdev_region(&mafp_devno, 0, 1, MAFP_DEV_NAME);
	if (ret)
		return ret;
	cdev_init(&mafp_cdev, &mafp_fops);
	ret = cdev_add(&mafp_cdev, mafp_devno, 1);
	if (ret)
		goto err_region;
	mafp_class = class_create(THIS_MODULE, MAFP_CLASS_NAME);
	if (IS_ERR(mafp_class)) {
		ret = PTR_ERR(mafp_class);
		goto err_cdev;
	}
	if (IS_ERR(device_create(mafp_class, &s->dev, mafp_devno, NULL,
				 "madev0"))) {
		ret = -ENODEV;
		goto err_class;
	}
	mafp_input = devm_input_allocate_device(&s->dev);
	if (!mafp_input) {
		ret = -ENOMEM;
		goto err_dev;
	}
	mafp_input->name = "madev";
	mafp_input->id.bustype = BUS_SPI;
	ret = input_register_device(mafp_input);
	if (ret)
		goto err_dev;
	dev_info(&s->dev, "mafp stub probe: /dev/madev0 + input/madev ready\n");
	return 0;
err_dev:
	device_destroy(mafp_class, mafp_devno);
err_class:
	class_destroy(mafp_class);
err_cdev:
	cdev_del(&mafp_cdev);
err_region:
	unregister_chrdev_region(mafp_devno, 1);
	return ret;
}

static int mafp_remove(struct spi_device *s)
{
	input_unregister_device(mafp_input);
	device_destroy(mafp_class, mafp_devno);
	class_destroy(mafp_class);
	cdev_del(&mafp_cdev);
	unregister_chrdev_region(mafp_devno, 1);
	return 0;
}

static const struct of_device_id mafp_of[] = {
	{ .compatible = "microarray,mafp" }, {}
};
MODULE_DEVICE_TABLE(of, mafp_of);
static const struct spi_device_id mafp_id[] = { { "mafp", 0 }, {} };
MODULE_DEVICE_TABLE(spi, mafp_id);
static struct spi_driver mafp_drv = {
	.driver = { .name = "mafp-stub", .of_match_table = mafp_of },
	.probe = mafp_probe,
	.remove = mafp_remove,
	.id_table = mafp_id,
};
module_spi_driver(mafp_drv);
MODULE_LICENSE("GPL");
