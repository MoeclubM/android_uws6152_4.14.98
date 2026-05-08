// SPDX-License-Identifier: GPL-2.0-only
/*
 * DA217 3-Axis Accelerometer Input Driver
 * SoraNeko fells gravity
 *
 * Reports acceleration via Linux input subsystem (EV_ABS) to match
 * android.hardware.sensors@1.0-service interface.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/pm.h>

#define DA217_REG_SPI_CFG      0x00
#define DA217_REG_CHIP_ID      0x01
#define DA217_REG_ACC_X_LSB    0x02  /* Start of 6-byte XYZ data block */
#define DA217_REG_MODE_BW      0x11

#define DA217_CHIP_ID          0x13
#define DA217_MODE_ENABLE      0x1E   /* Normal mode, 500Hz BW, no autosleep */
#define DA217_MODE_DISABLE     0x9E   /* Suspend mode */
#define DA217_SOFT_RESET_BITS  ((1 << 2) | (1 << 5))

#define DA217_POLL_INTERVAL    10     /* ms -> 100Hz ODR */

struct da217_data {
    struct i2c_client *client;
    struct input_dev *input;
    struct timer_list timer;
};

/* Soft reset (verified working in I2C mode) */
static int da217_soft_reset(struct i2c_client *client)
{
    int ret;
    u8 val;

    ret = i2c_smbus_read_byte_data(client, DA217_REG_SPI_CFG);
    if (ret < 0)
        return ret;
    val = ret | DA217_SOFT_RESET_BITS;
    ret = i2c_smbus_write_byte_data(client, DA217_REG_SPI_CFG, val);
    if (ret < 0)
        return ret;
    msleep(20);
    return 0;
}

/* Enable/disable measurements */
static int da217_enable(struct i2c_client *client, bool enable)
{
    u8 data = enable ? DA217_MODE_ENABLE : DA217_MODE_DISABLE;
    return i2c_smbus_write_byte_data(client, DA217_REG_MODE_BW, data);
}

/* Read 6 bytes (X_LSB..Z_MSB) via I2C burst, parse 14-bit left-justified data */
static int da217_read_xyz(struct i2c_client *client, s16 *x, s16 *y, s16 *z)
{
    u8 buf[6];
    int ret;

    ret = i2c_smbus_read_i2c_block_data(client, DA217_REG_ACC_X_LSB, 6, buf);
    if (ret != 6) {
        dev_err(&client->dev, "XYZ read error: %d\n", ret);
        return (ret < 0) ? ret : -EIO;
    }

    /* 14-bit left-justified: LSB low 2 bits are unused, MSB carries D[13:6] */
    *x = (s16)(((buf[1] << 8) | (buf[0] & 0xFC))) >> 2;
    *y = (s16)(((buf[3] << 8) | (buf[2] & 0xFC))) >> 2;
    *z = (s16)(((buf[5] << 8) | (buf[4] & 0xFC))) >> 2;

    return 0;
}

/* Timer callback: poll sensor and report to input subsystem */
static void da217_timer_callback(unsigned long ptr)
{
    struct da217_data *data = (struct da217_data *)ptr;
    s16 x, y, z;

    if (da217_read_xyz(data->client, &x, &y, &z) == 0) {
        input_report_abs(data->input, ABS_X, x);
        input_report_abs(data->input, ABS_Y, y);
        input_report_abs(data->input, ABS_Z, z);
        input_sync(data->input);
    }

    mod_timer(&data->timer, jiffies + msecs_to_jiffies(DA217_POLL_INTERVAL));
}

static int da217_probe(struct i2c_client *client,
                       const struct i2c_device_id *id)
{
    struct da217_data *data;
    struct input_dev *input;
    int ret;

    /* Verify chip ID */
    ret = i2c_smbus_read_byte_data(client, DA217_REG_CHIP_ID);
    if (ret != DA217_CHIP_ID) {
        dev_err(&client->dev, "Invalid chip ID: 0x%02x\n", ret);
        return (ret < 0) ? ret : -ENODEV;
    }

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    input = devm_input_allocate_device(&client->dev);
    if (!input)
        return -ENOMEM;

    data->client = client;
    data->input = input;

    /* Soft reset and enable */
    ret = da217_soft_reset(client);
    if (ret)
        return ret;

    ret = da217_enable(client, true);
    if (ret)
        return ret;

    /* Configure input device */
    input->name = "da217";
    input->id.bustype = BUS_I2C;
    input->dev.parent = &client->dev;

    __set_bit(EV_ABS, input->evbit);
    input_set_abs_params(input, ABS_X, -8192, 8191, 0, 0);
    input_set_abs_params(input, ABS_Y, -8192, 8191, 0, 0);
    input_set_abs_params(input, ABS_Z, -8192, 8191, 0, 0);

    ret = input_register_device(input);
    if (ret) {
        da217_enable(client, false);
        return ret;
    }

    /* Set up polling timer (4.14 kernel compatible) */
    setup_timer(&data->timer, da217_timer_callback, (unsigned long)data);
    mod_timer(&data->timer, jiffies + msecs_to_jiffies(DA217_POLL_INTERVAL));

    i2c_set_clientdata(client, data);
    dev_info(&client->dev, "DA217 accelerometer probed successfully\n");
    return 0;
}

static int da217_remove(struct i2c_client *client)
{
    struct da217_data *data = i2c_get_clientdata(client);

    del_timer_sync(&data->timer);
    da217_enable(client, false);
    return 0;
}

/* Power management */
static int da217_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct da217_data *data = i2c_get_clientdata(client);

    del_timer_sync(&data->timer);
    da217_enable(client, false);
    return 0;
}

static int da217_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct da217_data *data = i2c_get_clientdata(client);
    int ret;

    ret = da217_enable(client, true);
    if (ret)
        return ret;

    mod_timer(&data->timer, jiffies + msecs_to_jiffies(DA217_POLL_INTERVAL));
    return 0;
}

static SIMPLE_DEV_PM_OPS(da217_pm_ops, da217_suspend, da217_resume);

/* Device tree matching: only "da,da217" */
static const struct of_device_id da217_of_match[] = {
    { .compatible = "da,da217", },
    { }
};
MODULE_DEVICE_TABLE(of, da217_of_match);

static struct i2c_driver da217_driver = {
    .driver = {
        .name           = "da217",
        .of_match_table = da217_of_match,
        .pm             = &da217_pm_ops,
    },
    .probe      = da217_probe,
    .remove     = da217_remove,
    .id_table   = NULL,   /* No I2C device ID table needed */
};
module_i2c_driver(da217_driver);

MODULE_AUTHOR("ZeroDreamCat <neko@0w0.cafe>");
MODULE_DESCRIPTION("DA217 3-Axis Accelerometer Input Driver");
MODULE_LICENSE("GPL v2");
