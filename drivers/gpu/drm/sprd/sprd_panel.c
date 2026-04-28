/*
 * ICN3312 Panel driver from sprd simple panel by SoraNeko
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drm/drm_atomic_helper.h>
#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pm_runtime.h>
#include <video/mipi_display.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include "sprd_panel.h"
#include "dsi/sprd_dsi_api.h"
#include "sysfs/sysfs_display.h"

#define SPRD_MIPI_DSI_FMT_DSC 0xff

/* Backlight mode definitions */
#define BACKLIGHT_MODE_BYPASS		0
#define BACKLIGHT_MODE_DIRECT		1
#define BACKLIGHT_MODE_DUTY_DET_PWM	2
#define BACKLIGHT_MODE_DUTY_DET_ANALOG	3
#define BACKLIGHT_MODE_DUTY_DET_MIXED	4
#define BACKLIGHT_MODE_EXT_R_SINK	5
#define BACKLIGHT_MODE_MAX		6

/* Backlight control mode definitions */
#define BACKLIGHT_CTRL_IDAC		0
#define BACKLIGHT_CTRL_PWM		1
#define BACKLIGHT_CTRL_DEFAULT		BACKLIGHT_CTRL_PWM

/* Default PWM frequency to 20kHz and Max 33kHz */
#define BACKLIGHT_FREQ_DEFAULT		0	/* 20kHz */
#define BACKLIGHT_FREQ_MIN		20480
#define BACKLIGHT_FREQ_MAX		34816

/* PWM output duty range 0 ~ 256 */
#define BACKLIGHT_PWM_DUTY_MAX		255	/* 100% */

/* IDAC, Max = 40960 uA = 160uA * 255 steps */
#define BACKLIGHT_IDAC_MAX		40960
#define BACKLIGHT_IDAC_DEFAULT		0

/* This is for Direct mode/GPIO mode to set PWM default */
#define BACKLIGHT_PWM_DEFAULT		1000

#define BACKLIGHT_VDAC_MAX		1600000	/* uA */
#define BACKLIGHT_VDAC_MIN		20000	/* uA */
#define BACKLIGHT_VDAC_DEFAULT		0x95
#define BACKLIGHT_VDAC_SEL_DEFAULT	0x0

#define BACKLIGHT_PWM_DUTY_THRESH_MAX	255
#define BACKLIGHT_PWM_DUTY_THRESH_DEFAULT 50

#define BACKLIGHT_PWM_BRIGHTNESS_MAX	0xFFFF
#define BACKLIGHT_PWM_BRIGHTNESS_DEFAULT 100

#define BACKLIGHT_MIN_PWM_FREQ		25	/* kHz */
#define BACKLIGHT_IN_PWM_FREQ_STD	30	/* kHz */
#define BACKLIGHT_MAX_PWM_FREQ		35	/* kHz */

static DEFINE_MUTEX(panel_lock);

const char *lcd_name;
static int __init lcd_name_get(char *str)
{
	if (str != NULL)
		lcd_name = str;
	DRM_INFO("lcd name from uboot: %s\n", lcd_name);
	return 0;
}
__setup("lcd_name=", lcd_name_get);

static inline struct sprd_panel *to_sprd_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sprd_panel, base);
}

static int sprd_panel_send_cmds(struct mipi_dsi_device *dsi,
				const void *data, int size)
{
	struct sprd_panel *panel = mipi_dsi_get_drvdata(dsi);
	const struct dsi_cmd_desc *cmds = data;
	u16 len;

	if ((cmds == NULL) || (dsi == NULL))
		return -EINVAL;

	while (size > 0) {
		len = (cmds->wc_h << 8) | cmds->wc_l;

		/* 健壮性检查：确保长度合法，防止越界 */
		if (len + 4 > size) {
			DRM_ERROR("Invalid cmd length: len=%u, remaining size=%d\n",
				  len, size);
			return -EINVAL;
		}

		if (panel->info.use_dcs)
			mipi_dsi_dcs_write_buffer(dsi, cmds->payload, len);
		else
			mipi_dsi_generic_write(dsi, cmds->payload, len);

		if (cmds->wait)
			msleep(cmds->wait);
		cmds = (const struct dsi_cmd_desc *)(cmds->payload + len);
		size -= (len + 4);
	}

	return 0;
}

static int sprd_panel_unprepare(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct gpio_timing *timing;
	int items, i;

	DRM_INFO("%s()\n", __func__);

	if (panel->info.avee_gpio) {
		gpiod_direction_output(panel->info.avee_gpio, 0);
		mdelay(5);
	}

	if (panel->info.reset_gpio) {
		items = panel->info.rst_off_seq.items;
		timing = panel->info.rst_off_seq.timing;
		for (i = 0; i < items; i++) {
			gpiod_direction_output(panel->info.reset_gpio,
						timing[i].level);
			mdelay(timing[i].delay);
		}
	}

	if (panel->info.avdd_gpio) {
		mdelay(50);
		gpiod_direction_output(panel->info.avdd_gpio, 0);
		mdelay(5);
	}

	if (panel->supply)
		regulator_disable(panel->supply);

	DRM_INFO("[PANEL] unprepare done\n");

	return 0;
}

static int sprd_panel_prepare(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct gpio_timing *timing;
	int items, i, ret;

	DRM_INFO("%s()\n", __func__);

	if (panel->supply) {
		ret = regulator_enable(panel->supply);
		if (ret < 0) {
			DRM_ERROR("enable lcd regulator failed\n");
			return ret;
		}
	}

	if (panel->info.avee_gpio) {
		gpiod_direction_output(panel->info.avee_gpio, 1);
		mdelay(5);
	}

	if (panel->info.reset_gpio) {
		items = panel->info.rst_on_seq.items;
		timing = panel->info.rst_on_seq.timing;
		for (i = 0; i < items; i++) {
			gpiod_direction_output(panel->info.reset_gpio,
						timing[i].level);
			mdelay(timing[i].delay);
		}
	}

	if (panel->info.avdd_gpio) {
		mdelay(50);
		gpiod_direction_output(panel->info.avdd_gpio, 1);
		mdelay(5);
	}

	DRM_INFO("[PANEL] prepare done\n");
	return 0;
}

static int sprd_panel_disable(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct backlight_device *bl = NULL;

	mutex_lock(&panel_lock);
	/* 提前标记并释放锁，避免死锁 */
	if (panel->esd_work_pending) {
		panel->esd_work_pending = false;
		mutex_unlock(&panel_lock);
		cancel_delayed_work_sync(&panel->esd_work);
		mutex_lock(&panel_lock);
	}

	if (panel->backlight) {
		panel->backlight->props.power = FB_BLANK_POWERDOWN;
		panel->backlight->props.state |= BL_CORE_FBBLANK;
		bl = panel->backlight;
	}

	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_SLEEP_IN],
			     panel->info.cmds_len[CMD_CODE_SLEEP_IN]);

	panel->is_enabled = false;
	mutex_unlock(&panel_lock);

	if (bl)
		backlight_update_status(bl);

	return 0;
}

static int sprd_panel_enable(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct backlight_device *bl = NULL;

	mutex_lock(&panel_lock);
	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_INIT],
			     panel->info.cmds_len[CMD_CODE_INIT]);

	/* 先记录背光指针，但不要在锁内调 backlight_update_status */
	if (panel->backlight) {
		panel->backlight->props.power = FB_BLANK_UNBLANK;
		panel->backlight->props.state &= ~BL_CORE_FBBLANK;
		bl = panel->backlight;       // 锁外调用
	}

	if (panel->info.esd_check_en) {
		schedule_delayed_work(&panel->esd_work,
				      msecs_to_jiffies(1000));
		panel->esd_work_pending = true;
	}

	panel->is_enabled = true;
	mutex_unlock(&panel_lock);

	if (bl)
		backlight_update_status(bl);

	return 0;
}

static int sprd_panel_get_modes(struct drm_panel *p)
{
	struct drm_display_mode *mode;
	struct sprd_panel *panel = to_sprd_panel(p);
	struct device_node *np = panel->slave->dev.of_node;
	u32 surface_width = 0, surface_height = 0;
	int i, mode_count = 0;

	DRM_INFO("%s()\n", __func__);
	mode = drm_mode_duplicate(p->drm, &panel->info.mode);
	if (!mode) {
		DRM_ERROR("failed to alloc mode %s\n", panel->info.mode.name);
		return 0;
	}
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(p->connector, mode);
	mode_count++;

	for (i = 1; i < panel->info.num_biuldin_modes; i++) {
		mode = drm_mode_duplicate(p->drm,
			&(panel->info.buildin_modes[i]));
		if (!mode) {
			DRM_ERROR("failed to alloc mode %s\n",
				panel->info.buildin_modes[i].name);
			return 0;
		}
		mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_DEFAULT;
		drm_mode_probed_add(p->connector, mode);
		mode_count++;
	}

	of_property_read_u32(np, "sprd,surface-width", &surface_width);
	of_property_read_u32(np, "sprd,surface-height", &surface_height);
	if (surface_width && surface_height) {
		struct videomode vm = {};

		vm.hactive = surface_width;
		vm.vactive = surface_height;
		vm.pixelclock = surface_width * surface_height * 60;

		mode = drm_mode_create(p->drm);
		if (!mode) {
            DRM_ERROR("failed to create mode for surface\n");
            return mode_count;
        }
        
		mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
		mode->vrefresh = 60;
		drm_display_mode_from_videomode(&vm, mode);
		drm_mode_probed_add(p->connector, mode);
		mode_count++;
	}

	p->connector->display_info.width_mm = panel->info.mode.width_mm;
	p->connector->display_info.height_mm = panel->info.mode.height_mm;

	return mode_count;
}

static const struct drm_panel_funcs sprd_panel_funcs = {
	.get_modes = sprd_panel_get_modes,
	.enable = sprd_panel_enable,
	.disable = sprd_panel_disable,
	.prepare = sprd_panel_prepare,
	.unprepare = sprd_panel_unprepare,
};

static int sprd_panel_esd_check(struct sprd_panel *panel)
{
	struct panel_info *info = &panel->info;
	u8 read_val = 0;
	int ret;

	ret = mipi_dsi_dcs_read(panel->slave, info->esd_check_reg,
				&read_val, 1);
	if (ret < 0) {
		DRM_ERROR("esd check read failed: %d\n", ret);
		return ret;
	}

	/* 成功读取至少 1 字节 */
	if (ret != 1 || read_val != info->esd_check_val) {
		DRM_ERROR("esd check failed, read value = 0x%02x\n", read_val);
		return -EINVAL;
	}

	return 0;
}

static void sprd_panel_esd_work_func(struct work_struct *work)
{
	struct sprd_panel *panel = container_of(work, struct sprd_panel, esd_work);
	struct panel_info *info = &panel->info;
	int ret;

	mutex_lock(&panel_lock);
	if (!panel->is_enabled) {
		panel->esd_work_pending = false;
		mutex_unlock(&panel_lock);
		return;
	}

	ret = sprd_panel_esd_check(panel);
	if (ret) {
		panel->esd_work_pending = false;
		mutex_unlock(&panel_lock);

		/* 通过 panel 自身回调恢复，安全可靠 */
		sprd_panel_disable(&panel->base);
		sprd_panel_unprepare(&panel->base);
		sprd_panel_prepare(&panel->base);
		sprd_panel_enable(&panel->base);

		/* enable 中会重新调度 ESD work，无需额外操作 */
	} else {
		schedule_delayed_work(&panel->esd_work,
				      msecs_to_jiffies(info->esd_check_period));
		mutex_unlock(&panel_lock);
	}
}

static int sprd_panel_gpio_request(struct device *dev, struct sprd_panel *panel)
{
	struct gpio_desc *gpio;
	int ret;

	gpio = devm_gpiod_get_optional(dev, "avdd", GPIOD_ASIS);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		if (ret == -EPROBE_DEFER)
			return ret;
		DRM_WARN("Failed to get avdd gpio: %d\n", ret);
		panel->info.avdd_gpio = NULL;
	} else {
		panel->info.avdd_gpio = gpio;
	}

	gpio = devm_gpiod_get_optional(dev, "avee", GPIOD_ASIS);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		if (ret == -EPROBE_DEFER)
			return ret;
		DRM_WARN("Failed to get avee gpio: %d\n", ret);
		panel->info.avee_gpio = NULL;
	} else {
		panel->info.avee_gpio = gpio;
	}

	gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		if (ret == -EPROBE_DEFER)
			return ret;
		DRM_WARN("Failed to get reset gpio: %d\n", ret);
		panel->info.reset_gpio = NULL;
	} else {
		panel->info.reset_gpio = gpio;
	}

	return 0;
}

static int of_parse_reset_seq(struct device *dev, struct device_node *np,
			      struct panel_info *info)
{
	struct property *prop;
	int bytes, rc;
	u32 *p;

	prop = of_find_property(np, "sprd,reset-on-sequence", &bytes);
	if (!prop) {
		DRM_ERROR("sprd,reset-on-sequence property not found\n");
		return -EINVAL;
	}

	p = devm_kzalloc(dev, bytes, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	rc = of_property_read_u32_array(np, "sprd,reset-on-sequence",
					p, bytes / 4);
	if (rc) {
		DRM_ERROR("parse sprd,reset-on-sequence failed\n");
		return rc;
	}

	info->rst_on_seq.items = bytes / 8;
	info->rst_on_seq.timing = (struct gpio_timing *)p;

	prop = of_find_property(np, "sprd,reset-off-sequence", &bytes);
	if (!prop) {
		DRM_ERROR("sprd,reset-off-sequence property not found\n");
		return -EINVAL;
	}

	p = devm_kzalloc(dev, bytes, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	rc = of_property_read_u32_array(np, "sprd,reset-off-sequence",
					p, bytes / 4);
	if (rc) {
		DRM_ERROR("parse sprd,reset-off-sequence failed\n");
		return rc;
	}

	info->rst_off_seq.items = bytes / 8;
	info->rst_off_seq.timing = (struct gpio_timing *)p;

	return 0;
}

static int of_get_buildin_modes(struct panel_info *info,
	struct device_node *lcd_node, struct device *dev)
{
	int i, rc, num_timings;
	struct device_node *timings_np;

	timings_np = of_get_child_by_name(lcd_node, "display-timings");
	if (!timings_np) {
		DRM_ERROR("%s: can not find display-timings node\n",
			lcd_node->name);
		return -ENODEV;
	}

	num_timings = of_get_child_count(timings_np);
	if (num_timings == 0) {
		/* should never happen, as entry was already found above */
		DRM_ERROR("%s: no timings specified\n", lcd_node->name);
		rc = -ENODEV;
		goto done;
	}

	info->buildin_modes = devm_kcalloc(dev, num_timings,
					   sizeof(struct drm_display_mode),
					   GFP_KERNEL);
	if (!info->buildin_modes) {
		rc = -ENOMEM;
		goto done;
	}

	for (i = 0; i < num_timings; i++) {
		rc = of_get_drm_display_mode(lcd_node,
			&info->buildin_modes[i], NULL, i);
		if (rc) {
			DRM_ERROR("get display timing failed\n");
			goto entryfail;
		}

		info->buildin_modes[i].width_mm = info->mode.width_mm;
		info->buildin_modes[i].height_mm = info->mode.height_mm;
		info->buildin_modes[i].vrefresh = info->mode.vrefresh;
	}
	info->num_biuldin_modes = num_timings;
	DRM_INFO("info->num_buildin_modes = %d\n", num_timings);
	rc = 0;
	goto done;

entryfail:
	devm_kfree(dev, info->buildin_modes);
	info->buildin_modes = NULL;
	rc = -EINVAL;
done:
	of_node_put(timings_np);
	return rc;
}

static int sprd_panel_parse_dt(struct device_node *np, struct sprd_panel *panel,
			       struct device *dev)
{
	u32 val;
	struct device_node *lcd_node;
	struct panel_info *info = &panel->info;
	int bytes, rc;
	const void *p;
	const char *str;
	char lcd_path[60];

	rc = of_property_read_string(np, "sprd,force-attached", &str);
	if (!rc)
		lcd_name = str;

	if (!lcd_name) {
		DRM_ERROR("No lcd name provided (uboot lcd_name= or sprd,force-attached missing)\n");
		return -EINVAL;
	}

	sprintf(lcd_path, "/lcds/%s", lcd_name);
	lcd_node = of_find_node_by_path(lcd_path);
	if (!lcd_node) {
		DRM_ERROR("%pOF: could not find %s node\n", np, lcd_name);
		return -ENODEV;
	}
	info->of_node = lcd_node;

	rc = of_property_read_u32(lcd_node, "sprd,dsi-work-mode", &val);
	if (!rc) {
		if (val == SPRD_DSI_MODE_CMD)
			info->mode_flags = 0;
		else if (val == SPRD_DSI_MODE_VIDEO_BURST)
			info->mode_flags = MIPI_DSI_MODE_VIDEO |
					   MIPI_DSI_MODE_VIDEO_BURST;
		else if (val == SPRD_DSI_MODE_VIDEO_SYNC_PULSE)
			info->mode_flags = MIPI_DSI_MODE_VIDEO |
					   MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
		else if (val == SPRD_DSI_MODE_VIDEO_SYNC_EVENT)
			info->mode_flags = MIPI_DSI_MODE_VIDEO;
	} else {
		DRM_ERROR("dsi work mode is not found! use video mode\n");
		info->mode_flags = MIPI_DSI_MODE_VIDEO |
				   MIPI_DSI_MODE_VIDEO_BURST;
	}

	if (of_property_read_bool(lcd_node, "sprd,dsi-non-continuous-clock"))
		info->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;

	rc = of_property_read_u32(lcd_node, "sprd,dsi-lane-number", &val);
	if (!rc)
		info->lanes = val;
	else
		info->lanes = 4;

	rc = of_property_read_string(lcd_node, "sprd,dsi-color-format", &str);
	if (rc)
		info->format = MIPI_DSI_FMT_RGB888;
	else if (!strcmp(str, "rgb888"))
		info->format = MIPI_DSI_FMT_RGB888;
	else if (!strcmp(str, "rgb666"))
		info->format = MIPI_DSI_FMT_RGB666;
	else if (!strcmp(str, "rgb666_packed"))
		info->format = MIPI_DSI_FMT_RGB666_PACKED;
	else if (!strcmp(str, "rgb565"))
		info->format = MIPI_DSI_FMT_RGB565;
	else if (!strcmp(str, "dsc"))
		info->format = SPRD_MIPI_DSI_FMT_DSC;
	else {
	    info->format = MIPI_DSI_FMT_RGB888;
		DRM_ERROR("dsi-color-format (%s) is not supported\n", str);
	}

	rc = of_property_read_u32(lcd_node, "sprd,width-mm", &val);
	if (!rc)
		info->mode.width_mm = val;
	else
		info->mode.width_mm = 68;

	rc = of_property_read_u32(lcd_node, "sprd,height-mm", &val);
	if (!rc)
		info->mode.height_mm = val;
	else
		info->mode.height_mm = 121;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-enable", &val);
	if (!rc)
		info->esd_check_en = val;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-mode", &val);
	if (!rc)
		info->esd_check_mode = val;
	else
		info->esd_check_mode = 1;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-period", &val);
	if (!rc)
		info->esd_check_period = val;
	else
		info->esd_check_period = 1000;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-register", &val);
	if (!rc)
		info->esd_check_reg = val;
	else
		info->esd_check_reg = 0x0A;

	rc = of_property_read_u32(lcd_node, "sprd,esd-check-value", &val);
	if (!rc)
		info->esd_check_val = val;
	else
		info->esd_check_val = 0x9C;

	if (of_property_read_bool(lcd_node, "sprd,use-dcs-write"))
		info->use_dcs = true;
	else
		info->use_dcs = false;

	rc = of_parse_reset_seq(dev, lcd_node, info);
	if (rc) {
		DRM_ERROR("parse lcd reset sequence failed\n");
		goto parse_error;
	}

	p = of_get_property(lcd_node, "sprd,initial-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_INIT] = p;
		info->cmds_len[CMD_CODE_INIT] = bytes;
	} else
		DRM_ERROR("can't find sprd,initial-command property\n");

	p = of_get_property(lcd_node, "sprd,sleep-in-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_SLEEP_IN] = p;
		info->cmds_len[CMD_CODE_SLEEP_IN] = bytes;
	} else
		DRM_ERROR("can't find sprd,sleep-in-command property\n");

	p = of_get_property(lcd_node, "sprd,sleep-out-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_SLEEP_OUT] = p;
		info->cmds_len[CMD_CODE_SLEEP_OUT] = bytes;
	} else
		DRM_ERROR("can't find sprd,sleep-out-command property\n");

	rc = of_get_drm_display_mode(lcd_node, &info->mode, 0,
				     OF_USE_NATIVE_MODE);
	if (rc) {
		DRM_ERROR("get display timing failed\n");
		goto parse_error;
	}

	info->mode.vrefresh = drm_mode_vrefresh(&info->mode);
	of_get_buildin_modes(info, lcd_node, dev);
	DRM_INFO("[PANEL] lcd node: %s\n", lcd_node->name);
	DRM_INFO("[PANEL] lanes=%d, format=%d, mode_flags=%d\n",
		 info->lanes, info->format, info->mode_flags);
	DRM_INFO("[PANEL] resolution=%dx%d\n",
		 info->mode.hdisplay, info->mode.vdisplay);
	DRM_INFO("[PANEL] width_mm=%d, height_mm=%d\n",
		 info->mode.width_mm, info->mode.height_mm);
	if (info->cmds[CMD_CODE_INIT])
		DRM_INFO("[PANEL] init-command size=%d\n",
			 info->cmds_len[CMD_CODE_INIT]);
	else
		DRM_WARN("[PANEL] no init-command found\n");

	/* Parse backlight related properties */
	rc = of_property_read_u32(lcd_node, "sprd,max-brightness", &val);
	if (!rc) {
		if (val <= BACKLIGHT_PWM_BRIGHTNESS_MAX)
			info->cmds_len[CMD_OLED_BRIGHTNESS] = val;
		else
			info->cmds_len[CMD_OLED_BRIGHTNESS] = BACKLIGHT_PWM_BRIGHTNESS_DEFAULT;
	} else
		info->cmds_len[CMD_OLED_BRIGHTNESS] = BACKLIGHT_PWM_BRIGHTNESS_DEFAULT;

	return 0;

parse_error:
	of_node_put(lcd_node);
	info->of_node = NULL;
	return rc;
}

static int sprd_panel_device_create(struct device *parent,
				    struct sprd_panel *panel)
{
	panel->dev.class = display_class;
	panel->dev.parent = parent;
	panel->dev.of_node = panel->info.of_node;
	dev_set_name(&panel->dev, "panel0");
	dev_set_drvdata(&panel->dev, panel);

	return device_register(&panel->dev);
}

static int sprd_backlight_set_brightness(struct backlight_device *bdev)
{
	int level, brightness;
	struct sprd_backlight *backlight = bl_get_data(bdev);
	struct sprd_panel *panel = backlight->panel;

	if (!panel || !panel->slave)
		return 0;

	mutex_lock(&panel_lock);
	if (!panel->is_enabled) {
		mutex_unlock(&panel_lock);
		DRM_WARN("panel has been powered off\n");
		return -ENXIO;
	}

	brightness = bdev->props.brightness;

	level = (brightness * backlight->max_level) / 255;
	level = min(level, 255);
	
	DRM_INFO("[BACKLIGHT] brightness=%d -> level=%d (cmds_total=%d)\n",
		 brightness, level, backlight->cmds_total);

	if (backlight->cmds_total == 1) {
		backlight->cmds[0]->payload[1] = level;
		sprd_panel_send_cmds(panel->slave,
				     backlight->cmds[0],
				     backlight->cmd_len);
	} else
		sprd_panel_send_cmds(panel->slave,
				     backlight->cmds[level],
				     backlight->cmd_len);

	mutex_unlock(&panel_lock);

	return 0;
}

static const struct backlight_ops sprd_backlight_ops = {
	.update_status = sprd_backlight_set_brightness,
};

static int sprd_backlight_init(struct sprd_panel *panel)
{
	struct sprd_backlight *backlight;
	struct device_node *bl_node;
	struct panel_info *info = &panel->info;
	const void *p;
	struct dsi_cmd_desc *cmd;
	int bytes, rc;
	u32 temp;

	DRM_INFO("[BACKLIGHT] start init\n");

	bl_node = of_get_child_by_name(info->of_node, "backlight");
	if (!bl_node)
		bl_node = of_get_child_by_name(info->of_node, "oled-backlight");
	if (!bl_node) {
		DRM_INFO("[BACKLIGHT] no backlight node, skip\n");
		return 0;
	}
	DRM_INFO("[BACKLIGHT] found node: %s\n", bl_node->name);

	backlight = devm_kzalloc(&panel->dev,
				 sizeof(struct sprd_backlight), GFP_KERNEL);
	if (!backlight) {
		of_node_put(bl_node);
		return -ENOMEM;
	}

	backlight->panel = panel;

	/* 获取 brightness-levels 属性 */
	p = of_get_property(bl_node, "brightness-levels", &bytes);
	if (!p) {
		DRM_ERROR("[BACKLIGHT] no brightness-levels property\n");
		of_node_put(bl_node);
		return -EINVAL;
	}
	DRM_INFO("[BACKLIGHT] brightness-levels size = %d bytes\n", bytes);

	// 无论 DTS 中是什么数据，统一构造一条标准亮度命令
	cmd = devm_kzalloc(&panel->dev,
		sizeof(struct dsi_cmd_desc) + 2, GFP_KERNEL);
	if (!cmd) {
		of_node_put(bl_node);
		return -ENOMEM;
	}

	cmd->data_type = 0x15;
	cmd->wait = 0;
	cmd->wc_h = 0;
	cmd->wc_l = 2;
	cmd->payload[0] = 0x51;
	cmd->payload[1] = 0;

	backlight->cmds[0] = cmd;
	backlight->cmds_total = 1;
	backlight->cmd_len = 6;

	/* 这里注册 backlight 设备，避免注册后再失败留下残废 sysfs 节点 */
	backlight->bdev = devm_backlight_device_register(&panel->dev,
			"sprd_backlight", &panel->dev, backlight,
			&sprd_backlight_ops, NULL);
	if (IS_ERR(backlight->bdev)) {
		DRM_ERROR("failed to register backlight ops\n");
		of_node_put(bl_node);
		return PTR_ERR(backlight->bdev);
	}

	rc = of_property_read_u32(bl_node, "default-brightness-level", &temp);
	if (!rc)
		backlight->bdev->props.brightness = temp;
	else
		backlight->bdev->props.brightness = 25;

	rc = of_property_read_u32(bl_node, "sprd,max-level", &temp);
	if (!rc)
		backlight->max_level = temp;
	else
		backlight->max_level = 255;

	backlight->bdev->props.max_brightness = 255;
	panel->backlight = backlight->bdev;

	of_node_put(bl_node);

	DRM_INFO("%s() Neko wants to eat your screen!\n", __func__);
	return 0;
}

static int sprd_panel_probe(struct mipi_dsi_device *slave)
{
	int ret;
	struct sprd_panel *panel;

	panel = devm_kzalloc(&slave->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	DRM_INFO("Hello!Do you want some coffee?\n");

	panel->supply = devm_regulator_get_optional(&slave->dev, "power");
	if (IS_ERR(panel->supply)) {
		if (PTR_ERR(panel->supply) == -ENODEV) {
			/* DTS 没配 power-supply，认为是固定电源/GPIO 直接供电 */
			DRM_INFO("No panel regulator found, assume fixed supply\n");
			panel->supply = NULL;
		} else {
			DRM_ERROR("Failed to get panel regulator: %ld\n",
				  PTR_ERR(panel->supply));
			return PTR_ERR(panel->supply);
		}
	}

	INIT_DELAYED_WORK(&panel->esd_work, sprd_panel_esd_work_func);

	ret = sprd_panel_parse_dt(slave->dev.of_node, panel, &slave->dev);
	if (ret) {
		DRM_ERROR("parse panel info failed\n");
		return ret;
	}

	ret = sprd_panel_gpio_request(&slave->dev, panel);
	if (ret) {
		DRM_WARN("gpio request failed\n");
		goto err_put_node;
	}

	panel->slave = slave;
	mipi_dsi_set_drvdata(slave, panel);

	ret = sprd_panel_device_create(&slave->dev, panel);
	if (ret) {
		DRM_ERROR("panel device create failed\n");
		goto err_put_node;
	}

	panel->base.dev = &panel->dev;
	panel->base.funcs = &sprd_panel_funcs;
	drm_panel_init(&panel->base);

	ret = drm_panel_add(&panel->base);
	if (ret) {
		DRM_ERROR("drm_panel_add() failed\n");
		goto err_device_unregister;
	}

	slave->lanes = panel->info.lanes;
	slave->format = panel->info.format;
	slave->mode_flags = panel->info.mode_flags;

	ret = mipi_dsi_attach(slave);
	if (ret) {
		DRM_ERROR("failed to attach dsi panel to host\n");
		goto err_panel_remove;
	}

	sprd_panel_sysfs_init(&panel->dev);

	ret = sprd_backlight_init(panel);
	if (ret) {
		DRM_ERROR("backlight init failed\n");
		goto err_dsi_detach;
	}

	DRM_INFO("My coffee is cold....\n");

	return 0;

err_dsi_detach:
	mipi_dsi_detach(slave);
err_panel_remove:
	drm_panel_remove(&panel->base);
err_device_unregister:
	device_unregister(&panel->dev);
err_put_node:
	if (panel->info.of_node) {
		of_node_put(panel->info.of_node);
		panel->info.of_node = NULL;
	}
	return ret;
}

static int sprd_panel_remove(struct mipi_dsi_device *slave)
{
	struct sprd_panel *panel = mipi_dsi_get_drvdata(slave);
	int ret;

	sprd_panel_disable(&panel->base);
	sprd_panel_unprepare(&panel->base);

	ret = mipi_dsi_detach(slave);
	if (ret < 0)
		DRM_ERROR("failed to detach from DSI host: %d\n", ret);

	drm_panel_detach(&panel->base);
	drm_panel_remove(&panel->base);

	device_unregister(&panel->dev);

	/* 释放 of_node，因为 parse_dt 中增加了引用 */
	if (panel->info.of_node) {
		of_node_put(panel->info.of_node);
		panel->info.of_node = NULL;
	}

	return 0;
}

static const struct of_device_id panel_of_match[] = {
	{ .compatible = "sprd,generic-mipi-panel", },
	{ }
};
MODULE_DEVICE_TABLE(of, panel_of_match);

static struct mipi_dsi_driver sprd_panel_driver = {
	.driver = {
		.name = "sprd-mipi-panel-drv",
		.of_match_table = panel_of_match,
	},
	.probe = sprd_panel_probe,
	.remove = sprd_panel_remove,
};
module_mipi_dsi_driver(sprd_panel_driver);

MODULE_AUTHOR("ZeroDreamCat <neko@0w0.cafe>");
MODULE_DESCRIPTION("ICN3312 Simple Driver");
MODULE_LICENSE("GPL v2");