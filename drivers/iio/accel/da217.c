// SPDX-License-Identifier: GPL-2.0-only
/*
 * MiraMEMS DA217 3-Axis Accelerometer I2C driver
 *
 * Based on MiraMEMS DA280 driver and DA217 datasheet.
 * Modified for DA217 with soft reset and proper initialization.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define DA217_REG_SPI_CFG      0x00
#define DA217_REG_CHIP_ID      0x01
#define DA217_REG_ACC_X_LSB    0x02
#define DA217_REG_ACC_Y_LSB    0x04
#define DA217_REG_ACC_Z_LSB    0x06
#define DA217_REG_MODE_BW      0x11

#define DA217_CHIP_ID          0x13

/* MODE_BW: 
 * bit7 PWR_OFF (0=normal, 1=suspend)
 * bit1-2 BW (10=100Hz, 01=250Hz, 00/11=500Hz)
 * bit0 auto_sleep (0=disable)
 * 0x1E = 0b00011110 : PWR_OFF=0, BW=11 (500Hz), auto_sleep=0
 */
#define DA217_MODE_ENABLE      0x1E
#define DA217_MODE_DISABLE     0x9E

/* Software reset bits in SPI_CFG: bit2 (soft_reset) and bit5 */
#define DA217_SOFT_RESET_BITS  ((1 << 2) | (1 << 5))

static const int da217_nscale = 2395019; /* 2.395019 m/s^2 per LSB */

struct da217_match_data {
    const char *name;
    int num_channels;
};

struct da217_data {
    struct i2c_client *client;
};

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

    /* wait for reset to complete */
    msleep(20);

    return 0;
}

static int da217_enable(struct i2c_client *client, bool enable)
{
    u8 data = enable ? DA217_MODE_ENABLE : DA217_MODE_DISABLE;
    return i2c_smbus_write_byte_data(client, DA217_REG_MODE_BW, data);
}

#define DA217_CHANNEL(reg, axis) { \
    .type = IIO_ACCEL, \
    .address = reg, \
    .modified = 1, \
    .channel2 = IIO_MOD_##axis, \
    .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
    .info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE), \
}

static const struct iio_chan_spec da217_channels[] = {
    DA217_CHANNEL(DA217_REG_ACC_X_LSB, X),
    DA217_CHANNEL(DA217_REG_ACC_Y_LSB, Y),
    DA217_CHANNEL(DA217_REG_ACC_Z_LSB, Z),
};

static int da217_read_raw(struct iio_dev *indio_dev,
                          struct iio_chan_spec const *chan,
                          int *val, int *val2, long mask)
{
    struct da217_data *data = iio_priv(indio_dev);
    int ret;

    switch (mask) {
    case IIO_CHAN_INFO_RAW:
        ret = i2c_smbus_read_word_data(data->client, chan->address);
        if (ret < 0)
            return ret;
        /* 14-bit left justified, right shift 2 */
        *val = (s16)ret >> 2;
        return IIO_VAL_INT;
    case IIO_CHAN_INFO_SCALE:
        *val = 0;
        *val2 = da217_nscale;
        return IIO_VAL_INT_PLUS_NANO;
    default:
        return -EINVAL;
    }
}

static const struct iio_info da217_info = {
    .read_raw = da217_read_raw,
};

static void da217_disable(void *client)
{
    da217_enable(client, false);
}

static int da217_probe(struct i2c_client *client,
                       const struct i2c_device_id *id)
{
    const struct da217_match_data *match_data;
    struct iio_dev *indio_dev;
    struct da217_data *data;
    int ret;

    ret = i2c_smbus_read_byte_data(client, DA217_REG_CHIP_ID);
    if (ret != DA217_CHIP_ID)
        return (ret < 0) ? ret : -ENODEV;

    /* get match data from devicetree or id table */
    if (client->dev.of_node)
        match_data = of_device_get_match_data(&client->dev);
    else if (id)
        match_data = (const struct da217_match_data *)id->driver_data;
    else
        return -ENODEV;

    if (!match_data) {
        dev_err(&client->dev, "Error match-data not set\n");
        return -EINVAL;
    }

    indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
    if (!indio_dev)
        return -ENOMEM;

    data = iio_priv(indio_dev);
    data->client = client;

    indio_dev->info = &da217_info;
    indio_dev->modes = INDIO_DIRECT_MODE;
    indio_dev->channels = da217_channels;
    indio_dev->num_channels = match_data->num_channels;
    indio_dev->name = match_data->name;

    /* soft reset the device to known state */
    ret = da217_soft_reset(client);
    if (ret)
        return ret;

    /* enable measurements */
    ret = da217_enable(client, true);
    if (ret < 0)
        return ret;

    ret = devm_add_action_or_reset(&client->dev, da217_disable, client);
    if (ret)
        return ret;

    return devm_iio_device_register(&client->dev, indio_dev);
}

static int da217_suspend(struct device *dev)
{
    return da217_enable(to_i2c_client(dev), false);
}

static int da217_resume(struct device *dev)
{
    return da217_enable(to_i2c_client(dev), true);
}

static SIMPLE_DEV_PM_OPS(da217_pm_ops, da217_suspend, da217_resume);

static const struct da217_match_data da217_match_data = { "da217", 3 };
static const struct da217_match_data da226_match_data = { "da226", 2 };
static const struct da217_match_data da280_match_data = { "da280", 3 };

static const struct acpi_device_id da217_acpi_match[] = {
    { "NSA2513", (kernel_ulong_t)&da217_match_data },
    { "MIRAACC", (kernel_ulong_t)&da280_match_data },
    {}
};
MODULE_DEVICE_TABLE(acpi, da217_acpi_match);

static const struct of_device_id da217_of_match[] = {
    { .compatible = "da,da217", .data = &da217_match_data },
    { .compatible = "da,da226", .data = &da226_match_data },
    { .compatible = "da,da280", .data = &da280_match_data },
    {}
};
MODULE_DEVICE_TABLE(of, da217_of_match);

static const struct i2c_device_id da217_i2c_id[] = {
    { "da217", (kernel_ulong_t)&da217_match_data },
    { "da226", (kernel_ulong_t)&da226_match_data },
    { "da280", (kernel_ulong_t)&da280_match_data },
    {}
};
MODULE_DEVICE_TABLE(i2c, da217_i2c_id);

static struct i2c_driver da217_driver = {
    .driver = {
        .name = "da217",
        .acpi_match_table = da217_acpi_match,
        .of_match_table = da217_of_match,
        .pm = &da217_pm_ops,
    },
    .probe      = da217_probe,
    .id_table   = da217_i2c_id,
};
module_i2c_driver(da217_driver);

MODULE_AUTHOR("ZeroDreamCat <neko@0w0.cafe>");
MODULE_DESCRIPTION("MiraMEMS DA217 3-Axis Accelerometer driver");
MODULE_LICENSE("GPL v2");
