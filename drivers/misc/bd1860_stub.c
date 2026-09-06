// SPDX-License-Identifier: GPL-2.0
/*
 * bd1860 PPG driver for UWS6152 (i2c@70500000 bd1860@40)
 *
 * Register knowledge reverse-engineered from the stock W527 kernel
 * driver (gxy_bd1860, stock_Image 0x56cf20..0x573400) and validated
 * live on the chip with i2cset/i2cdump:
 *  - mode-6 (HR) init table: 0xE8=0, 0xEE=7, 10ms, 0xE8=0, 0x03=0xA0,
 *    0x04=5, 0x05=0, 0x12=1, 0x13=0, 0x08=0, 0x09=1, 0x0A=0, 0x0B=1,
 *    0x0C=0, 0x0D=1, 0x0E=0x14, 0x0F=0xA, 0x10=0xA, 0x11=0x33,
 *    0x14=led0, 0x15=led1, 0x16=led2, 0x17=0x60, 0x18=0, 0x19=0x10,
 *    0x1A=0, 0x1B=0x10, 0x1C=0, 0x1D=0x1F, 0x1E=0x4F, 0x1F=0x2F,
 *    0x22=0xF0, 0x26=0, 0xEB=0x80, 0x07=1 (measurement on)
 *  - sample: write 0xEB=0x80 (convert trigger), then read banks:
 *    A = 0x27/0x28/0x29 (24-bit PPG ch1) + 0x2E/0x2F (aux16)
 *    B = 0x32/0x33/0x34 (24-bit PPG ch2) + 0x39/0x3A (aux16)
 *  - hrenable values '1'..'5' select chip modes like stock:
 *    1->6, 2->2, 3->6, 4->13, 5->3; '0' powers sampling off.
 *
 * HAL contract (vendor sensors.ums312.so readEvents TBB):
 *   REL_X=hr REL_Y=sbp REL_Z=dbp REL_RX=spo2 REL_RY=unworn-flag
 * (wear==1 -> SENSOR_STATUS_NO_CONTACT; input core drops zero values,
 *  so worn == "do not report REL_RY at all").
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#define RING_N		192	/* ~5s of samples */

struct bd1860_priv {
	struct i2c_client *client;
	struct input_dev *input;
	struct class *cls;
	struct device *clsdev;
	struct task_struct *thread;
	struct mutex lock;
	bool enabled;
	int mode;

	/* sample ring (index 0 = oldest) */
	u32 pa[RING_N], pb[RING_N];
	u32 ts[RING_N];		/* ms timestamp */
	int head, count;

	/* computed state for hrtest/debug */
	s64 dc_a, dc_b;
	u32 rms_a, rms_b;
	u32 hr;
	bool worn;
	u32 last_pa, last_pb;
	u32 ibi_val[16];
	u32 nibi, nibi_save;
	u32 med;
	u64 last_ms;
	/* reporting hold: keep last valid hr alive across short
	 * algorithm dropouts so the HAL never sees hr=0 while worn */
	u32 hr_hold;
	u64 hr_hold_ms;
	u32 unworn_cnt;
};

/* tunables, adjustable from userspace without reflashing.
 * worn detection: pulse AC on channel A (wrist ~23000 vs desk <=50,
 * live-measured 2026-09-05) or channel B; OR-ed to fail towards
 * "worn" because a false NO_CONTACT kills the app measurement. */
static unsigned int wear_ac_min = 100;
static unsigned int loop_ms = 22;
static unsigned int report_div = 8;
static unsigned int stat_s = 8;	/* dmesg stat interval, 0 = off */
module_param(wear_ac_min, uint, 0644);
module_param(loop_ms, uint, 0644);
module_param(report_div, uint, 0644);
module_param(stat_s, uint, 0644);

static int bd_wr(struct i2c_client *c, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(c, reg, val);

	if (ret)
		dev_err(&c->dev, "i2c wr %02x=%02x failed %d\n", reg, val, ret);
	return ret;
}

static int bd_rd(struct i2c_client *c, u8 reg)
{
	return i2c_smbus_read_byte_data(c, reg);
}

/* per-mode led config bytes for regs 0x14/0x15/0x16 (from stock) */
static const u8 mode_leds[4][3] = {
	{ 0x05, 0x00, 0x02 },	/* mode 6  (HR)   */
	{ 0x00, 0x08, 0x08 },	/* mode 2  (SPO2) */
	{ 0x00, 0x08, 0x08 },	/* mode 13 (VP)   */
	{ 0x05, 0x00, 0x02 },	/* others         */
};

static int bd_chip_start(struct bd1860_priv *st, int mode)
{
	static const u8 seq[][2] = {
		{0xE8, 0x00}, {0xEE, 0x07}, {0xE8, 0x00},
		{0x03, 0xA0}, {0x04, 0x05}, {0x05, 0x00},
		{0x12, 0x01}, {0x13, 0x00},
		{0x08, 0x00}, {0x09, 0x01}, {0x0A, 0x00},
		{0x0B, 0x01}, {0x0C, 0x00}, {0x0D, 0x01},
		{0x0E, 0x14}, {0x0F, 0x0A}, {0x10, 0x0A},
		{0x11, 0x33},
		{0x19, 0x10}, {0x1A, 0x00}, {0x1B, 0x10},
		{0x1C, 0x00}, {0x1D, 0x1F}, {0x1E, 0x4F},
		{0x1F, 0x2F}, {0x22, 0xF0}, {0x26, 0x00},
		{0xEB, 0x80},
	};
	struct i2c_client *c = st->client;
	const u8 *leds;
	int i, mi;

	switch (mode) {
	case 6:   mi = 0; break;
	case 2:   mi = 1; break;
	case 13:  mi = 2; break;
	default:  mi = 3; break;
	}
	leds = mode_leds[mi];

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		if (bd_wr(c, seq[i][0], seq[i][1]))
			return -EIO;
		if (i == 1)
			msleep(10);
	}
	bd_wr(c, 0x14, leds[0]);
	bd_wr(c, 0x15, leds[1]);
	bd_wr(c, 0x16, leds[2]);
	bd_wr(c, 0x17, 0x60);
	bd_wr(c, 0x18, 0x00);
	return bd_wr(c, 0x07, 0x01);
}

static void bd_chip_stop(struct bd1860_priv *st)
{
	bd_wr(st->client, 0x07, 0x00);
}

/* trigger conversion and read one 24-bit bank + aux16 */
static int bd_sample(struct bd1860_priv *st, bool bank_a,
		     u32 *out24, u32 *outaux)
{
	struct i2c_client *c = st->client;
	int lo, mid, hi, alo, ahi;
	u32 v;

	if (bd_wr(c, 0xEB, 0x80))
		return -EIO;
	usleep_range(2500, 3500);
	if (bank_a) {
		lo = bd_rd(c, 0x27);
		mid = bd_rd(c, 0x28);
		hi = bd_rd(c, 0x29);
		alo = bd_rd(c, 0x2E);
		ahi = bd_rd(c, 0x2F);
	} else {
		lo = bd_rd(c, 0x32);
		mid = bd_rd(c, 0x33);
		hi = bd_rd(c, 0x34);
		alo = bd_rd(c, 0x39);
		ahi = bd_rd(c, 0x3A);
	}
	if (lo < 0 || mid < 0 || hi < 0 || alo < 0 || ahi < 0)
		return -EIO;
	v = ((u32)hi << 16) | ((u32)mid << 8) | lo;
	*out24 = v;
	*outaux = ((u32)ahi << 8) | alo;
	return 0;
}

static u32 isqrt64(u64 x)
{
	u32 res = 0, bit = 1u << 30;

	while (bit > x)
		bit >>= 2;
	while (bit) {
		u64 t = (u64)res + bit;

		if (x >= t) {
			x -= t;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

/*
 * HR from the pa ring: EMA high-pass (kills motion baseline wander),
 * threshold scaled to the recent peak amplitude (not RMS, which motion
 * inflates until no peak ever crosses), timestamped IBIs, median with
 * a majority regularity gate. Returns bpm (0 = not confident).
 */
static u32 bd_compute_hr(struct bd1860_priv *st)
{
	int i, n = st->count;
	s64 ema = 0, acc = 0, pmax = 0;
	s64 hp[RING_N];
	u32 thr;
	int last_pk = -1;

	st->nibi = 0;
	st->med = 0;
	if (n < 48)
		return 0;

	/* 1-pole high-pass, alpha=1/16 (~0.3Hz cutoff @ ~31Hz rate);
	 * seed ema with the first sample or the step response swamps
	 * everything (hp starts at ~dc and decays for ~100 samples) */
	ema = st->pa[0];
	for (i = 0; i < n; i++) {
		ema += (st->pa[i] - ema) >> 4;
		hp[i] = st->pa[i] - ema;
		acc += hp[i] * hp[i];
		if (hp[i] > pmax)
			pmax = hp[i];
	}
	st->rms_a = isqrt64(div_s64(acc, n));
	st->dc_a = ema;
	/* floor at 2*rms and 30: pmax/4 alone lets quasi-regular noise
	 * wiggles on the desk through (live-measured hr=130 on desk) */
	thr = max_t(s64, pmax / 4, 30);
	thr = max_t(s64, thr, 2 * (s64)st->rms_a);

	/* walk oldest -> newest; only prominent peaks (>= pmax/3) may
	 * terminate an IBI, small inter-beat wiggles are skipped */
	for (i = 2; i < n - 2; i++) {
		s64 v = hp[i];
		u32 dt;

		if (v < thr || v < hp[i - 1] || v < hp[i - 2] ||
		    v <= hp[i + 1] || v <= hp[i + 2])
			continue;
		if (v * 3 >= pmax) {
			if (last_pk >= 0) {
				dt = st->ts[i] - st->ts[last_pk];
				if (dt < 300)		/* >200bpm impossible */
					continue;
				if (dt > 1500) {	/* >40bpm gap */
					st->nibi = 0;
					last_pk = i;
					continue;
				}
				if (st->nibi < 16)
					st->ibi_val[st->nibi++] = dt;
			}
			last_pk = i;
		}
	}
	st->nibi_save = st->nibi;
	if (st->nibi < 4) {
		st->hr = 0;
		return 0;
	}
	{
		u32 tmp[16];
		int j, k, inmed = 0;

		memcpy(tmp, st->ibi_val, st->nibi * sizeof(u32));
		for (j = 1; j < st->nibi; j++) {
			u32 key = tmp[j];

			for (k = j - 1; k >= 0 && tmp[k] > key; k--)
				tmp[k + 1] = tmp[k];
			tmp[k + 1] = key;
		}
		st->med = tmp[st->nibi / 2];
		if (st->med < 300 || st->med > 1500) {
			st->hr = 0;
			return 0;
		}
		/* majority gate: 60% of IBIs within 40% of the median */
		for (i = 0; i < st->nibi; i++) {
			u32 d = st->ibi_val[i] > st->med ?
				st->ibi_val[i] - st->med :
				st->med - st->ibi_val[i];

			if (d * 10 <= st->med * 4)
				inmed++;
		}
		if (inmed * 10 < st->nibi * 6) {
			st->hr = 0;
			return 0;
		}
	}
	st->hr = 60000 / st->med;
	return st->hr;
}

static int bd_thread(void *data)
{
	struct bd1860_priv *st = data;
	int div = 0;
	int stat_cnt = 0;

	while (!kthread_should_stop()) {
		u32 va = 0, vb = 0, aa = 0, ab = 0;
		s64 dcb = 0, accb = 0;
		int i, n;

		if (!st->enabled) {
			msleep(200);
			continue;
		}
		mutex_lock(&st->lock);
		if (!st->enabled) {
			mutex_unlock(&st->lock);
			continue;
		}
		if (bd_sample(st, true, &va, &aa) ||
		    bd_sample(st, false, &vb, &ab)) {
			mutex_unlock(&st->lock);
			msleep(200);
			continue;
		}
		st->pa[st->head] = va;
		st->pb[st->head] = vb;
		st->ts[st->head] = (u32)(ktime_get_ns() / 1000000) & 0x7fffffff;
		st->last_ms = ktime_get_ns();
		st->head = (st->head + 1) % RING_N;
		if (st->count < RING_N)
			st->count++;
		st->last_pa = va;
		st->last_pb = vb;

		n = st->count;
		for (i = 0; i < n; i++)
			dcb += st->pb[i];
		dcb = div_s64(dcb, n);
		st->dc_b = dcb;
		for (i = 0; i < n; i++) {
			s64 d = (s64)st->pb[i] - dcb;

			accb += d * d;
		}
		st->rms_b = isqrt64(div_s64(accb, n));
		/* pulse AC on either channel counts as worn; OR-ed so a
		 * marginal channel can't fake NO_CONTACT (app then fails).
		 * Hysteresis: leaving "worn" needs a sustained dropout, a
		 * momentary signal dip must not send a NO_CONTACT event. */
		if (!st->worn) {
			if (st->rms_a >= wear_ac_min ||
			    st->rms_b >= wear_ac_min) {
				st->worn = true;
				st->unworn_cnt = 0;
			}
		} else {
			if (st->rms_a < wear_ac_min / 4 &&
			    st->rms_b < wear_ac_min / 4) {
				st->unworn_cnt += loop_ms;
				if (st->unworn_cnt > 2000) {
					st->worn = false;
					st->unworn_cnt = 0;
				}
			} else {
				st->unworn_cnt = 0;
			}
		}

		bd_compute_hr(st);

		if (stat_s) {
			if (++stat_cnt * loop_ms >= stat_s * 1000) {
				stat_cnt = 0;
				dev_info(&st->client->dev,
					 "bd1860 stat: hr=%u hold=%u worn=%d dcA=%llu rmsA=%u dcB=%llu rmsB=%u nibi=%u med=%u\n",
					 st->hr, st->hr_hold, st->worn,
					 st->dc_a, st->rms_a, st->dc_b,
					 st->rms_b, st->nibi_save, st->med);
			}
		}

		if (++div >= report_div) {
			div = 0;
			if (st->worn) {
				u64 now = ktime_get_ns();

				if (st->hr) {
					/* fresh value: report + refresh hold */
					st->hr_hold = st->hr;
					st->hr_hold_ms = now;
					input_report_rel(st->input, REL_X,
							 st->hr);
				} else if (st->hr_hold &&
					   now - st->hr_hold_ms < 10000000000ULL) {
					/* brief algorithm dropout while worn:
					 * keep last valid hr so the HAL never
					 * publishes a zero */
					input_report_rel(st->input, REL_X,
							 st->hr_hold);
				}
				/* worn but nothing valid yet: report nothing */
			} else {
				/* unworn flag: HAL maps to NO_CONTACT */
				input_report_rel(st->input, REL_RY, 1);
			}
			input_sync(st->input);
		}
		mutex_unlock(&st->lock);
		msleep(loop_ms);
	}
	return 0;
}

static ssize_t hrenable_show(struct device *d, struct device_attribute *a,
			     char *b)
{
	struct bd1860_priv *st = dev_get_drvdata(d);

	return sprintf(b, "%d\n", st->enabled);
}

/* HAL writes one raw byte; values '0'..'5' (or raw 0..5) select modes
 * exactly like the stock hrenable_store: 1->6 2->2 3->6 4->13 5->3. */
static ssize_t hrenable_store(struct device *d, struct device_attribute *a,
			      const char *b, size_t c)
{
	struct bd1860_priv *st = dev_get_drvdata(d);
	int v;
	static const int tbl[6] = { 0, 6, 2, 6, 13, 3 };

	int ch;

	if (c < 1)
		return -EINVAL;
	ch = (unsigned char)b[0];
	if (ch <= 5 && c == 1)
		v = tbl[ch];
	else if (ch >= '0' && ch <= '5')
		v = tbl[ch - '0'];
	else
		return -EINVAL;

	mutex_lock(&st->lock);
	if (v == 0) {
		if (st->enabled) {
			bd_chip_stop(st);
			st->enabled = false;
		}
	} else {
		/* keep the sample ring across HAL's quick 1/0/1 cycling;
		 * only reset when the mode changes or data went stale */
		bool stale = !st->enabled ||
			     (ktime_get_ns() - st->last_ms) > 5000000000ULL;

		if (stale || st->mode != v) {
			if (bd_chip_start(st, v) == 0) {
				if (stale || st->mode != v) {
					st->head = st->count = 0;
					st->hr = 0;
					st->hr_hold = 0;
					st->worn = false;
					st->unworn_cnt = 0;
				}
				st->mode = v;
				st->enabled = true;
			}
		} else {
			st->enabled = true;
		}
	}
	mutex_unlock(&st->lock);
	dev_info(d, "bd1860 hrenable -> mode %d (enabled=%d)\n", v,
		 st->enabled);
	return c;
}

static struct device_attribute dev_attr_hrenable = {
	.attr = { .name = "hrenable", .mode = 0666 },
	.show = hrenable_show,
	.store = hrenable_store,
};

/* hrtest: live sampler state for on-device tuning */
static ssize_t hrtest_store(struct device *d, struct device_attribute *a,
			    const char *b, size_t c)
{
	struct bd1860_priv *st = dev_get_drvdata(d);
	int i;

	dev_info(d, "hrtest: en=%d mode=%d hr=%u worn=%d nibi=%u med=%u\n",
		 st->enabled, st->mode, st->hr, st->worn, st->nibi_save,
		 st->med);
	dev_info(d, "hrtest: dcA=%lld rmsA=%u dcB=%lld rmsB=%u\n",
		 st->dc_a, st->rms_a, st->dc_b, st->rms_b);
	dev_info(d, "hrtest: last pa=%u pb=%u n=%d\n",
		 st->last_pa, st->last_pb, st->count);
	for (i = 0; i < 24 && i < st->count; i++)
		dev_info(d, "hrtest: pa=%u pb=%u\n",
			 st->pa[(st->head - 1 - i + RING_N) % RING_N],
			 st->pb[(st->head - 1 - i + RING_N) % RING_N]);
	return c;
}

static ssize_t hrtest_show(struct device *d, struct device_attribute *a,
			   char *b)
{
	struct bd1860_priv *st = dev_get_drvdata(d);

	return sprintf(b, "en=%d mode=%d hr=%u worn=%d dcA=%lld rmsA=%u dcB=%lld rmsB=%u pa=%u pb=%u nibi=%u med=%u\n",
		       st->enabled, st->mode, st->hr, st->worn,
		       st->dc_a, st->rms_a, st->dc_b, st->rms_b,
		       st->last_pa, st->last_pb, st->nibi_save, st->med);
}

static struct device_attribute dev_attr_hrtest = {
	.attr = { .name = "hrtest", .mode = 0644 },
	.show = hrtest_show,
	.store = hrtest_store,
};

static int bd_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct bd1860_priv *st;
	int ret;

	dev_info(dev, "bd1860 real probe (UWS6152 w527)\n");
	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->client = client;
	mutex_init(&st->lock);

	st->input = devm_input_allocate_device(dev);
	if (!st->input)
		return -ENOMEM;
	st->input->name = "bd1860";
	st->input->id.bustype = BUS_I2C;
	input_set_capability(st->input, EV_REL, REL_X);
	input_set_capability(st->input, EV_REL, REL_Y);
	input_set_capability(st->input, EV_REL, REL_Z);
	input_set_capability(st->input, EV_REL, REL_RX);
	input_set_capability(st->input, EV_REL, REL_RY);
	ret = input_register_device(st->input);
	if (ret)
		return ret;

	st->cls = class_create(THIS_MODULE, "bd1860");
	if (IS_ERR(st->cls))
		return PTR_ERR(st->cls);
	st->clsdev = device_create(st->cls, dev, MKDEV(0, 0), st, "device");
	if (IS_ERR(st->clsdev)) {
		ret = PTR_ERR(st->clsdev);
		goto err_class;
	}
	ret = device_create_file(st->clsdev, &dev_attr_hrenable);
	if (ret)
		goto err_dev;
	ret = device_create_file(st->clsdev, &dev_attr_hrtest);
	if (ret)
		goto err_dev2;

	st->thread = kthread_run(bd_thread, st, "bd1860-sampling");
	if (IS_ERR(st->thread)) {
		ret = PTR_ERR(st->thread);
		goto err_dev3;
	}

	i2c_set_clientdata(client, st);
	dev_info(dev, "bd1860 ready: input/bd1860 + hrenable + hrtest\n");
	return 0;

err_dev3:
	device_remove_file(st->clsdev, &dev_attr_hrtest);
err_dev2:
	device_remove_file(st->clsdev, &dev_attr_hrenable);
err_dev:
	device_destroy(st->cls, MKDEV(0, 0));
err_class:
	class_destroy(st->cls);
	return ret;
}

static int bd_remove(struct i2c_client *client)
{
	struct bd1860_priv *st = i2c_get_clientdata(client);

	if (st->thread)
		kthread_stop(st->thread);
	mutex_lock(&st->lock);
	if (st->enabled)
		bd_chip_stop(st);
	mutex_unlock(&st->lock);
	device_remove_file(st->clsdev, &dev_attr_hrtest);
	device_remove_file(st->clsdev, &dev_attr_hrenable);
	device_destroy(st->cls, MKDEV(0, 0));
	class_destroy(st->cls);
	return 0;
}

static const struct of_device_id bd_of[] = {
	{ .compatible = "bd,bd1860" },
	{}
};
MODULE_DEVICE_TABLE(of, bd_of);

static const struct i2c_device_id bd_id[] = {
	{ "bd1860", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, bd_id);

static struct i2c_driver bd_drv = {
	.driver = {
		.name = "bd1860",
		.of_match_table = bd_of,
	},
	.probe = bd_probe,
	.remove = bd_remove,
	.id_table = bd_id,
};
module_i2c_driver(bd_drv);
MODULE_AUTHOR("Kokuban");
MODULE_DESCRIPTION("UWS6152 bd1860 PPG driver (real chip bring-up)");
MODULE_LICENSE("GPL");
