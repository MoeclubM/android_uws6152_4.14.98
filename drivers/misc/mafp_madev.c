// SPDX-License-Identifier: GPL-2.0
/* MicroArray fingerprint TEE-mode driver for UWS6152 w527.
 * Binds spi fingerprint@2 compatible="microarray,mafp".
 * Exposes /dev/madev + /dev/madev0, input "madev", full mas_ioctl table,
 * 128KB mas_mmap shared buffer, fpint-gpios interrupt wakeup.
 * TEE mode: no REE-side SPI chip probing (TA owns the sensor registers);
 * probe always succeeds so the HAL can open/mmap/handshake.
 * Derived from the official MicroArray madev_tee.c (realme C21Y 4.14 tree)
 * with SPRD GPIO names (fpint-gpios / fpen-gpios) kept. */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/freezer.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/pm_wakeup.h>
#include <linux/compat.h>

#define MA_CHR_FILE_NAME	"madev0"
#define MA_CHR_DEV_NAME		"madev"
#define MA_DRV_VERSION		(0x00004006)

#define MA_IOC_MAGIC	'M'
#define MA_IOC_DELK     _IO(MA_IOC_MAGIC,  1)
#define MA_IOC_SLEP     _IO(MA_IOC_MAGIC,  2)
#define MA_IOC_WKUP     _IO(MA_IOC_MAGIC,  3)
#define MA_IOC_ENCK     _IO(MA_IOC_MAGIC,  4)
#define MA_IOC_DICK     _IO(MA_IOC_MAGIC,  5)
#define MA_IOC_EINT     _IO(MA_IOC_MAGIC,  6)
#define MA_IOC_DINT     _IO(MA_IOC_MAGIC,  7)
#define MA_IOC_TPDW     _IO(MA_IOC_MAGIC,  8)
#define MA_IOC_TPUP     _IO(MA_IOC_MAGIC,  9)
#define MA_IOC_SGTP     _IO(MA_IOC_MAGIC,  11)
#define MA_IOC_DBTP     _IO(MA_IOC_MAGIC,  12)
#define MA_IOC_LGTP     _IO(MA_IOC_MAGIC,  13)
#define MA_IOC_VTIM     _IOR(MA_IOC_MAGIC,  14, unsigned char)
#define MA_IOC_CNUM     _IOR(MA_IOC_MAGIC,  15, unsigned char)
#define MA_IOC_SNUM     _IOR(MA_IOC_MAGIC,  16, unsigned char)
#define MA_IOC_UKRP     _IOW(MA_IOC_MAGIC,  17, unsigned char)
#define MA_IOC_NAVW     _IO(MA_IOC_MAGIC,   18)
#define MA_IOC_NAVA     _IO(MA_IOC_MAGIC,   19)
#define MA_IOC_NAVS     _IO(MA_IOC_MAGIC,   20)
#define MA_IOC_NAVD     _IO(MA_IOC_MAGIC,   21)
#define MA_IOC_EIRQ     _IO(MA_IOC_MAGIC,   31)
#define MA_IOC_DIRQ     _IO(MA_IOC_MAGIC,   32)
#define MA_IOC_SPAR     _IOW(MA_IOC_MAGIC,   33, unsigned int)
#define MA_IOC_GPAR     _IOR(MA_IOC_MAGIC,   34, unsigned int)
#define MA_IOC_GVER     _IOR(MA_IOC_MAGIC,   35, unsigned int)
#define MA_IOC_PWOF     _IO(MA_IOC_MAGIC,    36)
#define MA_IOC_PWON     _IO(MA_IOC_MAGIC,    37)
#define MA_IOC_STSP     _IOW(MA_IOC_MAGIC,   38, unsigned int)
#define MA_IOC_FD_WAIT_CMD		_IO(MA_IOC_MAGIC, 39)
#define MA_IOC_TEST_WAKE_FD		_IO(MA_IOC_MAGIC, 40)
#define MA_IOC_TEST_WAIT_FD_RET		_IOR(MA_IOC_MAGIC, 41, unsigned int)
#define MA_IOC_FD_WAKE_TEST_RET		_IOW(MA_IOC_MAGIC, 42, unsigned int)
#define MA_IOC_GET_SCREEN		_IOR(MA_IOC_MAGIC, 43, unsigned int)
#define MA_IOC_GET_INT_STATE		_IOR(MA_IOC_MAGIC, 44, unsigned int)
#define MA_IOC_GET_FACTORY_FLAG		_IOR(MA_IOC_MAGIC, 53, unsigned int)
#define MA_IOC_SET_FACTORY_FLAG		_IOW(MA_IOC_MAGIC, 54, unsigned int)

struct mafp_dev {
	dev_t idd;
	struct cdev chd;
	struct class *cls;
	struct device *dev;
	struct input_dev *input;
	struct spi_device *spi;
	struct work_struct work;
	struct workqueue_struct *wq;
	struct notifier_block fb_nb;
	int irq;
	int int_gpio;
	int en_gpio;
	bool irq_requested;
	bool irq_enabled;
	unsigned int drv_reg;
	unsigned int speed;
	unsigned int screen_on;
	unsigned int factory_flag;
	unsigned int nav_enable;
	int irq_flag;
	unsigned int u1_flag;
	unsigned int u2_flag;
	void *shm;
	struct wakeup_source wakelock;
	bool wakelock_inited;
	struct mutex xfer_lock;
	u8 *txb;
	u8 *rxb;
};

static struct mafp_dev *g_mafp;
static DECLARE_WAIT_QUEUE_HEAD(mafp_wq);
static DECLARE_WAIT_QUEUE_HEAD(mafp_u1wq);
static DECLARE_WAIT_QUEUE_HEAD(mafp_u2wq);
static DEFINE_MUTEX(mafp_ioctl_lock);

static void mafp_work(struct work_struct *w)
{
	struct mafp_dev *ma = container_of(w, struct mafp_dev, work);

	ma->irq_flag = 1;
	if (ma->wakelock_inited)
		__pm_wakeup_event(&ma->wakelock, msecs_to_jiffies(2000));
	wake_up(&mafp_wq);
}

static irqreturn_t mafp_isr(int irq, void *dev_id)
{
	struct mafp_dev *ma = dev_id;

	queue_work(ma->wq, &ma->work);
	return IRQ_HANDLED;
}

static int mafp_fb_notifier(struct notifier_block *nb,
			    unsigned long event, void *data)
{
	struct mafp_dev *ma = container_of(nb, struct mafp_dev, fb_nb);
	struct fb_event *ev = data;
	unsigned int blank;

	if (event != FB_EVENT_BLANK)
		return 0;
	blank = *(int *)ev->data;
	if (blank == FB_BLANK_UNBLANK)
		ma->screen_on = 1;
	else if (blank == FB_BLANK_POWERDOWN)
		ma->screen_on = 0;
	return 0;
}

static void mafp_nav_key(struct mafp_dev *ma, unsigned int code)
{
	input_report_key(ma->input, code, 1);
	input_sync(ma->input);
	input_report_key(ma->input, code, 0);
	input_sync(ma->input);
}

static long mafp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mafp_dev *ma = filp->private_data;
	int ret = 0;
	unsigned int version = MA_DRV_VERSION;
	unsigned int tmp;

	if (!ma)
		return -ENODEV;

	switch (cmd) {
	case MA_IOC_DELK:
		if (ma->wakelock_inited)
			__pm_wakeup_event(&ma->wakelock,
					  msecs_to_jiffies(5 * HZ));
		break;
	case MA_IOC_SLEP:
		ma->irq_flag = 0;
		ret = wait_event_freezekillable_unsafe(mafp_wq,
						       ma->irq_flag != 0);
		return ret;
	case MA_IOC_WKUP:
		ma->irq_flag = 1;
		wake_up(&mafp_wq);
		break;
	case MA_IOC_ENCK:
	case MA_IOC_DICK:
		break;
	case MA_IOC_EINT:
		if (ma->irq_requested && !ma->irq_enabled) {
			enable_irq(ma->irq);
			ma->irq_enabled = true;
		}
		break;
	case MA_IOC_DINT:
		if (ma->irq_requested && ma->irq_enabled) {
			disable_irq_nosync(ma->irq);
			ma->irq_enabled = false;
		}
		break;
	case MA_IOC_TPDW:
		input_report_key(ma->input, KEY_FN_F5, 1);
		input_sync(ma->input);
		break;
	case MA_IOC_TPUP:
		input_report_key(ma->input, KEY_FN_F5, 0);
		input_sync(ma->input);
		break;
	case MA_IOC_SGTP:
		mafp_nav_key(ma, KEY_FN_F5);
		break;
	case MA_IOC_DBTP:
		mafp_nav_key(ma, KEY_FN_F6);
		break;
	case MA_IOC_LGTP:
		mafp_nav_key(ma, KEY_FN_F7);
		break;
	case MA_IOC_NAVW:
		mafp_nav_key(ma, KEY_FN_F1);
		break;
	case MA_IOC_NAVA:
		mafp_nav_key(ma, KEY_FN_F3);
		break;
	case MA_IOC_NAVS:
		mafp_nav_key(ma, KEY_FN_F2);
		break;
	case MA_IOC_NAVD:
		mafp_nav_key(ma, KEY_FN_F4);
		break;
	case MA_IOC_EIRQ:
		if (!ma->irq_requested && ma->irq > 0) {
			ret = request_irq(ma->irq, mafp_isr,
					  IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
					  "microarray_eint", ma);
			if (ret) {
				dev_err(&ma->spi->dev,
					"request irq %d failed: %d\n",
					ma->irq, ret);
				break;
			}
			ret = enable_irq_wake(ma->irq);
			if (ret)
				dev_warn(&ma->spi->dev,
					 "enable_irq_wake failed: %d\n", ret);
			ma->irq_requested = true;
			ma->irq_enabled = true;
			ret = ma->irq;
		}
		break;
	case MA_IOC_DIRQ:
		if (ma->irq_requested) {
			free_irq(ma->irq, ma);
			ma->irq_requested = false;
			ma->irq_enabled = false;
		}
		break;
	case MA_IOC_SPAR:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_from_user(&ma->drv_reg, (void __user *)arg,
				     sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	case MA_IOC_GPAR:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_to_user((void __user *)arg, &ma->drv_reg,
				   sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	case MA_IOC_GVER:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_to_user((void __user *)arg, &version,
				   sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	case MA_IOC_PWOF:
	case MA_IOC_PWON:
		break;
	case MA_IOC_STSP:
		if (copy_from_user(&ma->speed, (void __user *)arg,
				   sizeof(unsigned int)))
			return -EFAULT;
		break;
	case MA_IOC_FD_WAIT_CMD:
		ma->u2_flag = 0;
		ret = wait_event_interruptible(mafp_u2wq, ma->u2_flag != 0);
		break;
	case MA_IOC_TEST_WAKE_FD:
		ma->u2_flag = 1;
		wake_up_interruptible(&mafp_u2wq);
		break;
	case MA_IOC_TEST_WAIT_FD_RET:
		ma->u1_flag = 0;
		ret = wait_event_interruptible(mafp_u1wq, ma->u1_flag != 0);
		if (ret)
			break;
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_to_user((void __user *)arg, &ma->drv_reg,
				   sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	case MA_IOC_FD_WAKE_TEST_RET:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_from_user(&ma->drv_reg, (void __user *)arg,
				     sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		if (ret)
			break;
		msleep(20);
		ma->u1_flag = 1;
		wake_up_interruptible(&mafp_u1wq);
		break;
	case MA_IOC_GET_SCREEN:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_to_user((void __user *)arg, &ma->screen_on,
				   sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	case MA_IOC_GET_INT_STATE:
		if (ma->int_gpio >= 0) {
			tmp = !!gpio_get_value(ma->int_gpio);
			mutex_lock(&mafp_ioctl_lock);
			ret = copy_to_user((void __user *)arg, &tmp,
					   sizeof(unsigned int)) ? -EFAULT : 0;
			mutex_unlock(&mafp_ioctl_lock);
		} else {
			ret = -ENODEV;
		}
		break;
	case MA_IOC_GET_FACTORY_FLAG:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_to_user((void __user *)arg, &ma->factory_flag,
				   sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	case MA_IOC_SET_FACTORY_FLAG:
		mutex_lock(&mafp_ioctl_lock);
		ret = copy_from_user(&ma->factory_flag, (void __user *)arg,
				     sizeof(unsigned int)) ? -EFAULT : 0;
		mutex_unlock(&mafp_ioctl_lock);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static long mafp_compat_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	return filp->f_op->unlocked_ioctl(filp, cmd, arg);
}
#endif

static int mafp_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct mafp_dev *ma = filp->private_data;
	unsigned long page;

	if (!ma)
		return -ENODEV;
	if (!ma->shm) {
		ma->shm = kzalloc(128 * 1024, GFP_KERNEL);
		if (!ma->shm)
			return -ENOMEM;
	}
	page = virt_to_phys(ma->shm) >> PAGE_SHIFT;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma, vma->vm_start, page,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot))
		return -EAGAIN;
	vma->vm_flags |= (VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}

static int mafp_open(struct inode *ip, struct file *fp)
{
	fp->private_data = g_mafp;
	return g_mafp ? 0 : -ENODEV;
}

static int mafp_release(struct inode *ip, struct file *fp)
{
	return 0;
}

static int mafp_spi_sync(struct mafp_dev *ma, size_t len)
{
	struct spi_message m;
	struct spi_transfer t = {
		.tx_buf = ma->txb,
		.rx_buf = ma->rxb,
		.len = len,
		.delay_usecs = 1,
		.bits_per_word = 8,
		.speed_hz = ma->spi->max_speed_hz,
	};
	int ret;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(ma->spi, &m);
	return ret;
}

static ssize_t mafp_write(struct file *fp, const char __user *buf,
			  size_t count, loff_t *pos)
{
	struct mafp_dev *ma = fp->private_data;

	if (!ma)
		return -ENODEV;
	if (count == 0 || count > 8192)
		return -EINVAL;
	mutex_lock(&ma->xfer_lock);
	if (copy_from_user(ma->txb, buf, count)) {
		mutex_unlock(&ma->xfer_lock);
		return -EFAULT;
	}
	mutex_unlock(&ma->xfer_lock);
	return count;
}

static ssize_t mafp_read(struct file *fp, char __user *buf,
			 size_t count, loff_t *pos)
{
	struct mafp_dev *ma = fp->private_data;
	int ret;

	if (!ma)
		return -ENODEV;
	if (count == 0 || count > 8192)
		return -EINVAL;
	mutex_lock(&ma->xfer_lock);
	ret = mafp_spi_sync(ma, count);
	if (ret) {
		mutex_unlock(&ma->xfer_lock);
		return -EIO;
	}
	if (copy_to_user(buf, ma->rxb, count)) {
		mutex_unlock(&ma->xfer_lock);
		return -EFAULT;
	}
	mutex_unlock(&ma->xfer_lock);
	return count;
}

static const struct file_operations mafp_fops = {
	.owner = THIS_MODULE,
	.open = mafp_open,
	.release = mafp_release,
	.write = mafp_write,
	.read = mafp_read,
	.unlocked_ioctl = mafp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mafp_compat_ioctl,
#endif
	.mmap = mafp_mmap,
};

static ssize_t nav_enable_show(struct device *d,
			       struct device_attribute *a, char *b)
{
	struct mafp_dev *ma = dev_get_drvdata(d);

	return sprintf(b, "%u\n", ma ? ma->nav_enable : 0);
}

static ssize_t nav_enable_store(struct device *d,
				struct device_attribute *a,
				const char *b, size_t c)
{
	struct mafp_dev *ma = dev_get_drvdata(d);

	if (ma && c >= 1 && (b[0] == '0' || b[0] == '1'))
		ma->nav_enable = b[0] - '0';
	return c;
}
static DEVICE_ATTR(madev_nav_enable, 0644, nav_enable_show, nav_enable_store);

static int mafp_probe(struct spi_device *spi)
{
	struct device_node *np = spi->dev.of_node;
	struct mafp_dev *ma;
	int ret;

	ma = devm_kzalloc(&spi->dev, sizeof(*ma), GFP_KERNEL);
	if (!ma)
		return -ENOMEM;
	ma->spi = spi;
	ma->screen_on = 1;
	ma->int_gpio = -1;
	ma->en_gpio = -1;
	ma->irq = 0;
	mutex_init(&ma->xfer_lock);
	ma->txb = devm_kzalloc(&spi->dev, 8192, GFP_KERNEL);
	ma->rxb = devm_kzalloc(&spi->dev, 8192, GFP_KERNEL);
	if (!ma->txb || !ma->rxb)
		return -ENOMEM;
	spi->bits_per_word = 8;
	spi->max_speed_hz = 6000000;
	spi_setup(spi);

	ma->int_gpio = of_get_named_gpio(np, "fpint-gpios", 0);
	if (ma->int_gpio >= 0) {
		ret = devm_gpio_request_one(&spi->dev, ma->int_gpio,
					    GPIOF_IN, "microarray_eint");
		if (ret) {
			dev_err(&spi->dev, "fpint gpio request failed: %d\n",
				ret);
			return ret;
		}
		ma->irq = gpio_to_irq(ma->int_gpio);
		if (ma->irq < 0) {
			dev_err(&spi->dev, "gpio_to_irq failed: %d\n",
				ma->irq);
			return ma->irq;
		}
	} else {
		dev_warn(&spi->dev, "fpint-gpios missing, irq disabled\n");
	}

	ma->en_gpio = of_get_named_gpio(np, "fpen-gpios", 0);
	if (ma->en_gpio >= 0) {
		ret = devm_gpio_request_one(&spi->dev, ma->en_gpio,
					    GPIOF_OUT_INIT_HIGH,
					    "microarray_en");
		if (ret)
			dev_warn(&spi->dev, "fpen gpio request failed: %d\n",
				 ret);
	}

	ret = alloc_chrdev_region(&ma->idd, 0, 1, MA_CHR_DEV_NAME);
	if (ret)
		return ret;
	cdev_init(&ma->chd, &mafp_fops);
	ma->chd.owner = THIS_MODULE;
	ret = cdev_add(&ma->chd, ma->idd, 1);
	if (ret)
		goto err_region;
	ma->cls = class_create(THIS_MODULE, MA_CHR_DEV_NAME);
	if (IS_ERR(ma->cls)) {
		ret = PTR_ERR(ma->cls);
		goto err_cdev;
	}
	ma->dev = device_create(ma->cls, &spi->dev, ma->idd, ma,
				MA_CHR_FILE_NAME);
	if (IS_ERR(ma->dev)) {
		ret = PTR_ERR(ma->dev);
		goto err_class;
	}
	ret = device_create_file(ma->dev, &dev_attr_madev_nav_enable);
	if (ret)
		dev_warn(&spi->dev, "nav_enable sysfs failed: %d\n", ret);

	device_init_wakeup(ma->dev, true);
	wakeup_source_init(&ma->wakelock, "microarray_process_wakelock");
	ma->wakelock_inited = true;
	INIT_WORK(&ma->work, mafp_work);
	ma->wq = create_singlethread_workqueue("mas_workqueue");
	if (!ma->wq) {
		ret = -ENOMEM;
		goto err_dev;
	}

	ma->input = devm_input_allocate_device(&spi->dev);
	if (!ma->input) {
		ret = -ENOMEM;
		goto err_wq;
	}
	ma->input->name = "madev";
	ma->input->id.bustype = BUS_SPI;
	__set_bit(EV_KEY, ma->input->evbit);
	__set_bit(EV_SYN, ma->input->evbit);
	__set_bit(KEY_FN_F1, ma->input->keybit);
	__set_bit(KEY_FN_F2, ma->input->keybit);
	__set_bit(KEY_FN_F3, ma->input->keybit);
	__set_bit(KEY_FN_F4, ma->input->keybit);
	__set_bit(KEY_FN_F5, ma->input->keybit);
	__set_bit(KEY_FN_F6, ma->input->keybit);
	__set_bit(KEY_FN_F7, ma->input->keybit);
	__set_bit(KEY_POWER, ma->input->keybit);
	ret = input_register_device(ma->input);
	if (ret)
		goto err_wq;

	ma->fb_nb.notifier_call = mafp_fb_notifier;
	fb_register_client(&ma->fb_nb);

	spi_set_drvdata(spi, ma);
	g_mafp = ma;
	dev_info(&spi->dev,
		 "microarray mafp TEE probe ok: /dev/madev0 irq=%d\n", ma->irq);
	return 0;

err_wq:
	destroy_workqueue(ma->wq);
err_dev:
	device_destroy(ma->cls, ma->idd);
err_class:
	class_destroy(ma->cls);
err_cdev:
	cdev_del(&ma->chd);
err_region:
	unregister_chrdev_region(ma->idd, 1);
	return ret;
}

static int mafp_remove(struct spi_device *spi)
{
	struct mafp_dev *ma = spi_get_drvdata(spi);

	if (!ma)
		return 0;
	fb_unregister_client(&ma->fb_nb);
	if (ma->wakelock_inited) {
		wakeup_source_trash(&ma->wakelock);
		ma->wakelock_inited = false;
	}
	if (ma->input)
		input_unregister_device(ma->input);
	if (ma->wq)
		destroy_workqueue(ma->wq);
	if (ma->irq_requested)
		free_irq(ma->irq, ma);
	device_remove_file(ma->dev, &dev_attr_madev_nav_enable);
	device_destroy(ma->cls, ma->idd);
	class_destroy(ma->cls);
	cdev_del(&ma->chd);
	unregister_chrdev_region(ma->idd, 1);
	kfree(ma->shm);
	if (g_mafp == ma)
		g_mafp = NULL;
	return 0;
}

static const struct of_device_id mafp_of[] = {
	{ .compatible = "microarray,mafp" }, {}
};
MODULE_DEVICE_TABLE(of, mafp_of);
static const struct spi_device_id mafp_id[] = { { "mafp", 0 }, {} };
MODULE_DEVICE_TABLE(spi, mafp_id);
static struct spi_driver mafp_drv = {
	.driver = { .name = "mafp-madev", .of_match_table = mafp_of },
	.probe = mafp_probe,
	.remove = mafp_remove,
	.id_table = mafp_id,
};
module_spi_driver(mafp_drv);
MODULE_AUTHOR("Microarray / UWS6152 port");
MODULE_DESCRIPTION("MicroArray fingerprint TEE-mode driver (microarray,mafp)");
MODULE_LICENSE("GPL");
