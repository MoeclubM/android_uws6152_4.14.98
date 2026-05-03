/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 *
 * Modified: compat layer for newer iommu interfaces,
 *           using global notifier (no struct modification required).
 */

#include <linux/module.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/clk.h>
#include <linux/miscdevice.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/genalloc.h>
#include <linux/sprd_iommu.h>
#include <linux/sprd_ion.h>
#include <linux/scatterlist.h>
#include <asm/io.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/dma-buf.h>
#include <linux/pm_runtime.h>
#include <linux/pm.h>

#include "sprd_iommu_sysfs.h"
#include "drv/com/sprd_com.h"

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
static int sprd_iommu_probe(struct platform_device *pdev);
static int sprd_iommu_remove(struct platform_device *pdev);

/* ============================================================================
 * Global notifier chain for orphan buffer cleanup
 * (single global block, no need to modify sprd_iommu_dev)
 * ============================================================================
 */
BLOCKING_NOTIFIER_HEAD(sprd_iommu_notif_chain);
EXPORT_SYMBOL_GPL(sprd_iommu_notif_chain);

int sprd_iommu_notifier_call_chain(void *data)
{
	return blocking_notifier_call_chain(&sprd_iommu_notif_chain, 0, data);
}
EXPORT_SYMBOL_GPL(sprd_iommu_notifier_call_chain);

/* Global notifier block, registered once in module init */
static struct notifier_block sprd_iommu_nb_global;

/* ============================================================================
 * IOMMU device list
 * ============================================================================
 */
static struct sprd_iommu_list_data sprd_iommu_list[SPRD_IOMMU_MAX] = {
	{ .iommu_id = SPRD_IOMMU_VSP,   .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_DCAM,  .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_DCAM1, .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_CPP,   .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_GSP,   .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_JPG,   .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_DISP,  .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_ISP,   .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_ISP1,  .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_FD,    .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_AI,    .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_EPP,   .enabled = false, .iommu_dev = NULL },
	{ .iommu_id = SPRD_IOMMU_EDP,   .enabled = false, .iommu_dev = NULL },
};

/* ============================================================================
 * Device tree match table
 * ============================================================================
 */
static const struct of_device_id sprd_iommu_ids[] = {
	{ .compatible = "sprd,iommuexl3-dispc", .data = (void *)(IOMMU_EXL3_DISP) },
	{ .compatible = "sprd,iommuexl3-vsp",   .data = (void *)(IOMMU_EXL3_VSP) },
	{ .compatible = "sprd,iommuexl3-dcam",  .data = (void *)(IOMMU_EXL3_DCAM) },
	{ .compatible = "sprd,iommuexl3-isp",   .data = (void *)(IOMMU_EXL3_ISP) },
	{ .compatible = "sprd,iommuexl3-cpp",   .data = (void *)(IOMMU_EXL3_CPP) },
	{ .compatible = "sprd,iommuexl3-jpg",   .data = (void *)(IOMMU_EXL3_JPG) },
	{ .compatible = "sprd,iommuexl5-dispc", .data = (void *)(IOMMU_EXL5_DISP) },
	{ .compatible = "sprd,iommuexl5-vsp",   .data = (void *)(IOMMU_EXL5_VSP) },
	{ .compatible = "sprd,iommuexl5-dcam",  .data = (void *)(IOMMU_EXL5_DCAM) },
	{ .compatible = "sprd,iommuexl5-isp",   .data = (void *)(IOMMU_EXL5_ISP) },
	{ .compatible = "sprd,iommuexl5-cpp",   .data = (void *)(IOMMU_EXL5_CPP) },
	{ .compatible = "sprd,iommuexl5-jpg",   .data = (void *)(IOMMU_EXL5_JPG) },
	{ .compatible = "sprd,iommuexl5-fd",    .data = (void *)(IOMMU_EXL5_FD) },
	{ .compatible = "sprd,iommuexroc1-dispc", .data = (void *)(IOMMU_EXROC1_DISP) },
	{ .compatible = "sprd,iommuexroc1-vsp",   .data = (void *)(IOMMU_EXROC1_VSP) },
	{ .compatible = "sprd,iommuexroc1-dcam",  .data = (void *)(IOMMU_EXROC1_DCAM) },
	{ .compatible = "sprd,iommuexroc1-isp",   .data = (void *)(IOMMU_EXROC1_ISP) },
	{ .compatible = "sprd,iommuexroc1-cpp",   .data = (void *)(IOMMU_EXROC1_CPP) },
	{ .compatible = "sprd,iommuexroc1-jpg",   .data = (void *)(IOMMU_EXROC1_JPG) },
	{ .compatible = "sprd,iommuexroc1-fd",    .data = (void *)(IOMMU_EXROC1_FD) },
	{ .compatible = "sprd,iommuexroc1-ai",    .data = (void *)(IOMMU_EXROC1_AI) },
	{ .compatible = "sprd,iommuexroc1-epp",   .data = (void *)(IOMMU_EXROC1_EPP) },
	{ .compatible = "sprd,iommuexroc1-edp",   .data = (void *)(IOMMU_EXROC1_EDP) },
	{ .compatible = "sprd,iommuvaul5p-dispc", .data = (void *)(IOMMU_VAUL5P_DISP) },
	{ .compatible = "sprd,iommuvaul5p-vsp",   .data = (void *)(IOMMU_VAUL5P_VSP) },
	{ .compatible = "sprd,iommuvaul5p-dcam",  .data = (void *)(IOMMU_VAUL5P_DCAM) },
	{ .compatible = "sprd,iommuvaul5p-isp",   .data = (void *)(IOMMU_VAUL5P_ISP) },
	{ .compatible = "sprd,iommuvaul5p-cpp",   .data = (void *)(IOMMU_VAUL5P_CPP) },
	{ .compatible = "sprd,iommuvaul5p-jpg",   .data = (void *)(IOMMU_VAUL5P_JPG) },
	{ .compatible = "sprd,iommuvaul5p-fd",    .data = (void *)(IOMMU_VAUL5P_FD) },
	{ .compatible = "sprd,iommuvaul5p-ai",    .data = (void *)(IOMMU_VAUL5P_AI) },
	{ .compatible = "sprd,iommuvaul5p-epp",   .data = (void *)(IOMMU_VAUL5P_EPP) },
	{ .compatible = "sprd,iommuvaul5p-edp",   .data = (void *)(IOMMU_VAUL5P_EDP) },
};

static struct platform_driver iommu_driver = {
	.probe = sprd_iommu_probe,
	.remove = sprd_iommu_remove,
	.driver = {
		.name = "sprd_iommu_drv",
		.of_match_table = sprd_iommu_ids,
	},
};

/* ============================================================================
 * Utility functions (identical to original)
 * ============================================================================
 */
static void sprd_iommu_set_list(struct sprd_iommu_dev *iommu_dev)
{
	if (!iommu_dev) {
		IOMMU_ERR("iommu_dev == NULL!\n");
		return;
	}

	switch (iommu_dev->init_data->id) {
	case IOMMU_EX_VSP:
		sprd_iommu_list[SPRD_IOMMU_VSP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_VSP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_VSP;
		break;
	case IOMMU_EX_DCAM:
		sprd_iommu_list[SPRD_IOMMU_DCAM].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_DCAM].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_DCAM;
		break;
	case IOMMU_EX_CPP:
		sprd_iommu_list[SPRD_IOMMU_CPP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_CPP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_CPP;
		break;
	case IOMMU_EX_GSP:
		sprd_iommu_list[SPRD_IOMMU_GSP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_GSP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_GSP;
		break;
	case IOMMU_EX_JPG:
		sprd_iommu_list[SPRD_IOMMU_JPG].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_JPG].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_JPG;
		break;
	case IOMMU_EX_DISP:
		sprd_iommu_list[SPRD_IOMMU_DISP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_DISP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_DISP;
		break;
	case IOMMU_EX_ISP:
	case IOMMU_EX_NEWISP:
		sprd_iommu_list[SPRD_IOMMU_ISP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_ISP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_ISP;
		break;
	case IOMMU_EX_EDP:
		sprd_iommu_list[SPRD_IOMMU_EDP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_EDP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_EDP;
		break;
	case IOMMU_EX_FD:
		sprd_iommu_list[SPRD_IOMMU_FD].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_FD].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_FD;
		break;
	case IOMMU_EX_AI:
		sprd_iommu_list[SPRD_IOMMU_AI].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_AI].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_AI;
		break;
	case IOMMU_EX_EPP:
		sprd_iommu_list[SPRD_IOMMU_EPP].enabled = true;
		sprd_iommu_list[SPRD_IOMMU_EPP].iommu_dev = iommu_dev;
		iommu_dev->id = SPRD_IOMMU_EPP;
		break;
	default:
		IOMMU_ERR("no iommu id: %d\n", iommu_dev->init_data->id);
		iommu_dev->id = -1;
		break;
	}
}

static bool sprd_iommu_is_dev_valid_master(struct device *dev)
{
	struct device_node *np = dev->of_node;
	int ret = of_count_phandle_with_args(np, "iommus", "#iommu-cells");
	return (ret > 0);
}

static struct sprd_iommu_dev *sprd_iommu_get_subnode(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(np, "iommus", "#iommu-cells", 0, &args);
	if (ret) {
		IOMMU_ERR("of_parse_phandle_with_args(%s) => %d\n", np->full_name, ret);
		return NULL;
	}
	if (args.args_count != 0) {
		IOMMU_ERR("err param for %s (found %d, expected 0)\n",
		       args.np->full_name, args.args_count);
		return NULL;
	}
	return args.np->data;
}

static struct sprd_iommu_dev *sprd_iommu_get_subnode_with_idx(struct device *dev, int idx)
{
	struct device_node *np = dev->of_node;
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_args(np, "iommus", "#iommu-cells", idx, &args);
	if (ret) {
		IOMMU_ERR("of_parse_phandle_with_args(%s) idx=%d => %d\n",
		       np->full_name, idx, ret);
		return NULL;
	}
	if (args.args_count != 0) {
		IOMMU_ERR("err param for %s (found %d, expected 0)\n",
		       args.np->full_name, args.args_count);
		return NULL;
	}
	return args.np->data;
}

/* ----- sg pool helpers ----- */
static bool sprd_iommu_target_buf(struct sprd_iommu_dev *iommu_dev,
				  void *buf_addr, unsigned long *iova_addr)
{
	int i;
	for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
		if (iommu_dev->sg_pool.slot[i].status == SG_SLOT_USED &&
		    iommu_dev->sg_pool.slot[i].buf_addr == buf_addr) {
			*iova_addr = iommu_dev->sg_pool.slot[i].iova_addr;
			iommu_dev->sg_pool.slot[i].map_usrs++;
			return true;
		}
	}
	return false;
}

static bool sprd_iommu_target_iova_find_buf(struct sprd_iommu_dev *iommu_dev,
					    unsigned long iova_addr, size_t iova_size,
					    void **buf)
{
	int i;
	for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
		if (iommu_dev->sg_pool.slot[i].status == SG_SLOT_USED &&
		    iommu_dev->sg_pool.slot[i].iova_addr == iova_addr &&
		    iommu_dev->sg_pool.slot[i].iova_size == iova_size) {
			*buf = iommu_dev->sg_pool.slot[i].buf_addr;
			return true;
		}
	}
	return false;
}

static bool sprd_iommu_target_buf_find_iova(struct sprd_iommu_dev *iommu_dev,
					    void *buf_addr, size_t iova_size,
					    unsigned long *iova_addr)
{
	int i;
	for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
		if (iommu_dev->sg_pool.slot[i].status == SG_SLOT_USED &&
		    iommu_dev->sg_pool.slot[i].buf_addr == buf_addr &&
		    iommu_dev->sg_pool.slot[i].iova_size == iova_size) {
			*iova_addr = iommu_dev->sg_pool.slot[i].iova_addr;
			return true;
		}
	}
	return false;
}

static bool sprd_iommu_insert_slot(struct sprd_iommu_dev *iommu_dev,
				   unsigned long sg_table_addr, void *buf_addr,
				   unsigned long iova_addr, unsigned long iova_size)
{
	int i;
	struct sprd_iommu_sg_rec *rec;
	for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
		rec = &iommu_dev->sg_pool.slot[i];
		if (rec->status == SG_SLOT_FREE) {
			rec->sg_table_addr = sg_table_addr;
			rec->buf_addr = buf_addr;
			rec->iova_addr = iova_addr;
			rec->iova_size = iova_size;
			rec->status = SG_SLOT_USED;
			rec->map_usrs++;
			iommu_dev->sg_pool.pool_cnt++;
			return true;
		}
	}
	return false;
}

static bool sprd_iommu_remove_sg_iova(struct sprd_iommu_dev *iommu_dev,
				      unsigned long iova_addr, bool *be_free)
{
	int i;
	for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
		if (iommu_dev->sg_pool.slot[i].status == SG_SLOT_USED &&
		    iommu_dev->sg_pool.slot[i].iova_addr == iova_addr) {
			iommu_dev->sg_pool.slot[i].map_usrs--;
			if (iommu_dev->sg_pool.slot[i].map_usrs == 0) {
				iommu_dev->sg_pool.slot[i].sg_table_addr = 0;
				iommu_dev->sg_pool.slot[i].iova_addr = 0;
				iommu_dev->sg_pool.slot[i].iova_size = 0;
				iommu_dev->sg_pool.slot[i].status = SG_SLOT_FREE;
				iommu_dev->sg_pool.pool_cnt--;
				*be_free = true;
			} else {
				*be_free = false;
			}
			return true;
		}
	}
	return false;
}

static bool sprd_iommu_clear_sg_iova(struct sprd_iommu_dev *iommu_dev,
				     void *buf_addr, unsigned long sg_addr,
				     unsigned long size, unsigned long *iova)
{
	int i;
	struct sprd_iommu_sg_rec *rec;
	for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
		rec = &iommu_dev->sg_pool.slot[i];
		if (rec->status == SG_SLOT_USED &&
		    rec->buf_addr == buf_addr &&
		    rec->sg_table_addr == sg_addr &&
		    rec->iova_size == size) {
			rec->map_usrs = 0;
			rec->status = SG_SLOT_FREE;
			iommu_dev->sg_pool.pool_cnt--;
			*iova = rec->iova_addr;
			return true;
		}
	}
	return false;
}

/* ============================================================================
 * Notifier callback for orphan buffer cleanup (global, traverses all devices)
 * ============================================================================
 */
static int sprd_iommu_notify_callback(struct notifier_block *nb,
                                      unsigned long action, void *data)
{
    void *buf = data;  /* data is ion_buffer pointer */
    int i;

    if (!buf)
        return NOTIFY_OK;

    /* For each enabled IOMMU device, force unmapping of this buffer */
    for (i = 0; i < SPRD_IOMMU_MAX; i++) {
        struct sprd_iommu_dev *dev = sprd_iommu_list[i].iommu_dev;
        struct sprd_iommu_sg_rec *rec;
        int j;
        unsigned long flags;

        if (!dev || !sprd_iommu_list[i].enabled)
            continue;

        spin_lock_irqsave(&dev->pgt_lock, flags);
        for (j = 0; j < SPRD_MAX_SG_CACHED_CNT; j++) {
            rec = &dev->sg_pool.slot[j];
            if (rec->status == SG_SLOT_USED && rec->buf_addr == buf) {
                unsigned long iova = rec->iova_addr;
                size_t size = rec->iova_size;

                /* Clear the cache slot */
                rec->map_usrs = 0;
                rec->status = SG_SLOT_FREE;
                dev->sg_pool.pool_cnt--;

                spin_unlock_irqrestore(&dev->pgt_lock, flags);
                /* Unmap from hardware */
                dev->ops->iova_unmap(dev, iova, size);
                dev->map_count--;
                dev->ops->iova_free(dev, iova, size);
                IOMMU_ERR("%s: force orphan unmap buf %p iova 0x%lx size 0x%zx\n",
                          dev->init_data->name, buf, iova, size);
                spin_lock_irqsave(&dev->pgt_lock, flags);
                /* Restart scanning, as the pool may have changed */
                j = -1;
            }
        }
        spin_unlock_irqrestore(&dev->pgt_lock, flags);
    }

    return NOTIFY_OK;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================
 */
int sprd_iommu_attach_device(struct device *dev)
{
	struct device_node *np;
	struct of_phandle_args args;
	const char *status;
	int ret;

	if (!dev) {
		IOMMU_ERR("null parameter err!\n");
		return -EINVAL;
	}

	np = dev->of_node;
	ret = of_parse_phandle_with_args(np, "iommus", "#iommu-cells", 0, &args);
	if (!ret) {
		ret = of_property_read_string(args.np, "status", &status);
		if (!ret) {
			if (!strcmp("disabled", status))
				return -EPERM;
			return 0;
		}
		return -EPERM;
	}
	return -EPERM;
}
EXPORT_SYMBOL(sprd_iommu_attach_device);

int sprd_iommu_dettach_device(struct device *dev)
{
	if (!dev) {
		IOMMU_ERR("null parameter err!\n");
		return -EINVAL;
	}
	return 0;
}

int sprd_iommu_map(struct device *dev, struct sprd_iommu_map_data *data)
{
	int ret = 0;
	struct sprd_iommu_dev *iommu_dev = NULL;
	unsigned long iova = 0;
	unsigned long flag = 0;
	bool buf_cached = false, buf_insert = true;
	struct sg_table *table = NULL;

	if (!dev || !data) {
		IOMMU_ERR("null parameter err! dev %p data %p\n", dev, data);
		return -EINVAL;
	}
	if (!data->buf) {
		IOMMU_ERR("null buf pointer!\n");
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode(dev);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&iommu_dev->pgt_lock, flag);

	ret = sprd_ion_get_sg(data->buf, &table);
	if (ret || !table) {
		IOMMU_ERR("%s get sg error, buf %p size 0x%zx ret %d table %p\n",
		       iommu_dev->init_data->name, data->buf, data->iova_size, ret, table);
		data->iova_addr = 0;
		ret = -EINVAL;
		goto out;
	}

	buf_cached = sprd_iommu_target_buf(iommu_dev, data->buf, (unsigned long *)&iova);
	if (buf_cached) {
		data->iova_addr = iova;
		ret = 0;
		IOMMU_DEBUG("%s cached iova 0x%lx size 0x%zx buf %p\n",
			 iommu_dev->init_data->name, iova, data->iova_size, data->buf);
		goto out;
	}

	iova = iommu_dev->ops->iova_alloc(iommu_dev, data->iova_size, data);
	if (iova == 0) {
		IOMMU_ERR("%s alloc error iova 0x%lx size 0x%zx buf %p\n",
		       iommu_dev->init_data->name, iova, data->iova_size, data->buf);
		data->iova_addr = 0;
		ret = -ENOMEM;
		goto out;
	}

	ret = iommu_dev->ops->iova_map(iommu_dev, iova, data->iova_size, table, data);
	if (ret) {
		IOMMU_ERR("%s error, iova 0x%lx size 0x%zx ret %d buf %p\n",
		       iommu_dev->init_data->name, iova, data->iova_size, ret, data->buf);
		iommu_dev->ops->iova_free(iommu_dev, iova, data->iova_size);
		data->iova_addr = 0;
		ret = -ENOMEM;
		goto out;
	}

	iommu_dev->map_count++;
	data->iova_addr = iova;
	buf_insert = sprd_iommu_insert_slot(iommu_dev, (unsigned long)table,
					    data->buf, data->iova_addr, data->iova_size);
	if (!buf_insert) {
		IOMMU_ERR("%s error pool full iova 0x%lx size 0x%zx buf %p\n",
		       iommu_dev->init_data->name, iova, data->iova_size, data->buf);
		iommu_dev->ops->iova_unmap(iommu_dev, iova, data->iova_size);
		iommu_dev->ops->iova_free(iommu_dev, iova, data->iova_size);
		ret = -ENOMEM;
		goto out;
	}

	IOMMU_DEBUG("%s iova 0x%lx size 0x%zx buf %p\n",
		 iommu_dev->init_data->name, iova, data->iova_size, data->buf);
	spin_unlock_irqrestore(&iommu_dev->pgt_lock, flag);
	return ret;

out:
	spin_unlock_irqrestore(&iommu_dev->pgt_lock, flag);
	return ret;
}
EXPORT_SYMBOL(sprd_iommu_map);

int sprd_iommu_unmap(struct device *dev, struct sprd_iommu_unmap_data *data)
{
	int ret = 0;
	struct sprd_iommu_dev *iommu_dev = NULL;
	bool be_free = false;
	unsigned long flag = 0;
	bool valid_iova, valid_buf;
	void *buf;
	unsigned long iova;

	if (!dev || !data) {
		IOMMU_ERR("null parameter err! dev %p data %p\n", dev, data);
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode(dev);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&iommu_dev->pgt_lock, flag);

	valid_iova = sprd_iommu_target_iova_find_buf(iommu_dev, data->iova_addr,
						     data->iova_size, &buf);
	if (valid_iova) {
		iova = data->iova_addr;
	} else {
		valid_buf = sprd_iommu_target_buf_find_iova(iommu_dev, data->buf,
							    data->iova_size, &iova);
		if (valid_buf) {
			buf = data->buf;
			data->iova_addr = iova;
		} else {
			IOMMU_ERR("%s illegal error iova 0x%lx buf %p size 0x%zx\n",
			       iommu_dev->init_data->name, data->iova_addr,
			       data->buf, data->iova_size);
			ret = -EINVAL;
			goto out;
		}
	}

	sprd_iommu_remove_sg_iova(iommu_dev, iova, &be_free);
	if (be_free) {
		iommu_dev->ch_type = data->ch_type;
		iommu_dev->channel_id = data->channel_id;
		ret = iommu_dev->ops->iova_unmap(iommu_dev, iova, data->iova_size);
		if (ret)
			IOMMU_ERR("%s error iova 0x%lx 0x%zx buf %p\n",
			       iommu_dev->init_data->name, iova, data->iova_size, buf);
		iommu_dev->map_count--;
		iommu_dev->ops->iova_free(iommu_dev, iova, data->iova_size);
		iommu_dev->ch_type = SPRD_IOMMU_CH_TYPE_INVALID;
		IOMMU_DEBUG("%s iova 0x%lx size 0x%zx buf %p\n",
			 iommu_dev->init_data->name, iova, data->iova_size, buf);
	} else {
		IOMMU_DEBUG("%s cached iova 0x%lx size 0x%zx buf %p\n",
			 iommu_dev->init_data->name, iova, data->iova_size, buf);
		ret = 0;
	}

out:
	spin_unlock_irqrestore(&iommu_dev->pgt_lock, flag);
	return ret;
}
EXPORT_SYMBOL(sprd_iommu_unmap);

int sprd_iommu_unmap_orphaned(struct sprd_iommu_unmap_data *data)
{
    struct sprd_iommu_dev *iommu_dev;
    unsigned long flags;
    int i;

    if (!data || !data->buf) {
        IOMMU_ERR("null parameter error! data %p\n", data);
        return -EINVAL;
    }

    if (data->dev_id >= SPRD_IOMMU_MAX) {
        IOMMU_ERR("dev id error %d\n", data->dev_id);
        return -EINVAL;
    }

    iommu_dev = sprd_iommu_list[data->dev_id].iommu_dev;
    if (!iommu_dev) {
        IOMMU_ERR("dev not found for id %d\n", data->dev_id);
        return -EINVAL;
    }

    /* Loosen the search: just match buf_addr */
    spin_lock_irqsave(&iommu_dev->pgt_lock, flags);
    for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
        struct sprd_iommu_sg_rec *rec = &iommu_dev->sg_pool.slot[i];
        if (rec->status == SG_SLOT_USED && rec->buf_addr == data->buf) {
            unsigned long iova = rec->iova_addr;
            size_t size = rec->iova_size;

            rec->map_usrs = 0;
            rec->status = SG_SLOT_FREE;
            iommu_dev->sg_pool.pool_cnt--;

            spin_unlock_irqrestore(&iommu_dev->pgt_lock, flags);
            iommu_dev->ops->iova_unmap(iommu_dev, iova, size);
            iommu_dev->map_count--;
            iommu_dev->ops->iova_free(iommu_dev, iova, size);
            IOMMU_ERR("%s: orphan unmap buf %p iova 0x%lx size 0x%zx (dev_id %d)\n",
                      iommu_dev->init_data->name, data->buf, iova, size, data->dev_id);
            return 0;
        }
    }
    spin_unlock_irqrestore(&iommu_dev->pgt_lock, flags);

    IOMMU_ERR("%s: no orphan mapping for buf %p\n", iommu_dev->init_data->name, data->buf);
    return -EINVAL;
}

int sprd_iommu_suspend(struct device *dev)
{
	struct sprd_iommu_dev *iommu_dev;

	if (!dev) {
		IOMMU_ERR("null parameter err\n");
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode(dev);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev\n");
		return -EINVAL;
	}

	mutex_lock(&iommu_dev->status_mutex);
	if (iommu_dev->status_count) {
		iommu_dev->ops->suspend(iommu_dev);
		iommu_dev->status_count--;
	} else {
		IOMMU_ERR("%s have not enable!\n", iommu_dev->init_data->name);
	}
	mutex_unlock(&iommu_dev->status_mutex);
	return 0;
}

int sprd_iommu_resume(struct device *dev)
{
	struct sprd_iommu_dev *iommu_dev;

	if (!dev) {
		IOMMU_ERR("null parameter err\n");
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode(dev);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev\n");
		return -EINVAL;
	}

	mutex_lock(&iommu_dev->status_mutex);
	iommu_dev->ops->resume(iommu_dev);
	iommu_dev->status_count++;
	mutex_unlock(&iommu_dev->status_mutex);
	return 0;
}

int sprd_iommu_restore(struct device *dev)
{
	struct sprd_iommu_dev *iommu_dev;

	if (!dev) {
		IOMMU_ERR("null parameter err!\n");
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode(dev);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev\n");
		return -EINVAL;
	}

	if (iommu_dev->ops->restore)
		iommu_dev->ops->restore(iommu_dev);
	else
		return -1;

	sprd_iommu_pool_show(dev);
	return 0;
}
EXPORT_SYMBOL(sprd_iommu_restore);

int sprd_iommu_set_cam_bypass(bool vaor_bp_en)
{
	struct sprd_iommu_dev *iommu_dev;
	int ret = 0;

	iommu_dev = sprd_iommu_list[SPRD_IOMMU_DCAM].iommu_dev;
	if (iommu_dev && iommu_dev->ops->set_bypass)
		iommu_dev->ops->set_bypass(iommu_dev, vaor_bp_en);
	else
		ret = -1;

	iommu_dev = sprd_iommu_list[SPRD_IOMMU_ISP].iommu_dev;
	if (iommu_dev && iommu_dev->ops->set_bypass)
		iommu_dev->ops->set_bypass(iommu_dev, vaor_bp_en);
	else
		ret = -1;

	return ret;
}
EXPORT_SYMBOL(sprd_iommu_set_cam_bypass);

/* ============================================================================
 * Compatibility APIs matching new driver semantics
 * ============================================================================
 */
int sprd_iommu_map_single_page(struct device *dev, struct sprd_iommu_map_data *data)
{
    if (!dev || !data)
        return -EINVAL;
    return sprd_iommu_map(dev, data);
}
EXPORT_SYMBOL_GPL(sprd_iommu_map_single_page);

int sprd_iommu_map_with_idx(struct device *dev, struct sprd_iommu_map_data *data, int idx)
{
	int ret = 0;
	struct sprd_iommu_dev *iommu_dev = NULL;
	unsigned long iova = 0;
	unsigned long flag = 0;
	bool buf_cached = false, buf_insert = true;
	struct sg_table *table = NULL;

	if (!dev || !data) {
		IOMMU_ERR("null parameter err! dev %p data %p\n", dev, data);
		return -EINVAL;
	}
	if (!data->buf) {
		IOMMU_ERR("null buf pointer!\n");
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode_with_idx(dev, idx);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev idx %d\n", idx);
		return -EINVAL;
	}

	spin_lock_irqsave(&iommu_dev->pgt_lock, flag);

	ret = sprd_ion_get_sg(data->buf, &table);
	if (ret || !table) {
		IOMMU_ERR("%s get sg error, buf %p size 0x%zx ret %d table %p\n",
		       iommu_dev->init_data->name, data->buf, data->iova_size, ret, table);
		data->iova_addr = 0;
		ret = -EINVAL;
		goto out;
	}

	buf_cached = sprd_iommu_target_buf(iommu_dev, data->buf, (unsigned long *)&iova);
	if (buf_cached) {
		data->iova_addr = iova;
		ret = 0;
		IOMMU_DEBUG("%s cached iova 0x%lx size 0x%zx buf %p\n",
			 iommu_dev->init_data->name, iova, data->iova_size, data->buf);
		goto out;
	}

	iova = iommu_dev->ops->iova_alloc(iommu_dev, data->iova_size, data);
	if (iova == 0) {
		IOMMU_ERR("%s alloc error iova 0x%lx size 0x%zx buf %p\n",
		       iommu_dev->init_data->name, iova, data->iova_size, data->buf);
		data->iova_addr = 0;
		ret = -ENOMEM;
		goto out;
	}

	ret = iommu_dev->ops->iova_map(iommu_dev, iova, data->iova_size, table, data);
	if (ret) {
		IOMMU_ERR("%s error, iova 0x%lx size 0x%zx ret %d buf %p\n",
		       iommu_dev->init_data->name, iova, data->iova_size, ret, data->buf);
		iommu_dev->ops->iova_free(iommu_dev, iova, data->iova_size);
		data->iova_addr = 0;
		ret = -ENOMEM;
		goto out;
	}

	iommu_dev->map_count++;
	data->iova_addr = iova;
	buf_insert = sprd_iommu_insert_slot(iommu_dev, (unsigned long)table,
					    data->buf, data->iova_addr, data->iova_size);
	if (!buf_insert) {
		IOMMU_ERR("%s error pool full iova 0x%lx size 0x%zx buf %p\n",
		       iommu_dev->init_data->name, iova, data->iova_size, data->buf);
		iommu_dev->ops->iova_unmap(iommu_dev, iova, data->iova_size);
		iommu_dev->ops->iova_free(iommu_dev, iova, data->iova_size);
		ret = -ENOMEM;
		goto out;
	}

	IOMMU_DEBUG("%s iova 0x%lx size 0x%zx buf %p\n",
		 iommu_dev->init_data->name, iova, data->iova_size, data->buf);
	spin_unlock_irqrestore(&iommu_dev->pgt_lock, flag);
	return ret;

out:
	spin_unlock_irqrestore(&iommu_dev->pgt_lock, flag);
	return ret;
}
EXPORT_SYMBOL_GPL(sprd_iommu_map_with_idx);

int sprd_iommu_unmap_with_idx(struct device *dev, struct sprd_iommu_unmap_data *data, int idx)
{
	int ret = 0;
	struct sprd_iommu_dev *iommu_dev = NULL;
	bool be_free = false;
	unsigned long flag = 0;
	bool valid_iova, valid_buf;
	void *buf;
	unsigned long iova;

	if (!dev || !data) {
		IOMMU_ERR("null parameter err! dev %p data %p\n", dev, data);
		return -EINVAL;
	}
	if (!sprd_iommu_is_dev_valid_master(dev)) {
		IOMMU_ERR("illegal master\n");
		return -EINVAL;
	}

	iommu_dev = sprd_iommu_get_subnode_with_idx(dev, idx);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev idx %d\n", idx);
		return -EINVAL;
	}

	spin_lock_irqsave(&iommu_dev->pgt_lock, flag);

	valid_iova = sprd_iommu_target_iova_find_buf(iommu_dev, data->iova_addr,
						     data->iova_size, &buf);
	if (valid_iova) {
		iova = data->iova_addr;
	} else {
		valid_buf = sprd_iommu_target_buf_find_iova(iommu_dev, data->buf,
							    data->iova_size, &iova);
		if (valid_buf) {
			buf = data->buf;
			data->iova_addr = iova;
		} else {
			IOMMU_ERR("%s illegal error iova 0x%lx buf %p size 0x%zx\n",
			       iommu_dev->init_data->name, data->iova_addr,
			       data->buf, data->iova_size);
			ret = -EINVAL;
			goto out;
		}
	}

	sprd_iommu_remove_sg_iova(iommu_dev, iova, &be_free);
	if (be_free) {
		iommu_dev->ch_type = data->ch_type;
		iommu_dev->channel_id = data->channel_id;
		ret = iommu_dev->ops->iova_unmap(iommu_dev, iova, data->iova_size);
		if (ret)
			IOMMU_ERR("%s error iova 0x%lx 0x%zx buf %p\n",
			       iommu_dev->init_data->name, iova, data->iova_size, buf);
		iommu_dev->map_count--;
		iommu_dev->ops->iova_free(iommu_dev, iova, data->iova_size);
		iommu_dev->ch_type = SPRD_IOMMU_CH_TYPE_INVALID;
		IOMMU_DEBUG("%s iova 0x%lx size 0x%zx buf %p\n",
			 iommu_dev->init_data->name, iova, data->iova_size, buf);
	} else {
		IOMMU_DEBUG("%s cached iova 0x%lx size 0x%zx buf %p\n",
			 iommu_dev->init_data->name, iova, data->iova_size, buf);
		ret = 0;
	}

out:
	spin_unlock_irqrestore(&iommu_dev->pgt_lock, flag);
	return ret;
}
EXPORT_SYMBOL_GPL(sprd_iommu_unmap_with_idx);

void sprd_iommu_pool_show(struct device *dev)
{
	struct sprd_iommu_dev *iommu_dev;
	int i;
	struct sprd_iommu_sg_rec *rec;

	if (!dev) {
		IOMMU_ERR("null parameter err!\n");
		return;
	}
	iommu_dev = sprd_iommu_get_subnode(dev);
	if (!iommu_dev) {
		IOMMU_ERR("get null iommu dev\n");
		return;
	}

	if (iommu_dev->id == SPRD_IOMMU_VSP || iommu_dev->id == SPRD_IOMMU_DISP)
		return;

	IOMMU_ERR("%s restore, map_count %u\n",
	       iommu_dev->init_data->name, iommu_dev->map_count);

	if (iommu_dev->map_count > 0) {
		for (i = 0; i < SPRD_MAX_SG_CACHED_CNT; i++) {
			rec = &iommu_dev->sg_pool.slot[i];
			if (rec->status == SG_SLOT_USED) {
				IOMMU_ERR("Warning! buffer iova 0x%lx size 0x%lx sg 0x%lx buf %p map_usrs %d\n",
				       rec->iova_addr, rec->iova_size,
				       rec->sg_table_addr, rec->buf_addr, rec->map_usrs);
			}
		}
	}
}
EXPORT_SYMBOL_GPL(sprd_iommu_pool_show);

void sprd_iommu_reg_show(struct device *dev)
{
	/* intentionally empty */
}
EXPORT_SYMBOL_GPL(sprd_iommu_reg_show);

/* ============================================================================
 * Probe / Remove / Module Init
 * ============================================================================
 */

static int sprd_iommu_get_resource(struct device_node *np,
				   struct sprd_iommu_init_data *pdata)
{
	int err = 0;
	uint32_t val;
	struct resource res;
	struct device_node *np_memory;
	uint32_t out_values[4];

	err = of_address_to_resource(np, 0, &res);
	if (err < 0)
		return err;
	pdata->pgt_base = (unsigned long)ioremap_nocache(res.start, resource_size(&res));
	BUG_ON(pdata->pgt_base == 0);
	pdata->pgt_size = resource_size(&res);

	err = of_address_to_resource(np, 1, &res);
	if (err < 0)
		return err;
	pdata->ctrl_reg = (unsigned long)ioremap_nocache(res.start, resource_size(&res));
	BUG_ON(!pdata->ctrl_reg);

	err = of_property_read_u32(np, "iova-base", &val);
	if (err < 0)
		return err;
	pdata->iova_base = val;
	err = of_property_read_u32(np, "iova-size", &val);
	if (err < 0)
		return err;
	pdata->iova_size = val;

	err = of_property_read_u32(np, "sprd,reserved-fault-page", &val);
	if (err) {
		unsigned long page = __get_free_page(GFP_KERNEL);
		if (page)
			pdata->fault_page = virt_to_phys((void *)page);
		else
			pdata->fault_page = 0;
	} else {
		pdata->fault_page = val;
	}

	err = of_property_read_u32(np, "sprd,reserved-rr-page", &val);
	if (err)
		pdata->re_route_page = 0;
	else
		pdata->re_route_page = val;

	np_memory = of_parse_phandle(np, "memory-region", 0);
	if (!np_memory) {
		pdata->pagt_base_ddr = 0;
		pdata->pagt_ddr_size = 0;
	} else {
#ifdef CONFIG_64BIT
		err = of_property_read_u32_array(np_memory, "reg", out_values, 4);
		if (!err) {
			pdata->pagt_base_ddr = out_values[0];
			pdata->pagt_base_ddr = pdata->pagt_base_ddr << 32;
			pdata->pagt_base_ddr |= out_values[1];
			pdata->pagt_ddr_size = out_values[2];
			pdata->pagt_ddr_size = pdata->pagt_ddr_size << 32;
			pdata->pagt_ddr_size |= out_values[3];
		}
#else
		err = of_property_read_u32_array(np_memory, "reg", out_values, 2);
		if (!err) {
			pdata->pagt_base_ddr = out_values[0];
			pdata->pagt_ddr_size = out_values[1];
		}
#endif
	}
	return 0;
}

static int sprd_iommu_probe(struct platform_device *pdev)
{
	int err;
	struct device_node *np = pdev->dev.of_node;
	struct sprd_iommu_dev *iommu_dev;
	struct sprd_iommu_init_data *pdata;

	IOMMU_INFO("start\n");

	iommu_dev = kzalloc(sizeof(*iommu_dev), GFP_KERNEL);
	if (!iommu_dev)
		return -ENOMEM;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		kfree(iommu_dev);
		return -ENOMEM;
	}

	pdata->id = (int)((enum IOMMU_ID)(of_match_node(sprd_iommu_ids, np)->data));

	switch (pdata->id) {
	case IOMMU_EXL3_VSP: case IOMMU_EXL3_DCAM: case IOMMU_EXL3_CPP:
	case IOMMU_EXL3_JPG: case IOMMU_EXL3_DISP: case IOMMU_EXL3_ISP:
		pdata->iommuex_rev = 9;
		iommu_dev->ops = &sprd_iommuex_hw_ops;
		if (pdata->id == IOMMU_EXL3_DISP) pdata->id = IOMMU_EX_DISP;
		else if (pdata->id == IOMMU_EXL3_VSP) pdata->id = IOMMU_EX_VSP;
		else if (pdata->id == IOMMU_EXL3_DCAM) pdata->id = IOMMU_EX_DCAM;
		else if (pdata->id == IOMMU_EXL3_ISP) pdata->id = IOMMU_EX_ISP;
		else if (pdata->id == IOMMU_EXL3_CPP) pdata->id = IOMMU_EX_CPP;
		else if (pdata->id == IOMMU_EXL3_JPG) pdata->id = IOMMU_EX_JPG;
		break;
	case IOMMU_EXL5_VSP: case IOMMU_EXL5_DCAM: case IOMMU_EXL5_CPP:
	case IOMMU_EXL5_JPG: case IOMMU_EXL5_DISP: case IOMMU_EXL5_ISP:
	case IOMMU_EXL5_FD:
		pdata->iommuex_rev = 10;
		iommu_dev->ops = &sprd_iommuex_hw_ops;
		if (pdata->id == IOMMU_EXL5_DISP) pdata->id = IOMMU_EX_DISP;
		else if (pdata->id == IOMMU_EXL5_VSP) pdata->id = IOMMU_EX_VSP;
		else if (pdata->id == IOMMU_EXL5_DCAM) pdata->id = IOMMU_EX_DCAM;
		else if (pdata->id == IOMMU_EXL5_ISP) pdata->id = IOMMU_EX_NEWISP;
		else if (pdata->id == IOMMU_EXL5_CPP) pdata->id = IOMMU_EX_CPP;
		else if (pdata->id == IOMMU_EXL5_JPG) pdata->id = IOMMU_EX_JPG;
		else if (pdata->id == IOMMU_EXL5_FD) pdata->id = IOMMU_EX_FD;
		break;
	case IOMMU_EXROC1_VSP: case IOMMU_EXROC1_DCAM: case IOMMU_EXROC1_CPP:
	case IOMMU_EXROC1_JPG: case IOMMU_EXROC1_DISP: case IOMMU_EXROC1_ISP:
	case IOMMU_EXROC1_FD: case IOMMU_EXROC1_AI: case IOMMU_EXROC1_EPP:
	case IOMMU_EXROC1_EDP:
		pdata->iommuex_rev = 11;
		iommu_dev->ops = &sprd_iommuex_hw_ops;
		if (pdata->id == IOMMU_EXROC1_DISP) pdata->id = IOMMU_EX_DISP;
		else if (pdata->id == IOMMU_EXROC1_VSP) pdata->id = IOMMU_EX_VSP;
		else if (pdata->id == IOMMU_EXROC1_DCAM) pdata->id = IOMMU_EX_DCAM;
		else if (pdata->id == IOMMU_EXROC1_ISP) pdata->id = IOMMU_EX_NEWISP;
		else if (pdata->id == IOMMU_EXROC1_CPP) pdata->id = IOMMU_EX_CPP;
		else if (pdata->id == IOMMU_EXROC1_JPG) pdata->id = IOMMU_EX_JPG;
		else if (pdata->id == IOMMU_EXROC1_FD) pdata->id = IOMMU_EX_FD;
		else if (pdata->id == IOMMU_EXROC1_AI) pdata->id = IOMMU_EX_AI;
		else if (pdata->id == IOMMU_EXROC1_EPP) pdata->id = IOMMU_EX_EPP;
		else if (pdata->id == IOMMU_EXROC1_EDP) pdata->id = IOMMU_EX_EDP;
		break;
	case IOMMU_VAUL5P_VSP: case IOMMU_VAUL5P_DCAM: case IOMMU_VAUL5P_CPP:
	case IOMMU_VAUL5P_JPG: case IOMMU_VAUL5P_DISP: case IOMMU_VAUL5P_ISP:
	case IOMMU_VAUL5P_FD: case IOMMU_VAUL5P_AI: case IOMMU_VAUL5P_EPP:
	case IOMMU_VAUL5P_EDP:
		pdata->iommuex_rev = 12;
		iommu_dev->ops = &sprd_iommuvau_hw_ops;
		if (pdata->id == IOMMU_VAUL5P_DISP) pdata->id = IOMMU_EX_DISP;
		else if (pdata->id == IOMMU_VAUL5P_VSP) pdata->id = IOMMU_EX_VSP;
		else if (pdata->id == IOMMU_VAUL5P_DCAM) pdata->id = IOMMU_EX_DCAM;
		else if (pdata->id == IOMMU_VAUL5P_ISP) pdata->id = IOMMU_EX_NEWISP;
		else if (pdata->id == IOMMU_VAUL5P_CPP) pdata->id = IOMMU_EX_CPP;
		else if (pdata->id == IOMMU_VAUL5P_JPG) pdata->id = IOMMU_EX_JPG;
		else if (pdata->id == IOMMU_VAUL5P_FD) pdata->id = IOMMU_EX_FD;
		else if (pdata->id == IOMMU_VAUL5P_AI) pdata->id = IOMMU_EX_AI;
		else if (pdata->id == IOMMU_VAUL5P_EPP) pdata->id = IOMMU_EX_EPP;
		else if (pdata->id == IOMMU_VAUL5P_EDP) pdata->id = IOMMU_EX_EDP;
		break;
	default:
		IOMMU_ERR("unknown iommu id %d\n", pdata->id);
		err = -ENOMEM;
		goto errout;
	}

	err = sprd_iommu_get_resource(np, pdata);
	if (err) {
		IOMMU_ERR("no reg of property specified\n");
		goto errout;
	}

	iommu_dev->div2_frq = 500;
	iommu_dev->light_sleep_en = false;
	iommu_dev->drv_dev = &pdev->dev;
	iommu_dev->init_data = pdata;
	iommu_dev->map_count = 0;
	iommu_dev->status_count = 0;
	iommu_dev->ch_type = SPRD_IOMMU_CH_TYPE_INVALID;
	iommu_dev->channel_id = 0;
	iommu_dev->init_data->name = (char *)(of_match_node(sprd_iommu_ids, np)->compatible);

	err = iommu_dev->ops->init(iommu_dev, pdata);
	if (err) {
		IOMMU_ERR("iommu %s : failed init %d.\n", pdata->name, err);
		goto errout;
	}

	atomic_set(&iommu_dev->iommu_dev_cnt, 0);
	spin_lock_init(&iommu_dev->pgt_lock);
	mutex_init(&iommu_dev->status_mutex);
	memset(&iommu_dev->sg_pool, 0, sizeof(iommu_dev->sg_pool));

	sprd_iommu_sysfs_create(iommu_dev, iommu_dev->init_data->name);
	platform_set_drvdata(pdev, iommu_dev);

	np->data = iommu_dev;
	sprd_iommu_set_list(iommu_dev);

	pm_runtime_enable(&pdev->dev);
	IOMMU_INFO("%s end\n", iommu_dev->init_data->name);
	return 0;

errout:
	kfree(iommu_dev);
	kfree(pdata);
	return err;
}

static int sprd_iommu_remove(struct platform_device *pdev)
{
	struct sprd_iommu_dev *iommu_dev = platform_get_drvdata(pdev);

	sprd_iommu_sysfs_destroy(iommu_dev, iommu_dev->init_data->name);
	iommu_dev->ops->exit(iommu_dev);
	gen_pool_destroy(iommu_dev->pool);
	kfree(iommu_dev);
	return 0;
}

static int __init sprd_iommu_init(void)
{
	int err;

	IOMMU_INFO("begin\n");

	/* Register global iommu orphan notifier once */
	sprd_iommu_nb_global.notifier_call = sprd_iommu_notify_callback;
	blocking_notifier_chain_register(&sprd_iommu_notif_chain,
					 &sprd_iommu_nb_global);

	err = platform_driver_register(&iommu_driver);
	if (err < 0)
		IOMMU_ERR("sprd_iommu register err: %d\n", err);
	IOMMU_INFO("end\n");
	return err;
}

static void __exit sprd_iommu_exit(void)
{
	blocking_notifier_chain_unregister(&sprd_iommu_notif_chain,
					   &sprd_iommu_nb_global);
	platform_driver_unregister(&iommu_driver);
}

module_init(sprd_iommu_init);
module_exit(sprd_iommu_exit);

MODULE_DESCRIPTION("SPRD IOMMU Driver (compat)");
MODULE_LICENSE("GPL v2");
