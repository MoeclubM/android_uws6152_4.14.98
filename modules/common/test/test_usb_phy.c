#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>

static struct power_supply *test_psy;
static struct platform_device *pdev;

static int test_get_prop(struct power_supply *psy,
                         enum power_supply_property psp,
                         union power_supply_propval *val)
{
    switch (psp) {
    case POWER_SUPPLY_PROP_ONLINE:
        val->intval = 1;
        break;
    case POWER_SUPPLY_PROP_USB_TYPE:
        val->intval = POWER_SUPPLY_USB_TYPE_DCP;
        break;
    case POWER_SUPPLY_PROP_STATUS:
        val->intval = POWER_SUPPLY_STATUS_CHARGING;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static enum power_supply_property test_props[] = {
    POWER_SUPPLY_PROP_ONLINE,
    POWER_SUPPLY_PROP_USB_TYPE,
    POWER_SUPPLY_PROP_STATUS,
};

static const struct power_supply_desc test_desc = {
    .name = "usb",
    .type = POWER_SUPPLY_TYPE_USB,
    .properties = test_props,
    .num_properties = ARRAY_SIZE(test_props),
    .get_property = test_get_prop,
};

static int __init test_init(void)
{
    struct power_supply_config cfg = {0};

    pdev = platform_device_register_simple("test_usb_psy", -1, NULL, 0);
    if (IS_ERR(pdev)) {
        pr_err("test_usb_psy: failed to create platform device\n");
        return PTR_ERR(pdev);
    }

    test_psy = power_supply_register(&pdev->dev, &test_desc, &cfg);
    if (IS_ERR(test_psy)) {
        pr_err("test_usb_psy: register failed, error = %ld\n",
               PTR_ERR(test_psy));
        platform_device_unregister(pdev);
        return PTR_ERR(test_psy);
    }

    pr_info("test_usb_psy: registered 'usb' power supply\n");
    return 0;
}

static void __exit test_exit(void)
{
    if (test_psy) {
        power_supply_unregister(test_psy);
        test_psy = NULL;
    }
    if (pdev) {
        platform_device_unregister(pdev);
        pdev = NULL;
    }
    pr_info("test_usb_psy: unregistered\n");
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZeroDreamCat");
