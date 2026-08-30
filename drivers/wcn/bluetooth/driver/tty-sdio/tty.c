/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/tty.h>
#include <linux/vt_kern.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#ifdef CONFIG_OF
#include <linux/of_device.h>
#endif
#include <linux/compat.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>

#include <misc/marlin_platform.h>
#include "tty.h"
#include "lpm.h"
#include "rfkill.h"
#include "dump.h"

#include "alignment/sitm.h"
#include "unisoc_bt_log.h"

#include <linux/notifier.h>
#include <misc/wcn_bus.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/serdev.h>

/* Bluetooth HCI definitions */
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci.h>
#include <net/bluetooth/hci_core.h>
#include "../../../../bluetooth/hci_uart.h"

static struct semaphore sem_id;

struct rx_data {
    unsigned int channel;
    struct mbuf_t *head;
    struct mbuf_t *tail;
    unsigned int num;
    struct list_head entry;
};

struct mtty_device {
    struct mtty_init_data   *pdata;
    struct tty_port *port;
    struct tty_struct   *tty;
    struct tty_driver   *driver;

    struct platform_device *pdev;
    /* mtty state */
    atomic_t state;
    /*spinlock_t    rw_lock;*/
    struct mutex    rw_mutex;
    struct list_head rx_head;
    /*struct tasklet_struct rx_task;*/
    struct work_struct bt_rx_work;
    struct workqueue_struct *bt_rx_workqueue;
    /* HCI protocol */
    int hci_proto;
    /* HCI device */
    struct hci_dev *hdev;
    unsigned long hci_flags;
};

typedef struct {
    unsigned long vir;
    unsigned long phy;
    int size;
} dm_t;

extern int set_power_ret;
static struct device *dm_rx_t = NULL;
unsigned long dm_rx_phy[BT_PCIE_RX_MAX_NUM];
unsigned char *(dm_rx_ptr[BT_PCIE_RX_MAX_NUM]);

struct dma_buf {
	unsigned long vir;
	unsigned long phy;
	int size;
};

struct mchn_ops_t bt_pcie_rx_ops;
struct mchn_ops_t bt_pcie_tx_ops;

static struct mtty_device *mtty_dev;
static unsigned int que_task = 1;
static int que_sche = 1;
static bool is_user_debug = false;
bt_host_data_dump *data_dump = NULL;
extern void sdiohal_dump_aon_reg(void);
struct device *ttyBT_dev = NULL;

static bool is_dumped = false;

static int wcn_hw_type = 0;

static ssize_t chipid_show(struct device *dev,
       struct device_attribute *attr, char *buf)
{
    int i = 0, id;
    const char *id_str = NULL;

    id = wcn_get_chip_type();
    id_str = wcn_get_chip_name();
    dev_unisoc_bt_info(ttyBT_dev,
                       "%s: chipid: %d, chipid_str: %s",
                       __func__, id, id_str);

    i = scnprintf(buf, PAGE_SIZE, "%d/%s", id, id_str ? id_str : "unknown");
    dev_unisoc_bt_info(ttyBT_dev,
                       "%s: chipid: %d, chipid_str: %s, result: %s\n",
                       __func__, id, id_str ? id_str : "NULL", buf);
    return i;
}

static ssize_t dumpmem_store(struct device *dev,
    struct device_attribute *attr, const char *buf, size_t count)
{
    if (buf[0] == '2') {
        dev_unisoc_bt_info(ttyBT_dev,
                           "Set is_user_debug true!\n");
        is_user_debug = true;
        return 0;
    }

    if (is_dumped == false) {
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty BT start dump cp mem !\n");
        mdbg_assert_interface("BT command timeout assert !!!");
        bt_host_data_printf();
        if (data_dump != NULL) {
            vfree(data_dump);
            data_dump = NULL;
        }
    } else {
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty BT has dumped cp mem, pls restart phone!\n");
    }
    is_dumped = true;

    return 0;
}

static DEVICE_ATTR_RO(chipid);
static DEVICE_ATTR_WO(dumpmem);

static struct attribute *bluetooth_attrs[] = {
    &dev_attr_chipid.attr,
    &dev_attr_dumpmem.attr,
    NULL,
};

static struct attribute_group bluetooth_group = {
    .name = NULL,
    .attrs = bluetooth_attrs,
};

/*static void hex_dump(unsigned char *bin, size_t binsz)
{
  char *str, hex_str[]= "0123456789ABCDEF";
  size_t i;

  str = (char *)vmalloc(binsz * 3);
  if (!str) {
    return;
  }

  for (i = 0; i < binsz; i++) {
      str[(i * 3) + 0] = hex_str[(bin[i] >> 4) & 0x0F];
      str[(i * 3) + 1] = hex_str[(bin[i]     ) & 0x0F];
      str[(i * 3) + 2] = ' ';
  }
  str[(binsz * 3) - 1] = 0x00;
  dev_unisoc_bt_info(ttyBT_dev,
                     "%s\n",
                     str);
  vfree(str);
}

static void hex_dump_block(unsigned char *bin, size_t binsz)
{
#define HEX_DUMP_BLOCK_SIZE 20
	int loop = binsz / HEX_DUMP_BLOCK_SIZE;
	int tail = binsz % HEX_DUMP_BLOCK_SIZE;
	int i;

	if (!loop) {
		hex_dump(bin, binsz);
		return;
	}

	for (i = 0; i < loop; i++) {
		hex_dump(bin + i * HEX_DUMP_BLOCK_SIZE, HEX_DUMP_BLOCK_SIZE);
	}

	if (tail)
		hex_dump(bin + i * HEX_DUMP_BLOCK_SIZE, tail);
}*/

int mtty_dmalloc(struct device *priv, struct dma_buf *dm, int size)
{
	struct device *dev = priv;

	if (!dev) {
		dev_unisoc_bt_err(ttyBT_dev,"%s(NULL)\n", __func__);
		return -1;
	}

	if (dma_set_mask(dev, DMA_BIT_MASK(64))) {
		dev_unisoc_bt_info(ttyBT_dev,"dma_set_mask err\n");
		if (dma_set_coherent_mask(dev, DMA_BIT_MASK(64))) {
			dev_unisoc_bt_err(ttyBT_dev,"dma_set_coherent_mask err\n");
			return -1;
		}
	}

	dm->vir =(unsigned long)dma_alloc_coherent(dev, size,
					      (dma_addr_t *)(&(dm->phy)),GFP_DMA);
	if (dm->vir == 0) {
		dev_unisoc_bt_err(ttyBT_dev,"dma_alloc_coherent err\n");
		return -1;
	}
	dm->size = size;
	memset((unsigned char *)(dm->vir), 0x56, size);
	dev_unisoc_bt_info(ttyBT_dev,"dma_alloc_coherent(%d) 0x%lx 0x%lx\n",
		  size, dm->vir, dm->phy);

	return 0;
}

int mtty_dma_buf_alloc(int chn, int size, int num)
{
	int ret, i;
	struct dma_buf temp = {0};
	struct mbuf_t *mbuf = NULL;
	struct mbuf_t *head = NULL;
	struct mbuf_t *tail = NULL;
	dm_rx_t = &mtty_dev->pdev->dev;

	if (!dm_rx_t) {
		dev_unisoc_bt_err(ttyBT_dev,"%s:PCIE device link error\n", __func__);
		return -1;
	}
	ret = sprdwcn_bus_list_alloc(chn, &head, &tail, &num);
	if (ret != 0)
		return -1;
	for (i = 0, mbuf = head; i < num; i++, mbuf = mbuf->next) {
		ret = mtty_dmalloc(dm_rx_t, &temp, size);
		if (ret != 0) {
			/* Rollback: free already allocated DMA buffers */
			int j;
			for (j = 0; j < i; j++) {
				if (dm_rx_ptr[j]) {
					dma_free_coherent(dm_rx_t, size,
							  (void *)dm_rx_ptr[j],
							  dm_rx_phy[j]);
					dm_rx_ptr[j] = NULL;
					dm_rx_phy[j] = 0;
				}
			}
			sprdwcn_bus_list_free(chn, head, tail, i);
			return -ENOMEM;
		}
		mbuf->buf = (unsigned char *)(temp.vir);
        dm_rx_ptr[i] = mbuf->buf;
		mbuf->phy = (unsigned long)(temp.phy);
        dm_rx_phy[i] = mbuf->phy;
		mbuf->len = temp.size;
		memset(mbuf->buf, 0x0, mbuf->len);
	}

	ret = sprdwcn_bus_push_list(chn, head, tail, num);

	return ret;
}

int mtty_dma_buf_free(int num) {
    int loop_count = 0;
    for (; loop_count < num; loop_count++) {
        if (!dm_rx_t || !dm_rx_ptr[loop_count]) {
            dev_unisoc_bt_err(ttyBT_dev, "%s: dm_rx_t or dm_rx_ptr[%d] is NULL, skip\n",
                              __func__, loop_count);
            continue;
        }
        dma_free_coherent(dm_rx_t, BT_PCIE_RX_DMA_SIZE, (void *)dm_rx_ptr[loop_count], dm_rx_phy[loop_count]);
        dev_unisoc_bt_info(ttyBT_dev, "%s: free dm_rx_ptr[%d] success\n", __func__, loop_count);
        dm_rx_ptr[loop_count] = NULL;
        dm_rx_phy[loop_count] = 0;
    }
    return 0;
}

/* static void mtty_rx_task(unsigned long data) */
static void mtty_rx_work_queue(struct work_struct *work)

{
    int i, ret = 0;
    /*struct mtty_device *mtty = (struct mtty_device *)data;*/
    struct mtty_device *mtty;
    struct rx_data *rx = NULL;

    que_task = que_task + 1;
    if (que_task > 65530)
        que_task = 0;
    dev_unisoc_bt_info(ttyBT_dev,
                       "mtty que_task= %d\n",
                       que_task);
    que_sche = que_sche - 1;

    mtty = container_of(work, struct mtty_device, bt_rx_work);
    if (unlikely(!mtty)) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty_rx_task mtty is NULL\n");
        return;
    }

    if (atomic_read(&mtty->state) == MTTY_STATE_OPEN) {
        do {
            mutex_lock(&mtty->rw_mutex);
            if (list_empty_careful(&mtty->rx_head)) {
                dev_unisoc_bt_err(ttyBT_dev,
                                  "mtty over load queue done\n");
                mutex_unlock(&mtty->rw_mutex);
                break;
            }
            rx = list_first_entry_or_null(&mtty->rx_head,
                        struct rx_data, entry);
            if (!rx) {
                dev_unisoc_bt_err(ttyBT_dev,
                                  "mtty over load queue abort\n");
                mutex_unlock(&mtty->rw_mutex);
                break;
            }
            list_del(&rx->entry);
            mutex_unlock(&mtty->rw_mutex);

            dev_unisoc_bt_err(ttyBT_dev,
                              "mtty over load working at channel: %d, len: %d\n",
                              rx->channel, rx->head->len);
            for (i = 0; i < rx->head->len; i++) {
                int retry_count = 0;
                while (retry_count < 100) {
                    ret = tty_insert_flip_char(mtty->port,
                                *(rx->head->buf+i), TTY_NORMAL);
                    if (ret == 1) {
                        break;
                    } else if (ret == 0) {
                        retry_count++;
                        if (retry_count >= 100) {
                            dev_unisoc_bt_err(ttyBT_dev,
                                              "mtty over load tty buffer full, dropping data\n");
                            break;
                        }
                        msleep(1);
                    }
                }
                if (ret == 1) {
                    tty_flip_buffer_push(mtty->port);
                }
            }
            dev_unisoc_bt_err(ttyBT_dev,
                              "mtty over load cut channel: %d\n",
                              rx->channel);
            kfree(rx->head->buf);
            kfree(rx);

        } while (1);
    } else {
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty status isn't open, status:%d\n",
                           atomic_read(&mtty->state));
    }
}

static int mtty_sdio_rx_cb(int chn, struct mbuf_t *head, struct mbuf_t *tail, int num)
{
    int ret = 0, block_size;
    struct rx_data *rx;

    bt_wakeup_host();
    block_size = ((head->buf[2] & 0x7F) << 9) + (head->buf[1] << 1) + (head->buf[0] >> 7);

    /*{
        dev_unisoc_bt_info(ttyBT_dev,
                           "%s dump head: %d, channel: %d, num: %d\n",
                           __func__, BT_SDIO_HEAD_LEN, chn, num);
        hex_dump_block((unsigned char *)head->buf, BT_SDIO_HEAD_LEN);
        dev_unisoc_bt_info(ttyBT_dev,
                           "%s dump block %d\n",
                           __func__, block_size);
        hex_dump_block((unsigned char *)head->buf + BT_SDIO_HEAD_LEN, block_size);
    }*/

    if (atomic_read(&mtty_dev->state) == MTTY_STATE_CLOSE) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s mtty bt is closed abnormally\n",
                          __func__);
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -1;
    }
        if(is_user_debug){
            bt_host_data_save((unsigned char *)head->buf + BT_SDIO_HEAD_LEN, block_size, BT_DATA_IN);
    }

    if (mtty_dev != NULL) {
        if (!work_pending(&mtty_dev->bt_rx_work)) {
            dev_unisoc_bt_dbg(ttyBT_dev,
                              "%s tty_insert_flip_string",
                              __func__);
            ret = tty_insert_flip_string(mtty_dev->port,
                            (unsigned char *)head->buf + BT_SDIO_HEAD_LEN,
                            block_size);   // -BT_SDIO_HEAD_LEN
            dev_unisoc_bt_dbg(ttyBT_dev,
                              "%s ret: %d, len: %d\n",
                              __func__, ret, block_size);
            if (ret)
                tty_flip_buffer_push(mtty_dev->port);

            /* 直接调用 hci_recv_frame() 将 HCI 数据传递给协议栈 */
            if (mtty_dev->hdev) {
                struct sk_buff *skb;
                unsigned char *hci_data;
                skb = bt_skb_alloc(block_size, GFP_ATOMIC);
                if (skb) {
                    hci_data = (unsigned char *)head->buf + BT_SDIO_HEAD_LEN;
                    skb_put_data(skb, hci_data, block_size);
                    hci_skb_pkt_type(skb) = hci_data[0];
                    ret = hci_recv_frame(mtty_dev->hdev, skb);
                    if (ret < 0)
                        dev_unisoc_bt_err(ttyBT_dev,
                                          "%s hci_recv_frame failed: %d\n",
                                          __func__, ret);
                } else {
                    dev_unisoc_bt_err(ttyBT_dev,
                                      "%s bt_skb_alloc failed\n",
                                      __func__);
                }
            }

            if (ret == (block_size)) {
                dev_unisoc_bt_dbg(ttyBT_dev,
                                  "%s send success",
                                  __func__);
                sprdwcn_bus_push_list(chn, head, tail, num);
                return 0;
            }
        }

        rx = kmalloc(sizeof(struct rx_data), GFP_KERNEL);
        if (rx == NULL) {
            dev_unisoc_bt_err(ttyBT_dev,
                              "%s rx == NULL\n",
                              __func__);
            sprdwcn_bus_push_list(chn, head, tail, num);
            return -ENOMEM;
        }

        rx->head = head;
        rx->tail = tail;
        rx->channel = chn;
        rx->num = num;
        rx->head->len = (block_size) - ret;
        rx->head->buf = kmalloc(rx->head->len, GFP_KERNEL);
        if (rx->head->buf == NULL) {
            dev_unisoc_bt_err(ttyBT_dev,
                              "mtty low memory!\n");
            kfree(rx);
            sprdwcn_bus_push_list(chn, head, tail, num);
            return -ENOMEM;
        }

        memcpy(rx->head->buf, (unsigned char *)head->buf + BT_SDIO_HEAD_LEN + ret, rx->head->len);
        sprdwcn_bus_push_list(chn, head, tail, num);
        mutex_lock(&mtty_dev->rw_mutex);
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty over load push %d -> %d, channel: %d len: %d\n",
                          block_size, ret, rx->channel, rx->head->len);
        list_add_tail(&rx->entry, &mtty_dev->rx_head);
        mutex_unlock(&mtty_dev->rw_mutex);
        if (!work_pending(&mtty_dev->bt_rx_work)) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "work_pending\n");
            queue_work(mtty_dev->bt_rx_workqueue,
                        &mtty_dev->bt_rx_work);
        }
        return 0;
    }
    dev_unisoc_bt_err(ttyBT_dev,
                      "mtty_rx_cb mtty_dev is NULL!!!\n");

    return -1;
}

static int mtty_pcie_rx_cb(int chn, struct mbuf_t *head, struct mbuf_t *tail, int num)
{
    int ret = 0, len_send;
    struct rx_data *rx;
    unsigned char *sdio_buf = NULL;
    sdio_buf = (unsigned char *)head->buf;
    bt_wakeup_host();

    len_send = head->len;

	dev_unisoc_bt_dbg(ttyBT_dev,"%s() channel: %d, num: %d\n", __func__, chn, num);
	dev_unisoc_bt_dbg(ttyBT_dev,"%s() ---mtty receive channel= %d, len_send = %d\n", __func__, chn, len_send);

    if (atomic_read(&mtty_dev->state) == MTTY_STATE_CLOSE) {
		dev_unisoc_bt_err(ttyBT_dev,"%s() mtty bt is closed abnormally\n", __func__);
        sprdwcn_bus_push_list(chn, head, tail, num);
        return -1;
    }

    if (mtty_dev != NULL) {
        if (!work_pending(&mtty_dev->bt_rx_work)) {
			dev_unisoc_bt_dbg(ttyBT_dev,"%s() tty_insert_flip_string", __func__);
            ret = tty_insert_flip_string(mtty_dev->port,
                            (unsigned char *)head->buf+BT_PCIE_HEAD_LEN,
                            len_send);   // -BT_PCIE_HEAD_LEN
			dev_unisoc_bt_dbg(ttyBT_dev,"%s() ret=%d, len=%d\n", __func__, ret, len_send);
            if (ret)
                tty_flip_buffer_push(mtty_dev->port);

            /* 直接调用 hci_recv_frame() 将 HCI 数据传递给协议栈 */
            if (mtty_dev->hdev) {
                struct sk_buff *skb;
                unsigned char *hci_data;
                skb = bt_skb_alloc(len_send, GFP_ATOMIC);
                if (skb) {
                    hci_data = (unsigned char *)head->buf + BT_PCIE_HEAD_LEN;
                    skb_put_data(skb, hci_data, len_send);
                    hci_skb_pkt_type(skb) = hci_data[0];
                    ret = hci_recv_frame(mtty_dev->hdev, skb);
                    if (ret < 0)
                        dev_unisoc_bt_err(ttyBT_dev,
                                          "%s() hci_recv_frame failed: %d\n",
                                          __func__, ret);
                } else {
                    dev_unisoc_bt_err(ttyBT_dev,
                                      "%s() bt_skb_alloc failed\n",
                                      __func__);
                }
            }

            if (ret == (len_send)) {
				dev_unisoc_bt_dbg(ttyBT_dev,"%s() send success", __func__);
                sprdwcn_bus_push_list(chn, head, tail, num);
                return 0;
            }
        }

        rx = kmalloc(sizeof(struct rx_data), GFP_KERNEL);
        if (rx == NULL) {
			dev_unisoc_bt_err(ttyBT_dev,"%s() rx == NULL\n", __func__);
            sprdwcn_bus_push_list(chn, head, tail, num);
            return -ENOMEM;
        }

        rx->head = head;
        rx->tail = tail;
        rx->channel = chn;
        rx->num = num;
        rx->head->len = (len_send) - ret;
        rx->head->buf = kmalloc(rx->head->len, GFP_KERNEL);
        if (rx->head->buf == NULL) {
			dev_unisoc_bt_err(ttyBT_dev,"mtty low memory!\n");
            kfree(rx);
            sprdwcn_bus_push_list(chn, head, tail, num);
            return -ENOMEM;
        }

        memcpy(rx->head->buf, head->buf + BT_PCIE_HEAD_LEN + ret, rx->head->len);
        sprdwcn_bus_push_list(chn, head, tail, num);
        mutex_lock(&mtty_dev->rw_mutex);
		dev_unisoc_bt_dbg(ttyBT_dev,"mtty over load push %d -> %d, channel: %d len: %d\n",
                len_send, ret, rx->channel, rx->head->len);
        list_add_tail(&rx->entry, &mtty_dev->rx_head);
        mutex_unlock(&mtty_dev->rw_mutex);
        if (!work_pending(&mtty_dev->bt_rx_work)) {
			dev_unisoc_bt_dbg(ttyBT_dev,"work_pending\n");
            queue_work(mtty_dev->bt_rx_workqueue,
                        &mtty_dev->bt_rx_work);
        }
        return 0;
    }
	dev_unisoc_bt_err(ttyBT_dev,"mtty_rx_cb mtty_dev is NULL!!!\n");

    return -1;
}

static int mtty_sdio_tx_cb(int chn, struct mbuf_t *head, struct mbuf_t *tail, int num)
{
    int i;
    struct mbuf_t *pos = NULL;
    dev_unisoc_bt_dbg(ttyBT_dev,
                      "%s channel: %d, head: %p, tail: %p num: %d\n",
                      __func__, chn, head, tail, num);
    pos = head;
    for (i = 0; i < num; i++, pos = pos->next) {
        kfree(pos->buf);
        pos->buf = NULL;
    }
    if ((sprdwcn_bus_list_free(chn, head, tail, num)) == 0)
    {
		dev_unisoc_bt_dbg(ttyBT_dev,"%s sprdwcn_bus_list_free() success\n", __func__);
        up(&sem_id);
    }
    else
		dev_unisoc_bt_err(ttyBT_dev,"%s sprdwcn_bus_list_free() fail\n", __func__);

    return 0;
}

static int mtty_pcie_tx_cb(int chn, struct mbuf_t *head, struct mbuf_t *tail, int num)
{
    int i;
    struct mbuf_t *pos = NULL;
	dev_unisoc_bt_dbg(ttyBT_dev,"%s channel: %d, head: %p, tail: %p num: %d mtty_dev %p pdev %p\n", 
			__func__, chn, head, tail, num, mtty_dev, mtty_dev->pdev);
    pos = head;
    for (i = 0; i < num; i++, pos = pos->next) {
        struct device *dm = &mtty_dev->pdev->dev;
        dma_free_coherent(dm, pos->len, (void *)pos->buf, head->phy);
        pos->buf = NULL;
    }
    if ((sprdwcn_bus_list_free(chn, head, tail, num)) == 0)
    {
        dev_unisoc_bt_dbg(ttyBT_dev,
                          "%s sprdwcn_bus_list_free() success\n",
                          __func__);
        up(&sem_id);
    }
    else
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s sprdwcn_bus_list_free() fail\n",
                          __func__);

    return 0;
}

/* HCI callback: open device */
static int mtty_hci_open(struct hci_dev *hdev)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);

    BT_DBG("%s", hdev->name);

    if (!mtty)
        return -ENODEV;

    atomic_set(&mtty->state, MTTY_STATE_OPEN);
    return 0;
}

/* HCI callback: close device */
static int mtty_hci_close(struct hci_dev *hdev)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);

    BT_DBG("%s", hdev->name);

    if (!mtty)
        return -ENODEV;

    atomic_set(&mtty->state, MTTY_STATE_CLOSE);
    return 0;
}

/* HCI callback: flush device */
static int mtty_hci_flush(struct hci_dev *hdev)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);
    struct tty_struct *tty;

    BT_DBG("%s", hdev->name);

    if (!mtty)
        return 0;

    mutex_lock(&mtty->rw_mutex);
    tty = mtty->tty;
    if (tty && atomic_read(&mtty->state) == MTTY_STATE_OPEN &&
        tty->ops && tty->ops->flush_buffer) {
        tty->ops->flush_buffer(tty);
    }
    mutex_unlock(&mtty->rw_mutex);
    return 0;
}

/* HCI callback: send frame */
static int mtty_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);
    unsigned char *ptr;
    int ret;

    BT_DBG("%s: type %d len %d", hdev->name, hci_skb_pkt_type(skb), skb->len);

    if (!mtty)
        return -ENODEV;

    mutex_lock(&mtty->rw_mutex);
    if (!mtty->tty ||
        atomic_read(&mtty->state) != MTTY_STATE_OPEN) {
        mutex_unlock(&mtty->rw_mutex);
        return -ENODEV;
    }
    ptr = skb->data;
    ret = mtty->tty->ops->write(mtty->tty, ptr, skb->len);
    mutex_unlock(&mtty->rw_mutex);

    /* Check return value BEFORE freeing skb to avoid use-after-free */
    if (ret < 0) {
        kfree_skb(skb);
        return ret;
    }
    if (ret < skb->len) {
        kfree_skb(skb);
        return -EIO;
    }
    kfree_skb(skb);
    return 0;
}

/* HCI callback: setup device */
static int mtty_hci_setup(struct hci_dev *hdev)
{
    struct mtty_device *mtty = hci_get_drvdata(hdev);

    BT_DBG("%s", hdev->name);

    if (!mtty)
        return -ENODEV;

    mutex_lock(&mtty->rw_mutex);
    if (!mtty->tty ||
        atomic_read(&mtty->state) != MTTY_STATE_OPEN) {
        mutex_unlock(&mtty->rw_mutex);
        return -ENODEV;
    }

    /* Send HCI reset command */
    /* HCI Reset: opcode 0x0c03, parameter length 0 */
    if (mtty->tty->ops && mtty->tty->ops->write) {
        unsigned char reset_cmd[] = {
            0x01,       /* HCI packet type: command */
            0x03, 0x0c, /* HCI opcode: HCI_OP_RESET (0x0c03) - little endian */
            0x00,       /* Parameter total length */
        };
        int ret = mtty->tty->ops->write(mtty->tty, reset_cmd, sizeof(reset_cmd));
        mutex_unlock(&mtty->rw_mutex);
        if (ret < 0)
            return ret;
        if (ret < (int)sizeof(reset_cmd))
            return -EIO;
        return 0;
    }
    mutex_unlock(&mtty->rw_mutex);
    return 0;
}

static int mtty_open(struct tty_struct *tty, struct file *filp)
{
    struct mtty_device *mtty = NULL;
    struct tty_driver *driver = NULL;

    if (tty == NULL) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty open input tty is NULL!\n");
        return -ENOMEM;
    }
    driver = tty->driver;
    mtty = (struct mtty_device *)driver->driver_state;

    if (mtty == NULL) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty open input mtty NULL!\n");
        return -ENOMEM;
    }

    /* Free old data_dump if exists */
    if (data_dump != NULL) {
        vfree(data_dump);
        data_dump = NULL;
    }
    data_dump = (bt_host_data_dump*) vmalloc(sizeof(bt_host_data_dump));
    if (data_dump == NULL) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty open failed to allocate data_dump\n");
        return -ENOMEM;
    }
    memset(data_dump, 0, sizeof(bt_host_data_dump));

    mtty->tty = tty;
    tty->driver_data = (void *)mtty;

    atomic_set(&mtty->state, MTTY_STATE_OPEN);
    que_task = 0;
    que_sche = 0;
    sitm_ini();
	if (wcn_hw_type == HW_TYPE_PCIE) {
		sprdwcn_bus_chn_init(&bt_pcie_rx_ops);
		sprdwcn_bus_chn_init(&bt_pcie_tx_ops);
		if (mtty_dma_buf_alloc(BT_PCIE_RX_CHANNEL, BT_PCIE_RX_DMA_SIZE, BT_PCIE_RX_MAX_NUM) < 0) {
			dev_unisoc_bt_err(ttyBT_dev,
			                  "%s: DMA buffer allocation failed\n",
			                  __func__);
			sprdwcn_bus_chn_deinit(&bt_pcie_rx_ops);
			sprdwcn_bus_chn_deinit(&bt_pcie_tx_ops);
			return -ENOMEM;
		}
	}
    dev_unisoc_bt_info(ttyBT_dev,
                       "mtty_open device success!\n");

    return 0;
}

static void mtty_close(struct tty_struct *tty, struct file *filp)
{
    struct mtty_device *mtty = NULL;

    if (tty == NULL) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty close input tty is NULL!\n");
        return;
    }
    mtty = (struct mtty_device *) tty->driver_data;
    if (mtty == NULL) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty close s tty is NULL!\n");
        return;
    }

	if (wcn_hw_type == HW_TYPE_PCIE) {
		mtty_dma_buf_free(BT_PCIE_RX_MAX_NUM);
		sprdwcn_bus_chn_deinit(&bt_pcie_rx_ops);
		sprdwcn_bus_chn_deinit(&bt_pcie_tx_ops);
	}

    atomic_set(&mtty->state, MTTY_STATE_CLOSE);
    mtty->tty = NULL;  /* Prevent HCI callbacks from accessing freed tty */
    sitm_cleanup();

    if (data_dump != NULL) {
        vfree(data_dump);
        data_dump = NULL;
    }
    dev_unisoc_bt_info(ttyBT_dev,
                       "mtty_close device success !\n");
}

static int mtty_sdio_write(struct tty_struct *tty,
            const unsigned char *buf, int count)
{
    int num = 1, ret;
    struct mbuf_t *tx_head = NULL, *tx_tail = NULL;
    unsigned char *block = NULL;

    dev_unisoc_bt_dbg(ttyBT_dev,
                      "%s +++\n",
                      __func__);
    if (is_user_debug) {
        bt_host_data_save(buf, count, BT_DATA_OUT);
    }
    /*{
        dev_unisoc_bt_info(ttyBT_dev,
                           "%s dump size: %d\n",
                           __func__, count);
        hex_dump_block((unsigned char *)buf, count);
    }*/

    block = kmalloc(count + BT_SDIO_HEAD_LEN, GFP_KERNEL);

    if (!block) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s kmalloc failed\n",
                          __func__);
        return -ENOMEM;
    }
    memset(block, 0, count + BT_SDIO_HEAD_LEN);
    memcpy(block + BT_SDIO_HEAD_LEN, buf, count);
    down(&sem_id);
    ret = sprdwcn_bus_list_alloc(BT_SDIO_TX_CHANNEL, &tx_head, &tx_tail, &num);
	if (ret) {
		dev_unisoc_bt_err(ttyBT_dev,
							"%s sprdwcn_bus_list_alloc failed: %d\n",
							__func__, ret);
		up(&sem_id);
		kfree(block);
		block = NULL;
		return -ENOMEM;
	}
	    tx_head->buf = block;
	    tx_head->len = count + BT_SDIO_HEAD_LEN;
	    tx_head->next = NULL;
	    /* Fill SDIO header: channel 3 (BT_TX_INOUT), type 0 */
	    block[0] = (3 << 1) | 0;  /* channel 3, bit 0 = 0 */
	    block[1] = 0;
	    block[2] = 0;
	    block[3] = 0;
	ret = sprdwcn_bus_push_list(BT_SDIO_TX_CHANNEL, tx_head, tx_tail, num);
	if (ret) {
		dev_unisoc_bt_err(ttyBT_dev,
							"%s sprdwcn_bus_push_list failed: %d\n",
							__func__, ret);
		kfree(tx_head->buf);
		tx_head->buf = NULL;
		sprdwcn_bus_list_free(BT_SDIO_TX_CHANNEL, tx_head, tx_tail, num);
        up(&sem_id);
		return -EBUSY;
	}

	dev_unisoc_bt_dbg(ttyBT_dev,
						"%s ---\n",
						__func__);
	return count;
}

static int mtty_pcie_write(struct tty_struct *tty,
            const unsigned char *buf, int count)
{
	int num = 1;
	struct mbuf_t *tx_head = NULL;
	struct mbuf_t *tx_tail = NULL;

	down(&sem_id);
	if (!sprdwcn_bus_list_alloc(BT_PCIE_TX_CHANNEL, &tx_head, &tx_tail, &num)) {
		int ret = 0;
		struct device *dm = &mtty_dev->pdev->dev;
		dev_unisoc_bt_dbg(ttyBT_dev,"%s() sprdwcn_bus_list_alloc() success tx_head %p tx_tail %p num %d mtty_dev->tty->dev %p\n", 
				__func__, tx_head, tx_tail, num, mtty_dev->tty->dev);
		if ((ret = dma_set_mask(dm, DMA_BIT_MASK(64)))) {
			dev_unisoc_bt_err(ttyBT_dev,"dma_set_mask err ret %d\n", ret);
			if ((ret = dma_set_coherent_mask(dm, DMA_BIT_MASK(64)))) {
				dev_unisoc_bt_err(ttyBT_dev,"dma_set_coherent_mask err ret %d\n", ret);
				sprdwcn_bus_list_free(BT_PCIE_TX_CHANNEL, tx_head, tx_tail, num);
				up(&sem_id);
				return -ENOMEM;
			}
		}
		tx_head->buf = (unsigned char *)dma_alloc_coherent(dm, count, (dma_addr_t *)(&(tx_head->phy)), GFP_DMA);

		if(!tx_head->buf)
		{
			dev_unisoc_bt_err(ttyBT_dev,"%s:line:%d dma_alloc_coherent err dev %p count %d phy %p\n",
					__func__, __LINE__, mtty_dev->tty->dev, count, &(tx_head->phy));
			sprdwcn_bus_list_free(BT_PCIE_TX_CHANNEL, tx_head, tx_tail, num);
			up(&sem_id);
			return -ENOMEM;
		}
		memcpy(tx_head->buf, buf, count);
		tx_head->len = count;
		tx_head->next = NULL;
		/*packer type 0, subtype 0*/
		dev_unisoc_bt_dbg(ttyBT_dev,"%s sprdwcn_bus_push_list num: %d ++\n", __func__, num);
		ret = sprdwcn_bus_push_list(BT_PCIE_TX_CHANNEL, tx_head, tx_tail, num);
		dev_unisoc_bt_dbg(ttyBT_dev,"%s sprdwcn_bus_push_list ret: %d --\n", __func__, ret);
		if (ret)
		{
			dma_free_coherent(dm, count, (void *)tx_head->buf, tx_head->phy);
			tx_head->buf = NULL;
			sprdwcn_bus_list_free(BT_PCIE_TX_CHANNEL, tx_head, tx_tail, num);
            up(&sem_id);
			return -EBUSY;
		}
		else
		{
			dev_unisoc_bt_dbg(ttyBT_dev,"%s() sprdwcn_bus_push_list() success\n", __func__);
			return count;
		}
	} else {
		dev_unisoc_bt_err(ttyBT_dev,"%s:%d sprdwcn_bus_list_alloc fail\n", __func__, __LINE__);
		up(&sem_id);
		return -ENOMEM;
	}
}

static int mtty_write(struct tty_struct *tty,
            const unsigned char *buf, int count)
{
	int mcount;
	dev_unisoc_bt_dbg(ttyBT_dev,"%s +++\n", __func__);

	if (wcn_hw_type == HW_TYPE_SDIO) {
		mcount = mtty_sdio_write(tty,buf,count);
		return mcount;
	} else if (wcn_hw_type == HW_TYPE_PCIE) {
		mcount = mtty_pcie_write(tty,buf,count);
		return mcount;
	} else {
		dev_unisoc_bt_err(ttyBT_dev,"%s invalid hw type\n", __func__);
		return -ENOMEM;
	}
}

static  int sdio_data_transmit(uint8_t *data, size_t count)
{
	return mtty_write(NULL, data, count);
}


static int mtty_write_plus(struct tty_struct *tty,
	      const unsigned char *buf, int count)
{
	return sitm_write(buf, count, sdio_data_transmit);
}


static void mtty_flush_chars(struct tty_struct *tty)
{
}

static int mtty_write_room(struct tty_struct *tty)
{
	return INT_MAX;
}

static int mtty_ioctl(struct tty_struct *tty,
                      unsigned int cmd, unsigned long arg)
{
    struct mtty_device *mtty = tty->driver_data;
    int ret = -ENOIOCTLCMD;

    if (!mtty)
        return -EBADF;

    switch (cmd) {
    case HCIUARTSETPROTO:
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty_ioctl: HCIUARTSETPROTO=%d\n", (int)arg);
        /* Store the protocol for later use */
        mtty->hci_proto = (int)arg;
        ret = 0;
        break;

    case HCIUARTGETPROTO:
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty_ioctl: HCIUARTGETPROTO\n");
        ret = mtty->hci_proto;
        break;

    case HCIUARTGETDEVICE:
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty_ioctl: HCIUARTGETDEVICE\n");
        ret = 0;  /* Return hci0 */
        break;

    case HCIUARTSETFLAGS:
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty_ioctl: HCIUARTSETFLAGS\n");
        ret = 0;
        break;

    case HCIUARTGETFLAGS:
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty_ioctl: HCIUARTGETFLAGS\n");
        ret = 0;
        break;

    default:
        dev_unisoc_bt_info(ttyBT_dev,
                           "mtty_ioctl: unknown cmd=0x%x\n", cmd);
        break;
    }

    return ret;
}

static const struct tty_operations mtty_ops = {
    .open  = mtty_open,
    .close = mtty_close,
    .write = mtty_write_plus,
    .flush_chars = mtty_flush_chars,
    .write_room  = mtty_write_room,
    .ioctl = mtty_ioctl,
};

static struct tty_port *mtty_port_init(void)
{
    struct tty_port *port = NULL;

    port = kzalloc(sizeof(struct tty_port), GFP_KERNEL);
    if (port == NULL)
        return NULL;
    tty_port_init(port);

    return port;
}

static int mtty_tty_driver_init(struct mtty_device *device)
{
    struct tty_driver *driver;
    int ret = 0;

    device->port = mtty_port_init();
    if (!device->port)
        return -ENOMEM;

    driver = alloc_tty_driver(MTTY_DEV_MAX_NR);
    if (!driver)
        return -ENOMEM;

    /*
    * Initialize the tty_driver structure
    * Entries in mtty_driver that are NOT initialized:
    * proc_entry, set_termios, flush_buffer, set_ldisc, write_proc
    */
    driver->owner = THIS_MODULE;
    driver->driver_name = device->pdata->name;
    driver->name = device->pdata->name;
    driver->major = 0;
    driver->type = TTY_DRIVER_TYPE_SYSTEM;
    driver->subtype = SYSTEM_TYPE_TTY;
    driver->init_termios = tty_std_termios;
    driver->driver_state = (void *)device;
    device->driver = driver;
    device->driver->flags = TTY_DRIVER_REAL_RAW;
    /* initialize the tty driver */
    tty_set_operations(driver, &mtty_ops);
    tty_port_link_device(device->port, driver, 0);
    ret = tty_register_driver(driver);
    if (ret) {
        put_tty_driver(driver);
        tty_port_destroy(device->port);
        return ret;
    }
    return ret;
}

static void mtty_tty_driver_exit(struct mtty_device *device)
{
    struct tty_driver *driver = device->driver;

    tty_unregister_driver(driver);
    put_tty_driver(driver);
    tty_port_destroy(device->port);
}

static int mtty_parse_dt(struct mtty_init_data **init, struct device *dev)
{
#ifdef CONFIG_OF
    struct device_node *np = dev->of_node;
    struct mtty_init_data *pdata = NULL;
    int ret;

    pdata = kzalloc(sizeof(struct mtty_init_data), GFP_KERNEL);
    if (!pdata)
        return -ENOMEM;

    ret = of_property_read_string(np,
                    "sprd,name",
                    (const char **)&pdata->name);
    if (ret)
        goto error;
    *init = pdata;

    return 0;
error:
    kfree(pdata);
    *init = NULL;
    return ret;
#else
    return -ENODEV;
#endif
}

static inline void mtty_destroy_pdata(struct mtty_init_data **init)
{
    struct mtty_init_data *pdata = *init;

    kfree(pdata);
    *init = NULL;
}

struct mchn_ops_t bt_sdio_rx_ops = {
    .channel = BT_SDIO_RX_CHANNEL,
    .inout = BT_RX_INOUT,
    .pool_size = BT_RX_POOL_SIZE,
    .pop_link = mtty_sdio_rx_cb,
};

struct mchn_ops_t bt_sdio_tx_ops = {
    .channel = BT_SDIO_TX_CHANNEL,
    .inout = BT_TX_INOUT,
    .pool_size = BT_TX_POOL_SIZE,
    .pop_link = mtty_sdio_tx_cb,
};

struct mchn_ops_t bt_pcie_rx_ops = {
    .channel = BT_PCIE_RX_CHANNEL,
    .inout = BT_RX_INOUT,
    .pool_size = BT_RX_POOL_SIZE,
    .pop_link = mtty_pcie_rx_cb,
};

struct mchn_ops_t bt_pcie_tx_ops = {
    .channel = BT_PCIE_TX_CHANNEL,
    .inout = BT_TX_INOUT,
    .pool_size = BT_TX_POOL_SIZE,
    .pop_link = mtty_pcie_tx_cb,
};

static int bluetooth_reset(struct notifier_block *this, unsigned long ev, void *ptr)
{
#define RESET_BUFSIZE 5

    int ret = 0;
    int block_size = RESET_BUFSIZE;
	unsigned char reset_buf[RESET_BUFSIZE]= {0x04, 0xff, 0x02, 0x57, 0xa5};
    int reset_retry;

	dev_unisoc_bt_info(ttyBT_dev,"%s: reset callback coming\n", __func__);
	if (mtty_dev != NULL) {
		if (!work_pending(&mtty_dev->bt_rx_work)) {

			dev_unisoc_bt_info(ttyBT_dev,"%s tty_insert_flip_string", __func__);

			for (reset_retry = 0; reset_retry < 100 && ret < block_size; reset_retry++) {
				dev_unisoc_bt_info(ttyBT_dev,"%s before tty_insert_flip_string ret: %d, len: %d\n",
						__func__, ret, RESET_BUFSIZE);
				ret = tty_insert_flip_string(mtty_dev->port,
									(unsigned char *)reset_buf,
									RESET_BUFSIZE);   // -BT_SDIO_HEAD_LEN
				dev_unisoc_bt_info(ttyBT_dev,"%s ret: %d, len: %d\n", __func__, ret, RESET_BUFSIZE);
				if (ret)
					tty_flip_buffer_push(mtty_dev->port);
				block_size = block_size - ret;
				ret = 0;
			}
			if (reset_retry >= 100) {
				dev_unisoc_bt_err(ttyBT_dev,
								  "%s: reset buffer full, dropping reset data\n",
								  __func__);
			}
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block bluetooth_reset_block = {
	.notifier_call = bluetooth_reset,
};

static int  mtty_probe(struct platform_device *pdev)
{
    struct mtty_init_data *pdata = (struct mtty_init_data *)
                                pdev->dev.platform_data;
    struct mtty_device *mtty;
    int rval = 0;

    if (pdev->dev.of_node && !pdata) {
        rval = mtty_parse_dt(&pdata, &pdev->dev);
        if (rval) {
            dev_unisoc_bt_err(ttyBT_dev,
                              "failed to parse mtty device tree, ret=%d\n",
                              rval);
            return rval;
        }
    }

    /* Default name if pdata is NULL */
    if (!pdata) {
        pdata = kzalloc(sizeof(struct mtty_init_data), GFP_KERNEL);
        if (!pdata)
            return -ENOMEM;
        pdata->name = "ttyBT";
    }

    mtty = kzalloc(sizeof(struct mtty_device), GFP_KERNEL);
    ttyBT_dev = &pdev->dev;
    if (mtty == NULL) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "mtty Failed to allocate device!\n");
        return -ENOMEM;
    }

    mtty->pdata = pdata;
    rval = mtty_tty_driver_init(mtty);
    if (rval) {
        kfree(mtty);
        mtty_destroy_pdata(&pdata);
        dev_unisoc_bt_err(ttyBT_dev,
                          "regitster notifier failed (%d)\n",
                          rval);
        return rval;
    }

    dev_unisoc_bt_info(ttyBT_dev,
                       "mtty_probe init device addr: 0x%p\n",
                       mtty);
    platform_set_drvdata(pdev, mtty);

    /*spin_lock_init(&mtty->rw_lock);*/
    atomic_set(&mtty->state, MTTY_STATE_CLOSE);
    mutex_init(&mtty->rw_mutex);
    INIT_LIST_HEAD(&mtty->rx_head);
    /*tasklet_init(&mtty->rx_task, mtty_rx_task, (unsigned long)mtty);*/
    mtty->bt_rx_workqueue =
        create_singlethread_workqueue("SPRDBT_RX_QUEUE");
    if (!mtty->bt_rx_workqueue) {
        mtty_tty_driver_exit(mtty);
        kfree(mtty);
        mtty_destroy_pdata(&pdata);
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s SPRDBT_RX_QUEUE create failed",
                          __func__);
        return -ENOMEM;
    }
    INIT_WORK(&mtty->bt_rx_work, mtty_rx_work_queue);

    mtty_dev = mtty;

    if (sysfs_create_group(&pdev->dev.kobj,
            &bluetooth_group)) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s failed to create bluetooth tty attributes.\n",
                          __func__);
    }

    rfkill_bluetooth_init(pdev);
    bluesleep_init();

    atomic_notifier_chain_register(&wcn_reset_notifier_list,&bluetooth_reset_block);

	wcn_hw_type = sprdwcn_bus_get_hwintf_type();
    dev_unisoc_bt_info(ttyBT_dev,"mtty_probe get hw type:%d\n", wcn_hw_type);
	if (wcn_hw_type == HW_TYPE_INVALIED) {
		dev_unisoc_bt_err(ttyBT_dev,"%s wcn invalid hw type", __func__);
		return -ENOMEM;
	} else if (wcn_hw_type == HW_TYPE_SDIO) {
        sprdwcn_bus_chn_init(&bt_sdio_rx_ops);
        sprdwcn_bus_chn_init(&bt_sdio_tx_ops);
	}

    sema_init(&sem_id, BT_TX_POOL_SIZE - 1);

    /* Create HCI device automatically */
    mtty->hdev = hci_alloc_dev();
    if (!mtty->hdev) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s: failed to allocate HCI device\n",
                          __func__);
        mtty_tty_driver_exit(mtty);
        kfree(mtty);
        mtty_destroy_pdata(&pdata);
        return -ENOMEM;
    }

    hci_set_drvdata(mtty->hdev, mtty);
    mtty->hdev->bus = HCI_UART;
    mtty->hdev->dev_type = HCI_PRIMARY;
    set_bit(HCI_QUIRK_INVALID_BDADDR, &mtty->hdev->quirks);
    mtty->hdev->open = mtty_hci_open;
    mtty->hdev->close = mtty_hci_close;
    mtty->hdev->flush = mtty_hci_flush;
    mtty->hdev->send = mtty_hci_send_frame;
    mtty->hdev->setup = mtty_hci_setup;
    SET_HCIDEV_DEV(mtty->hdev, &pdev->dev);

    if (hci_register_dev(mtty->hdev) < 0) {
        dev_unisoc_bt_err(ttyBT_dev,
                          "%s: failed to register HCI device\n",
                          __func__);
        hci_free_dev(mtty->hdev);
        mtty->hdev = NULL;
        if (mtty->bt_rx_workqueue) {
            destroy_workqueue(mtty->bt_rx_workqueue);
        }
        mtty_tty_driver_exit(mtty);
        kfree(mtty);
        mtty_destroy_pdata(&pdata);
        return -ENODEV;
    }

    dev_unisoc_bt_info(ttyBT_dev,
                       "%s: HCI device %s created successfully\n",
                       __func__, mtty->hdev->name);

    return 0;
}

static int  mtty_remove(struct platform_device *pdev)
{
    struct mtty_device *mtty = platform_get_drvdata(pdev);

    if (mtty && mtty->hdev) {
        hci_unregister_dev(mtty->hdev);
        hci_free_dev(mtty->hdev);
        mtty->hdev = NULL;
    }

    /* Flush and destroy workqueue BEFORE TTY driver exit to prevent race */
    if (mtty->bt_rx_workqueue) {
        flush_workqueue(mtty->bt_rx_workqueue);
        destroy_workqueue(mtty->bt_rx_workqueue);
    }

    mtty_tty_driver_exit(mtty);

    if (wcn_hw_type == HW_TYPE_SDIO) {
        sprdwcn_bus_chn_deinit(&bt_sdio_rx_ops);
        sprdwcn_bus_chn_deinit(&bt_sdio_tx_ops);
    } else if (wcn_hw_type == HW_TYPE_PCIE) {
        sprdwcn_bus_chn_deinit(&bt_pcie_rx_ops);
        sprdwcn_bus_chn_deinit(&bt_pcie_tx_ops);
    }

    rfkill_bluetooth_remove(pdev);
    mtty_destroy_pdata(&mtty->pdata);
    /*tasklet_kill(&mtty->rx_task);*/
    kfree(mtty);
    platform_set_drvdata(pdev, NULL);
    sysfs_remove_group(&pdev->dev.kobj, &bluetooth_group);
    bluesleep_exit();

    return 0;
}

static const struct of_device_id mtty_match_table[] = {
    { .compatible = "sprd,mtty", },
    { },
};

static struct platform_driver mtty_driver = {
    .driver = {
        .owner = THIS_MODULE,
        .name = "mtty",
        .of_match_table = mtty_match_table,
    },
    .probe = mtty_probe,
    .remove = mtty_remove,
};

module_platform_driver(mtty_driver);

MODULE_AUTHOR("Unisoc wcn bt");
MODULE_DESCRIPTION("Unisoc marlin tty driver");
