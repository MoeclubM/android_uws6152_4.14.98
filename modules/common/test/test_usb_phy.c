#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/device.h>

static struct power_supply *test_psy;
static char test_status[] = "Charging";

static int test_get_prop(struct power_supply *psy,
                         enum power_supply_property psp,
                         union power_supply_propval *val)
{
    switch (psp) {
    case POWER_SUPPLY_PROP_ONLINE:
        val->intval = 1;   // 充电器在线
        break;
    case POWER_SUPPLY_PROP_USB_TYPE:
        val->intval = POWER_SUPPLY_USB_TYPE_DCP;  // 随便报一个充电类型
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
    test_psy = power_supply_register(NULL, &test_desc, &cfg);
    if (IS_ERR(test_psy)) {
        pr_err("test_usb_psy: register failed\n");
        return PTR_ERR(test_psy);
    }
    pr_info("test_usb_psy: registered, check /sys/class/power_supply/usb\n");
    return 0;
}

static void __exit test_exit(void)
{
    if (test_psy) {
        power_supply_unregister(test_psy);
        pr_info("test_usb_psy: unregistered\n");
    }
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZeroDreamCat");
