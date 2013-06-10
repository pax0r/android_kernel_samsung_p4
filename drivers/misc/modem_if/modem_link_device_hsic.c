/* /linux/drivers/new_modem_if/link_dev_usb.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/irq.h>
#include <linux/poll.h>
#include <linux/gpio.h>
#include <linux/if_arp.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/pm_runtime.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/wakelock.h>
#include <linux/suspend.h>
#include <linux/version.h>
#include <linux/ratelimit.h>

#include <linux/platform_data/modem.h>
#include "modem_prj.h"
#include "modem_link_device_hsic.h"
#include "modem_utils.h"

/*grzwolf-beg*/
   int g_timeout = 0;
/*grzwolf-end*/


static struct modem_ctl *if_usb_get_modemctl(struct link_pm_data *pm_data);
static int link_pm_runtime_get_active(struct link_pm_data *pm_data);
static int usb_tx_urb_with_skb(struct usb_device *usbdev, struct sk_buff *skb,
					struct if_usb_devdata *pipe_data);
#ifdef FOR_TEGRA
#define ehci_vendor_txfilltuning tegra_ehci_txfilltuning
#else
#define ehci_vendor_txfilltuning()
#endif
static void usb_rx_complete(struct urb *urb);

#if 1
static void usb_set_autosuspend_delay(struct usb_device *usbdev, int delay)
{
	pm_runtime_set_autosuspend_delay(&usbdev->dev, delay);
}
#else
static void usb_set_autosuspend_delay(struct usb_device *usbdev, int delay)
{
	usbdev->autosuspend_delay = msecs_to_jiffies(delay);
}
#endif

static int start_ipc(struct link_device *ld, struct io_device *iod)
{
	struct sk_buff *skb;
	char data[1] = {'a'};
	int err;
	struct usb_link_device *usb_ld = to_usb_link_device(ld);
	struct if_usb_devdata *pipe_data = &usb_ld->devdata[IF_USB_FMT_EP];

	if (!usb_ld->if_usb_connected) {
		mif_err("HSIC not connected, skip start ipc\n");
		err = -ENODEV;
		goto exit;
	}

	if (ld->mc->phone_state != STATE_ONLINE) {
		mif_err("[MODEM_IF] MODEM is not online, skip start ipc\n");
		err = -ENODEV;
		goto exit;
	}

	mif_err("send 'a'\n");

	skb = alloc_skb(16, GFP_ATOMIC);
	if (unlikely(!skb))
		return -ENOMEM;
	memcpy(skb_put(skb, 1), data, 1);
	skbpriv(skb)->iod = iod;
	skbpriv(skb)->ld = ld;

	if (!usb_ld->if_usb_connected || !usb_ld->usbdev)
		return -ENODEV;

	usb_mark_last_busy(usb_ld->usbdev);
	err = usb_tx_urb_with_skb(usb_ld->usbdev, skb, pipe_data);
	if (err < 0) {
		mif_err("usb_tx_urb fail\n");
		dev_kfree_skb_any(skb);
		goto exit;
	}
exit:
	return err;
}

static void stop_ipc(struct link_device *ld)
{
	ld->com_state = COM_NONE;
}

static int usb_init_communication(struct link_device *ld,
			struct io_device *iod)
{
#ifndef CONFIG_SLP
	struct task_struct *task = get_current();
	char str[TASK_COMM_LEN];

	mif_info("%d:%s\n", task->pid, get_task_comm(str, task));

#endif
	/* Send IPC Start ASCII 'a' */
	if (iod->id == 0x1)
		return start_ipc(ld, iod);

	return 0;
}

static void usb_terminate_communication(struct link_device *ld,
			struct io_device *iod)
{
	if (iod->id != 0x1 || iod->format != IPC_FMT)
		return;

	if (iod->mc->phone_state == STATE_CRASH_RESET ||
			iod->mc->phone_state == STATE_CRASH_EXIT)
		stop_ipc(ld);
}

static int usb_rx_submit(struct usb_link_device *usb_ld,
					struct if_usb_devdata *pipe_data,
					gfp_t gfp_flags)
{
	int ret;
	struct urb *urb;

	if (pipe_data->disconnected)
		return -ENOENT;

	ehci_vendor_txfilltuning();

	urb = pipe_data->urb;

	urb->transfer_flags = 0;
	usb_fill_bulk_urb(urb, pipe_data->usbdev,
				pipe_data->rx_pipe, pipe_data->rx_buf,
				pipe_data->rx_buf_size, usb_rx_complete,
				(void *)pipe_data);

	if (!usb_ld->if_usb_connected || !usb_ld->usbdev)
		return -ENOENT;

	usb_mark_last_busy(usb_ld->usbdev);
	ret = usb_submit_urb(urb, gfp_flags);
	if (ret)
		mif_err("submit urb fail with ret (%d)\n", ret);

	return ret;
}

static void usb_rx_retry_work(struct work_struct *work)
{
	int ret = 0;
	struct usb_link_device *usb_ld =
		container_of(work, struct usb_link_device, rx_retry_work.work);
	struct urb *urb = usb_ld->retry_urb;
	struct if_usb_devdata *pipe_data = urb->context;
	struct io_device *iod;
	int iod_format;

	if (!usb_ld->if_usb_connected || !usb_ld->usbdev)
		return;

	if (usb_ld->usbdev)
		usb_mark_last_busy(usb_ld->usbdev);
	switch (pipe_data->format) {
	case IF_USB_FMT_EP:
		if (usb_ld->if_usb_is_main) {
			pr_urb("IPC-RX, retry", urb);
			iod_format = IPC_FMT;
		} else {
			iod_format = IPC_BOOT;
		}
		break;
	case IF_USB_RAW_EP:
		iod_format = IPC_MULTI_RAW;
		break;
	case IF_USB_RFS_EP:
		iod_format = IPC_RFS;
		pr_urb("RFS-RX, retry", urb);
		break;
	case IF_USB_CMD_EP:
		iod_format = IPC_CMD;
		break;
	default:
		iod_format = -1;
		break;
	}

	iod = link_get_iod_with_format(&usb_ld->ld, iod_format);
	if (iod) {
		ret = iod->recv(iod, &usb_ld->ld, (char *)urb->transfer_buffer,
			urb->actual_length);
		if (ret == -ENOMEM) {
			/* TODO: check the retry count */
			/* retry the delay work after 20ms and resubit*/
			mif_err("ENOMEM, +retry 20ms\n");
			if (usb_ld->usbdev)
				usb_mark_last_busy(usb_ld->usbdev);
			usb_ld->retry_urb = urb;
			if (usb_ld->rx_retry_cnt++ < 10)
				queue_delayed_work(usb_ld->ld.tx_wq,
					&usb_ld->rx_retry_work,	10);
			return;
		}
		if (ret < 0)
			mif_err("io device recv error (%d)\n", ret);
		usb_ld->rx_retry_cnt = 0;
	}

	if (usb_ld->usbdev)
		usb_mark_last_busy(usb_ld->usbdev);
	usb_rx_submit(usb_ld, pipe_data, GFP_ATOMIC);
}


static void usb_rx_complete(struct urb *urb)
{
	struct if_usb_devdata *pipe_data = urb->context;
	struct usb_link_device *usb_ld = pipe_data->usb_ld;
	struct io_device *iod;
	int iod_format;
	int ret;

	if (usb_ld->usbdev)
		usb_mark_last_busy(usb_ld->usbdev);

	switch (urb->status) {
	case -ENOENT:
		/* case for 'link pm suspended but rx data had remained' */
		mif_debug("urb->status = -ENOENT\n");
	case 0:
		if (!urb->actual_length) {
			mif_debug("urb has zero length!\n");
			goto rx_submit;
		}

		/* call iod recv */
		/* how we can distinguish boot ch with fmt ch ?? */
		switch (pipe_data->format) {
		case IF_USB_FMT_EP:
			if (usb_ld->if_usb_is_main) {
				pr_urb("IPC-RX", urb);
				iod_format = IPC_FMT;
			} else {
				iod_format = IPC_BOOT;
				/*
				usb_mark_last_busy(usb_ld->usbdev);
				usb_ld->retry_urb = urb;
				queue_delayed_work(usb_ld->ld.tx_wq,
					&usb_ld->rx_retry_work,
					msecs_to_jiffies(20));
				return;
				*/
			}
			break;
		case IF_USB_RAW_EP:
			iod_format = IPC_MULTI_RAW;
			break;
		case IF_USB_RFS_EP:
			iod_format = IPC_RFS;
			break;
		case IF_USB_CMD_EP:
			iod_format = IPC_CMD;
			break;
		default:
			iod_format = -1;
			break;
		}

		/* flow control CMD by CP, not use io device */
		if (unlikely(iod_format == IPC_CMD)) {
			ret = link_rx_flowctl_cmd(&usb_ld->ld,
					(char *)urb->transfer_buffer,
					urb->actual_length);
			if (ret < 0)
				mif_err("no multi raw device (%d)\n", ret);
			goto rx_submit;
		}

		iod = link_get_iod_with_format(&usb_ld->ld, iod_format);
		if (iod) {
			ret = iod->recv(iod,
					&usb_ld->ld,
					(char *)urb->transfer_buffer,
					urb->actual_length);
			if (ret == -ENOMEM) {
				/* retry the delay work and resubit*/
				mif_err("ENOMEM, retry\n");
				if (usb_ld->usbdev)
					usb_mark_last_busy(usb_ld->usbdev);
				usb_ld->retry_urb = urb;
				queue_delayed_work(usb_ld->ld.tx_wq,
					&usb_ld->rx_retry_work, 0);
				return;
			}
			if (ret < 0)
				mif_err("io device recv error (%d)\n", ret);
		}
rx_submit:
		if (urb->status == 0) {
			if (usb_ld->usbdev)
				usb_mark_last_busy(usb_ld->usbdev);
			usb_rx_submit(usb_ld, pipe_data, GFP_ATOMIC);
		}
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		pr_err("%s: RX complete Status(%d)\n", __func__, urb->status);
		break;
	case -EOVERFLOW:
		pr_err("%s: RX overflow\n", __func__);
		break;
	case -EILSEQ:
		break;
	default:
		mif_err("urb err status = %d\n", urb->status);
		break;
	}
}

static int usb_send(struct link_device *ld, struct io_device *iod,
			struct sk_buff *skb)
{
	struct sk_buff_head *txq;
	size_t tx_size;
	struct usb_link_device *usb_ld = to_usb_link_device(ld);
	struct link_pm_data *pm_data = usb_ld->link_pm_data;

	switch (iod->format) {
	case IPC_RAW:
		txq = &ld->sk_raw_tx_q;

		if (unlikely(ld->raw_tx_suspended)) {
			mif_err("wait RESUME CMD...\n");
			INIT_COMPLETION(ld->raw_tx_resumed_by_cp);
			wait_for_completion(&ld->raw_tx_resumed_by_cp);
			mif_err("resumed done.\n");
		}
		break;
	case IPC_BOOT:
	case IPC_FMT:
	case IPC_RFS:
	default:
		txq = &ld->sk_fmt_tx_q;
		/* Hold wake_lock for getting schedule the tx_work */
		wake_lock(&pm_data->tx_async_wake);
		break;
	}
	/* store the tx size before run the tx_delayed_work*/
	tx_size = skb->len;

	/* drop packet, when link is not online */
	if (ld->com_state == COM_BOOT && iod->format != IPC_BOOT) {
		mif_err("%s: drop packet, size=%d, com_state=%d\n",
				iod->name, skb->len, ld->com_state);
		return -ENODEV;
	}

	/* en queue skb data */
	skb_queue_tail(txq, skb);

	if (!work_pending(&ld->tx_delayed_work.work))
		queue_delayed_work(ld->tx_wq, &ld->tx_delayed_work, 0);

	return tx_size;
}

static void usb_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct io_device *iod = skbpriv(skb)->iod;
	struct link_device *ld = skbpriv(skb)->ld;
	struct usb_link_device *usb_ld = to_usb_link_device(ld);

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	default:
		if (!iod && !iod->mc && iod->mc->phone_state != STATE_BOOTING)
			mif_err("TX error (%d)\n", urb->status);
	}

	dev_kfree_skb_any(skb);
	if (urb->dev && usb_ld->if_usb_connected)
		usb_mark_last_busy(urb->dev);
	usb_free_urb(urb);
}

/* Even if usb_tx_urb_with_skb is failed, does not release the skb to retry */
static int usb_tx_urb_with_skb(struct usb_device *usbdev, struct sk_buff *skb,
					struct if_usb_devdata *pipe_data)
{
	int ret;
	struct urb *urb;

	if (pipe_data->disconnected)
		return -ENOENT;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		mif_err("alloc urb error\n");
		return -ENOMEM;
	}
#if 0
	int i;
	for (i = 0; i < skb->len; i++) {
		if (i > 16)
			break;
		mif_err("[0x%02x]", *(skb->data + i));
	}
#endif
	urb->transfer_flags = URB_ZERO_PACKET;
	usb_fill_bulk_urb(urb, pipe_data->usbdev, pipe_data->tx_pipe, skb->data,
			skb->len, usb_tx_complete, (void *)skb);

	usb_mark_last_busy(usbdev);
	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret < 0) {
		mif_err("usb_submit_urb with ret(%d)\n", ret);
		usb_free_urb(urb);
		return ret;
	}

	return 0;
}


static int _usb_tx_work(struct sk_buff *skb)
{
	struct sk_buff_head *txq;
	struct io_device *iod = skbpriv(skb)->iod;
	struct link_device *ld = skbpriv(skb)->ld;
	struct usb_link_device *usb_ld = to_usb_link_device(ld);
	struct if_usb_devdata *pipe_data;
	int ret;

	switch (iod->format) {
	case IPC_BOOT:
	case IPC_FMT:
		/* boot device uses same intf with fmt*/
		pipe_data = &usb_ld->devdata[IF_USB_FMT_EP];
		txq = &ld->sk_fmt_tx_q;
		break;
	case IPC_RAW:
		pipe_data = &usb_ld->devdata[IF_USB_RAW_EP];
		txq = &ld->sk_raw_tx_q;
		break;
	case IPC_RFS:
		pipe_data = &usb_ld->devdata[IF_USB_RFS_EP];
		txq = &ld->sk_fmt_tx_q;
		break;
	default:
		/* wrong packet, drop it */
		pipe_data =  NULL;
		break;
	}

	if (!pipe_data) {
		dev_kfree_skb_any(skb);
		return -ENOENT;
	}

	if (iod->format == IPC_FMT && usb_ld->if_usb_is_main)
		pr_skb("IPC-TX", skb);

	if (iod->format == IPC_RAW)
		mif_debug("TX[RAW]\n");

	if (iod->format == IPC_RFS)
		pr_skb("RFS-TX", skb);

	if (!usb_ld->if_usb_connected || !usb_ld->usbdev)
		return -ENODEV;

	usb_mark_last_busy(usb_ld->usbdev);
	ret = usb_tx_urb_with_skb(usb_ld->usbdev,
				skb,
				pipe_data);
	if (ret < 0) {
		if (ret == -ENODEV || ret == -ENOENT) {
			mif_err("link broken while in runtime active ..."
					" purge!\n");
			return ret;
		}
		mif_err("usb_tx_urb_with_skb for iod(%d), ret=%d\n",
				iod->format, ret);
		skb_queue_head(txq, skb);
		queue_delayed_work(ld->tx_wq,
				&ld->tx_delayed_work,
				msecs_to_jiffies(20));
		return ret;
	}

	return 0;
}


static void usb_tx_work(struct work_struct *work)
{
	int ret = 0;
	struct link_device *ld =
		container_of(work, struct link_device, tx_delayed_work.work);
	struct usb_link_device *usb_ld = to_usb_link_device(ld);
	struct sk_buff *skb;
	struct link_pm_data *pm_data = usb_ld->link_pm_data;

	if (!usb_ld->usbdev) {
		mif_info("usbdev is invalid\n");
		return;
	}

	while (ld->sk_fmt_tx_q.qlen || ld->sk_raw_tx_q.qlen) {
		/* request and check usb runtime pm first */
		ret = link_pm_runtime_get_active(pm_data);
		if (ret < 0) {
			if (ret == -ENODEV)
				mif_err("link not avail, retry reconnect.\n");
			else
				queue_delayed_work(ld->tx_wq,
				&ld->tx_delayed_work, msecs_to_jiffies(20));
			return;
		}

		usb_mark_last_busy(usb_ld->usbdev);
		pm_runtime_get_sync(&usb_ld->usbdev->dev);

		ret = 0;
		/* send skb from fmt_txq and raw_txq,*/
		/* one by one for fair flow control */
		skb = skb_dequeue(&ld->sk_fmt_tx_q);
		if (skb)
			ret = _usb_tx_work(skb);

		if (ret) {
			if (ret != -ENODEV && ret != -ENOENT)
				pm_runtime_put(&usb_ld->usbdev->dev);
			/* Do not call runtime_put if ret is ENODEV. Unless it
			 * will invoke bugs */
			else
				skb_queue_head(&ld->sk_fmt_tx_q, skb);
			return;
		}

		skb = skb_dequeue(&ld->sk_raw_tx_q);
		if (skb)
			ret = _usb_tx_work(skb);

		if (ret) {
			if (ret != -ENODEV && ret != -ENOENT)
				pm_runtime_put(&usb_ld->usbdev->dev);
			else
				skb_queue_head(&ld->sk_raw_tx_q, skb);
			return;
		}

		pm_runtime_put(&usb_ld->usbdev->dev);
		usb_mark_last_busy(usb_ld->usbdev);
	}
	wake_unlock(&pm_data->tx_async_wake);
}

/*
#ifdef CONFIG_LINK_PM
*/

static int link_pm_runtime_get_active(struct link_pm_data *pm_data)
{
	int ret;
	struct usb_link_device *usb_ld = pm_data->usb_ld;
	struct device *dev = &usb_ld->usbdev->dev;

	if (!usb_ld->if_usb_connected || usb_ld->ld.com_state == COM_NONE)
		return -ENODEV;

	if (pm_data->dpm_suspending) {
		printk_ratelimited(KERN_DEBUG "[MIF] Kernel in suspending "
						"try get_active later\n");
		/* during dpm_suspending..
		 * if AP get tx data, wake up. */
		wake_lock(&pm_data->l2_wake);
		return -EAGAIN;
	}

	if (dev->power.runtime_status == RPM_ACTIVE) {
		pm_data->resume_retry_cnt = 0;
		return 0;
	}

	if (!pm_data->resume_requested) {
		mif_debug("QW PM\n");
		queue_delayed_work(pm_data->wq, &pm_data->link_pm_work, 0);
	}
	mif_debug("Wait pm\n");
	INIT_COMPLETION(pm_data->active_done);
	ret = wait_for_completion_timeout(&pm_data->active_done,
						msecs_to_jiffies(500));

	/* If usb link was disconnected while waiting ACTIVE State, usb device
	 * was removed, usb_ld->usbdev->dev is invalid and below
	 * dev->power.runtime_status is also invalid address.
	 * It will be occured LPA L3 -> AP iniated L0 -> disconnect -> link
	 * timeout
	 */
	if (!usb_ld->if_usb_connected || usb_ld->ld.com_state == COM_NONE) {
		mif_info("link disconnected after timed-out\n");
		return -ENODEV;
	}

	if (dev->power.runtime_status != RPM_ACTIVE) {
		mif_info("link_active (%d) retry\n",
						dev->power.runtime_status);
		return -EAGAIN;
	}
	mif_debug("link_active success(%d)\n", ret);
	return 0;
}

static void link_pm_runtime_start(struct work_struct *work)
{
	struct link_pm_data *pm_data =
		container_of(work, struct link_pm_data, link_pm_start.work);
	struct usb_device *usbdev = pm_data->usb_ld->usbdev;
	struct device *dev, *ppdev;
	struct link_device *ld = &pm_data->usb_ld->ld;

	if (!pm_data->usb_ld->if_usb_connected
		|| pm_data->usb_ld->ld.com_state == COM_NONE) {
		mif_debug("disconnect status, ignore\n");
		return;
	}

	dev = &pm_data->usb_ld->usbdev->dev;

	/* wait interface driver resumming */
	if (dev->power.runtime_status == RPM_SUSPENDED) {
		if (pm_data->usb_ld->ld.com_state != COM_ONLINE) {
			mif_info("com_state is not online (%d)\n",
				pm_data->usb_ld->ld.com_state);
			return;
		}
		mif_info("suspended yet, delayed work, com_state(%d)\n",
				pm_data->usb_ld->ld.com_state);
		queue_delayed_work(pm_data->wq, &pm_data->link_pm_start,
			msecs_to_jiffies(20));
		return;
	}

	if (pm_data->usb_ld->usbdev && dev->parent) {
		mif_info("rpm_status: %d\n",
			dev->power.runtime_status);
		usb_set_autosuspend_delay(usbdev, 200);
		ppdev = dev->parent->parent;
		pm_runtime_allow(dev);
		pm_runtime_allow(ppdev);/*ehci*/
		pm_data->link_pm_active = true;
		pm_data->resume_requested = false;
		pm_data->link_reconnect_cnt = 2;
		pm_data->resume_retry_cnt = 0;

		/* retry prvious link tx q */
		queue_delayed_work(ld->tx_wq, &ld->tx_delayed_work, 0);
	}
}

static void link_pm_change_modem_state(struct link_pm_data *pm_data,
						enum modem_state state)
{
	struct modem_ctl *mc = if_usb_get_modemctl(pm_data);

	if (!mc->iod || pm_data->usb_ld->ld.com_state != COM_ONLINE)
		return;

	mif_err("set modem state %d\n", state);
	mc->iod->modem_state_changed(mc->iod, state);
}

static void link_pm_reconnect_work(struct work_struct *work)
{
	struct link_pm_data *pm_data =
		container_of(work, struct link_pm_data,
					link_reconnect_work.work);
	struct modem_ctl *mc = if_usb_get_modemctl(pm_data);

	mif_info("\n");

	if (!mc || pm_data->usb_ld->if_usb_connected) {
		mif_err("mc or if_usb_connected is invalid\n");
		return;
	}

	if (pm_data->usb_ld->ld.com_state != COM_ONLINE) {
		mif_err("com_state is not COM_ONLINE\n");
		return;
	}

	if (pm_data->link_reconnect_cnt--) {
		if (mc->phone_state == STATE_ONLINE &&
						!pm_data->link_reconnect())
			/* try reconnect and check */
			schedule_delayed_work(&pm_data->link_reconnect_work,
							msecs_to_jiffies(500));
		else	/* under cp crash or reset, just return */
			return;
	} else {
		/* try to recover cp */
		mif_err("recover connection: silent reset\n");
		link_pm_change_modem_state(pm_data, STATE_CRASH_RESET);
	}
}

static inline int link_pm_slave_wake(struct link_pm_data *pm_data)
{
	int spin = 20;

	/* when slave device is in sleep, wake up slave cpu first */
	if (gpio_get_value(pm_data->gpio_link_hostwake)
				!= HOSTWAKE_TRIGLEVEL) {
		if (gpio_get_value(pm_data->gpio_link_slavewake)) {
			gpio_set_value(pm_data->gpio_link_slavewake, 0);
			mif_info("gpio [SWK] set [0]\n");
			mdelay(5);
		}
		gpio_set_value(pm_data->gpio_link_slavewake, 1);
		mif_info("gpio [SWK] set [1]\n");
		mdelay(5);

		/* wait host wake signal*/
		while (spin-- && gpio_get_value(pm_data->gpio_link_hostwake) !=
							HOSTWAKE_TRIGLEVEL)
			mdelay(5);
	}
	return spin;
}

static void link_pm_runtime_work(struct work_struct *work)
{
	int ret;
	struct link_pm_data *pm_data =
		container_of(work, struct link_pm_data, link_pm_work.work);
	struct device *dev = &pm_data->usb_ld->usbdev->dev;

	if (!pm_data->usb_ld->if_usb_connected || pm_data->dpm_suspending)
		return;

	if (pm_data->usb_ld->ld.com_state == COM_NONE)
		return;

	mif_debug("for dev 0x%p : current %d\n", dev,
					dev->power.runtime_status);

	switch (dev->power.runtime_status) {
	case RPM_ACTIVE:
		pm_data->resume_retry_cnt = 0;
		pm_data->resume_requested = false;
		complete(&pm_data->active_done);

		return;
	case RPM_SUSPENDED:
		if (pm_data->resume_requested)
			break;
		pm_data->resume_requested = true;
		wake_lock(&pm_data->rpm_wake);
		ret = link_pm_slave_wake(pm_data);
		if (ret < 0) {
			mif_err("slave wake fail\n");
			wake_unlock(&pm_data->rpm_wake);
			break;
		}

		if (!pm_data->usb_ld->if_usb_connected) {
			wake_unlock(&pm_data->rpm_wake);
			return;
		}

		ret = pm_runtime_resume(dev);
		if (ret < 0) {
			mif_err("resume error(%d)\n", ret);
			if (!pm_data->usb_ld->if_usb_connected) {
				wake_unlock(&pm_data->rpm_wake);
				return;
			}
			/* force to go runtime idle before retry resume */
			if (dev->power.timer_expires == 0 &&
						!dev->power.request_pending) {
				mif_debug("run time idle\n");
				pm_runtime_idle(dev);
			}
		}
		wake_unlock(&pm_data->rpm_wake);
		break;
	default:
		break;
	}
	pm_data->resume_requested = false;

	/* check until runtime_status goes to active */
	/* attemp 10 times, or re-establish modem-link */
	/* if pm_runtime_resume run properly, rpm status must be in ACTIVE */
	if (dev->power.runtime_status == RPM_ACTIVE) {
		pm_data->resume_retry_cnt = 0;
		complete(&pm_data->active_done);
	} else if (pm_data->resume_retry_cnt++ > 10)
		link_pm_change_modem_state(pm_data, STATE_CRASH_RESET);
	else
		queue_delayed_work(pm_data->wq, &pm_data->link_pm_work,
							msecs_to_jiffies(20));
}

static irqreturn_t link_pm_irq_handler(int irq, void *data)
{
	int value;
	struct link_pm_data *pm_data = data;

#if defined(CONFIG_SLP)
	pm_wakeup_event(pm_data->miscdev.this_device, 0);
#endif

	if (!pm_data->link_pm_active)
		return IRQ_HANDLED;

	/* host wake up HIGH */
	/*
		resume usb runtime pm start
	*/
	/* host wake up LOW */
	/*
		slave usb enumeration end,
		host can send usb packet after
		runtime pm status changes to ACTIVE
	*/
	value = gpio_get_value(pm_data->gpio_link_hostwake);
	mif_info("gpio [HWK] get [%d]\n", value);

	/*
	* igonore host wakeup interrupt at suspending kernel
	*/
	if (pm_data->dpm_suspending) {
		mif_info("ignore request by suspending\n");
		/* Ignore HWK but AP got to L2 by suspending fail */
		wake_lock(&pm_data->l2_wake);
		return IRQ_HANDLED;
	}

	if (value == HOSTWAKE_TRIGLEVEL) {
		/* move to slave wake function */
		/* runtime pm goes to active */
		/*
		if (gpio_get_value(pm_data->gpio_link_active)) {
			mif_err("gpio [H ACTV : %d] set 1\n",
				gpio_get_value(pm_data->gpio_link_active));
			gpio_set_value(pm_data->gpio_link_active, 1);
		}
		*/
		queue_delayed_work(pm_data->wq, &pm_data->link_pm_work, 0);
	} else {
		/* notification of enumeration process from slave device
		 * But it does not mean whole connection is in resume, so do not
		 * notify resume completion here.

		if (pm_data->link_pm_active && !pm_data->active_done.done)
			complete(&pm_data->active_done);
		*/
		/* clear slave cpu wake up pin */
		gpio_set_value(pm_data->gpio_link_slavewake, 0);
		mif_info("gpio [SWK] set [0]\n");
	}
	return IRQ_HANDLED;
}

static long link_pm_ioctl(struct file *file, unsigned int cmd,
						unsigned long arg)
{
	int value;
	struct link_pm_data *pm_data = file->private_data;

	mif_info("%x\n", cmd);

	switch (cmd) {
	case IOCTL_LINK_CONTROL_ENABLE:
		if (copy_from_user(&value, (const void __user *)arg,
							sizeof(int)))
			return -EFAULT;
		if (pm_data->link_ldo_enable)
			pm_data->link_ldo_enable(!!value);
		if (pm_data->gpio_link_enable)
			gpio_set_value(pm_data->gpio_link_enable, value);
		break;
	case IOCTL_LINK_CONTROL_ACTIVE:
		if (copy_from_user(&value, (const void __user *)arg,
							sizeof(int)))
			return -EFAULT;
		gpio_set_value(pm_data->gpio_link_active, value);
		break;
	case IOCTL_LINK_GET_HOSTWAKE:
		return !gpio_get_value(pm_data->gpio_link_hostwake);
	case IOCTL_LINK_CONNECTED:
		return pm_data->usb_ld->if_usb_connected;
	case IOCTL_LINK_SET_BIAS_CLEAR:
		if (copy_from_user(&value, (const void __user *)arg,
							sizeof(int)))
			return -EFAULT;
		if (value) {
			gpio_direction_output(pm_data->gpio_link_slavewake, 0);
			gpio_direction_output(pm_data->gpio_link_hostwake, 0);
		} else {
			gpio_direction_output(pm_data->gpio_link_slavewake, 0);
			gpio_direction_input(pm_data->gpio_link_hostwake);
			irq_set_irq_type(pm_data->irq_link_hostwake,
				IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING);
		}
	default:
		break;
	}

	return 0;
}

static int link_pm_open(struct inode *inode, struct file *file)
{
	struct link_pm_data *pm_data =
		(struct link_pm_data *)file->private_data;
	file->private_data = (void *)pm_data;
	return 0;
}

static int link_pm_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static const struct file_operations link_pm_fops = {
	.owner = THIS_MODULE,
	.open = link_pm_open,
	.release = link_pm_release,
	.unlocked_ioctl = link_pm_ioctl,
};

static int link_pm_notifier_event(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	struct link_pm_data *pm_data =
			container_of(this, struct link_pm_data,	pm_notifier);
	struct usb_device *usbdev = pm_data->usb_ld->usbdev;
#ifdef CONFIG_UMTS_MODEM_XMM6262
	struct modem_ctl *mc = if_usb_get_modemctl(pm_data);
#endif

	switch (event) {
	case PM_SUSPEND_PREPARE:
		pm_data->dpm_suspending = true;
#ifdef CONFIG_UMTS_MODEM_XMM6262
		/* set PDA Active High if previous state was LPA */
		gpio_set_value(mc->gpio_pda_active, 1);
#endif
		mif_debug("dpm suspending set to true\n");
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		pm_data->dpm_suspending = false;
		mif_info("dpm suspending set to false\n");
		/*
		 * tegra bsp does not need this routine with K31
		 */
		#if 0
		/*
		 * L3 -> L0 : kick runtime pm by calling pm_start
		 */
		queue_delayed_work(pm_data->wq, &pm_data->link_pm_start, 0);
		#endif
		if (gpio_get_value(pm_data->gpio_link_hostwake)
			== HOSTWAKE_TRIGLEVEL) {
			queue_delayed_work(pm_data->wq, &pm_data->link_pm_work,
				0);
			pr_info("%s: post resume\n", __func__);
		}
		usb_set_autosuspend_delay(usbdev, 200);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static struct modem_ctl *if_usb_get_modemctl(struct link_pm_data *pm_data)
{
	struct io_device *iod;

	iod = link_get_iod_with_format(&pm_data->usb_ld->ld, IPC_FMT);
	if (!iod) {
		mif_err("no iodevice for modem control\n");
		return NULL;
	}

	return iod->mc;
}

static int if_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct if_usb_devdata *devdata = usb_get_intfdata(intf);
	struct link_pm_data *pm_data = devdata->usb_ld->link_pm_data;
	if (!devdata->disconnected && devdata->state == STATE_RESUMED) {
		usb_kill_urb(devdata->urb);
		devdata->state = STATE_SUSPENDED;
/*grzwolf-beg*/
                mif_info("if_usb_supend usb_kill_urb\n");
/*grzwolf-end*/
	}

	devdata->usb_ld->suspended++;

/*grzwolf-beg*/
	if (devdata->usb_ld->suspended == LINKPM_DEV_NUM) {
		mif_info("[if_usb_suspended]\n");
		wake_lock_timeout(&pm_data->l2_wake, msecs_to_jiffies(50));
//	}
//	if ( devdata->usb_ld->suspended == LINKPM_DEV_NUM ) {
//           g_timeout++;
//           if ( (g_timeout % 2) != 0 ) {  // g_timeout is odd number  
//              mif_info("[if_usb_suspended] - wake_lock_timeout sent\n"); 
//  	      wake_lock_timeout(&pm_data->l2_wake, msecs_to_jiffies(50));
//           } else {
//              mif_info("[if_usb_suspended] - wake_lock_timeout suppressed\n"); 
//           } 
	} else {
           mif_info("if_usb_supend NO SUSPEND: %i\t%i\n", devdata->usb_ld->suspended, LINKPM_DEV_NUM);
        }
/*grzwolf-end*/
	return 0;
}

static int if_usb_resume(struct usb_interface *intf)
{
	int ret;
	struct if_usb_devdata *devdata = usb_get_intfdata(intf);
	struct link_pm_data *pm_data = devdata->usb_ld->link_pm_data;

	if (!devdata->disconnected && devdata->state == STATE_SUSPENDED) {
		ret = usb_rx_submit(devdata->usb_ld, devdata, GFP_ATOMIC);
		if (ret < 0) {
			mif_err("usb_rx_submit error with (%d)\n", ret);
			return ret;
		}
		devdata->state = STATE_RESUMED;
	}

	devdata->usb_ld->suspended--;
	if (!devdata->usb_ld->suspended) {
		mif_info("[if_usb_resumed]\n");
		wake_lock(&pm_data->l2_wake);
	}

	return 0;
}

static int if_usb_reset_resume(struct usb_interface *intf)
{
	int ret;
	struct if_usb_devdata *devdata = usb_get_intfdata(intf);
	struct device *dev, *pdev;

	ret = if_usb_resume(intf);
	if (ret)
		return ret;

	/*
	 * for, Tegra K31 release it need to make root_hub runtime enable
	 */
	if (!devdata->usb_ld->suspended) {
		dev = &devdata->usb_ld->usbdev->dev;
		pdev = dev->parent;
		pm_runtime_allow(pdev);
	}
	return 0;
}

static void if_usb_disconnect(struct usb_interface *intf)
{
	struct if_usb_devdata *devdata = usb_get_intfdata(intf);
	struct link_pm_data *pm_data = devdata->usb_ld->link_pm_data;
	struct device *dev, *ppdev;

	mif_info("\n");

	if (devdata->disconnected)
		return;

	usb_driver_release_interface(to_usb_driver(intf->dev.driver), intf);

	usb_kill_urb(devdata->urb);

	dev = &devdata->usb_ld->usbdev->dev;
	ppdev = dev->parent->parent;
	pm_runtime_disable(dev);
	pm_runtime_disable(ppdev); /*ehci*/

	devdata->usb_ld->if_usb_connected = 0;

	mif_info("put dev 0x%p\n", devdata->usbdev);
	usb_put_dev(devdata->usbdev);

	devdata->data_intf = NULL;
	devdata->usbdev = NULL;
	/* if possible, merge below 2 variables */
	devdata->disconnected = 1;
	devdata->state = STATE_SUSPENDED;

	devdata->usb_ld->suspended = 0;
	wake_lock(&pm_data->boot_wake);

	usb_set_intfdata(intf, NULL);

	/* cancel runtime start delayed works */
	cancel_delayed_work_sync(&pm_data->link_pm_start);

	/* if reconnect function exist , try reconnect without reset modem
	 * reconnect function checks modem is under crash or not, so we don't
	 * need check crash state here. reconnect work checks and determine
	 * further works
	 */
	if (!pm_data->link_reconnect)
		return;

	if (devdata->usb_ld->ld.com_state != COM_ONLINE) {
		cancel_delayed_work(&pm_data->link_reconnect_work);
		return;
	} else
		schedule_delayed_work(&pm_data->link_reconnect_work,
							msecs_to_jiffies(200));
	return;
}

static int if_usb_set_pipe(struct usb_link_device *usb_ld,
			const struct usb_host_interface *desc, int pipe)
{
	if (pipe < 0 || pipe >= IF_USB_DEVNUM_MAX) {
		mif_err("undefined endpoint, exceed max\n");
		return -EINVAL;
	}

	mif_info("set %d\n", pipe);

	if ((usb_pipein(desc->endpoint[0].desc.bEndpointAddress)) &&
	    (usb_pipeout(desc->endpoint[1].desc.bEndpointAddress))) {
		usb_ld->devdata[pipe].rx_pipe = usb_rcvbulkpipe(usb_ld->usbdev,
				desc->endpoint[0].desc.bEndpointAddress);
		usb_ld->devdata[pipe].tx_pipe = usb_sndbulkpipe(usb_ld->usbdev,
				desc->endpoint[1].desc.bEndpointAddress);
	} else if ((usb_pipeout(desc->endpoint[0].desc.bEndpointAddress)) &&
		   (usb_pipein(desc->endpoint[1].desc.bEndpointAddress))) {
		usb_ld->devdata[pipe].rx_pipe = usb_rcvbulkpipe(usb_ld->usbdev,
				desc->endpoint[1].desc.bEndpointAddress);
		usb_ld->devdata[pipe].tx_pipe = usb_sndbulkpipe(usb_ld->usbdev,
				desc->endpoint[0].desc.bEndpointAddress);
	} else {
		mif_err("undefined endpoint\n");
		return -EINVAL;
	}

	return 0;
}

static struct usb_id_info hsic_channel_info;

static int __devinit if_usb_probe(struct usb_interface *intf,
					const struct usb_device_id *id)
{
	int err;
	int pipe;
	const struct usb_cdc_union_desc *union_hdr;
	const struct usb_host_interface *data_desc;
	unsigned char *buf = intf->altsetting->extra;
	int buflen = intf->altsetting->extralen;
	struct usb_interface *data_intf;
	struct usb_device *usbdev = interface_to_usbdev(intf);
	struct usb_driver *usbdrv = to_usb_driver(intf->dev.driver);
	struct usb_id_info *info = (struct usb_id_info *)id->driver_info;
	struct usb_link_device *usb_ld = info->usb_ld;

	mif_info("usbdev = 0x%p\n", usbdev);

	usb_ld->usbdev = usbdev;
	pm_runtime_forbid(&usbdev->dev);
	usb_ld->link_pm_data->link_pm_active = false;
	usb_ld->link_pm_data->dpm_suspending = false;
	usb_ld->if_usb_is_main = (info == &hsic_channel_info);

	union_hdr = NULL;
	/* for WMC-ACM compatibility, WMC-ACM use an end-point for control msg*/
	if (intf->altsetting->desc.bInterfaceSubClass != USB_CDC_SUBCLASS_ACM) {
		mif_err("ignore Non ACM end-point\n");
		return -EINVAL;
	}

	if (!buflen) {
		if (intf->cur_altsetting->endpoint->extralen &&
				    intf->cur_altsetting->endpoint->extra) {
			buflen = intf->cur_altsetting->endpoint->extralen;
			buf = intf->cur_altsetting->endpoint->extra;
		} else {
			mif_err("Zero len descriptor reference\n");
			return -EINVAL;
		}
	}

	while (buflen > 0) {
		if (buf[1] == USB_DT_CS_INTERFACE) {
			switch (buf[2]) {
			case USB_CDC_UNION_TYPE:
				if (union_hdr)
					break;
				union_hdr = (struct usb_cdc_union_desc *)buf;
				break;
			default:
				break;
			}
		}
		buf += buf[0];
		buflen -= buf[0];
	}

	if (!union_hdr) {
		mif_err("USB CDC is not union type\n");
		return -EINVAL;
	}

	data_intf = usb_ifnum_to_if(usbdev, union_hdr->bSlaveInterface0);
	if (!data_intf) {
		mif_err("data_inferface is NULL\n");
		return -ENODEV;
	}

	data_desc = data_intf->altsetting;
	if (!data_desc) {
		mif_err("data_desc is NULL\n");
		return -ENODEV;
	}

	switch (info->intf_id) {
	case BOOT_DOWN:
		pipe = IF_USB_BOOT_EP;
		usb_ld->ld.com_state = COM_BOOT;
		usb_ld->boot_ep = data_desc->endpoint;
		/* purge previous boot fmt/raw tx q
		 clear all tx q*/
		skb_queue_purge(&usb_ld->ld.sk_fmt_tx_q);
		skb_queue_purge(&usb_ld->ld.sk_raw_tx_q);
		break;
	case IPC_CHANNEL:
		pipe = intf->altsetting->desc.bInterfaceNumber / 2;
		usb_ld->ld.com_state = COM_ONLINE;
		break;
	default:
		pipe = -1;
		break;
	}

	if (if_usb_set_pipe(usb_ld, data_desc, pipe) < 0)
		return -EINVAL;

	usb_ld->devdata[pipe].usbdev = usb_get_dev(usbdev);
	mif_info("devdata usbdev = 0x%p\n",
		usb_ld->devdata[pipe].usbdev);
	usb_ld->devdata[pipe].usb_ld = usb_ld;
	usb_ld->devdata[pipe].data_intf = data_intf;
	usb_ld->devdata[pipe].format = pipe;
	usb_ld->devdata[pipe].disconnected = 0;
	usb_ld->devdata[pipe].state = STATE_RESUMED;

	usb_ld->if_usb_connected = 1;
	usb_ld->suspended = 0;

	err = usb_driver_claim_interface(usbdrv, data_intf,
		(void *)&usb_ld->devdata[pipe]);
	if (err < 0) {
		mif_err("usb_driver_claim() failed\n");
		return err;
	}

	pm_suspend_ignore_children(&usbdev->dev, true);

	usb_set_intfdata(intf, (void *)&usb_ld->devdata[pipe]);

	/* rx start for this endpoint */
	usb_rx_submit(usb_ld, &usb_ld->devdata[pipe], GFP_KERNEL);

	if (info->intf_id == IPC_CHANNEL &&
		!work_pending(&usb_ld->link_pm_data->link_pm_start.work)) {
			queue_delayed_work(usb_ld->link_pm_data->wq,
					&usb_ld->link_pm_data->link_pm_start,
					msecs_to_jiffies(10000));
			wake_lock(&usb_ld->link_pm_data->l2_wake);
			wake_unlock(&usb_ld->link_pm_data->boot_wake);
	}

	/* HSIC main comm channel has been established */
	if (pipe == IF_USB_CMD_EP) {
		link_pm_change_modem_state(usb_ld->link_pm_data, STATE_ONLINE);
		enable_irq(usb_ld->link_pm_data->irq_link_hostwake);
	}

	mif_info("successfully done\n");

	return 0;
}

static void if_usb_free_pipe_data(struct usb_link_device *usb_ld)
{
	int i;
	for (i = 0; i < IF_USB_DEVNUM_MAX; i++) {
		kfree(usb_ld->devdata[i].rx_buf);
		usb_kill_urb(usb_ld->devdata[i].urb);
	}
}

static struct usb_id_info hsic_boot_down_info = {
	.intf_id = BOOT_DOWN,
};
static struct usb_id_info hsic_channel_info = {
	.intf_id = IPC_CHANNEL,
};

static struct usb_device_id if_usb_ids[] = {
	{USB_DEVICE(IMC_BOOT_VID, IMC_BOOT_PID),
	.driver_info = (unsigned long)&hsic_boot_down_info,},
	{USB_DEVICE(IMC_MAIN_VID, IMC_MAIN_PID),
	.driver_info = (unsigned long)&hsic_channel_info,},
	{USB_DEVICE(STE_BOOT_VID, STE_BOOT_PID),
	.driver_info = (unsigned long)&hsic_boot_down_info,},
	{USB_DEVICE(STE_MAIN_VID, STE_MAIN_PID),
	.driver_info = (unsigned long)&hsic_channel_info,},
	{}
};
MODULE_DEVICE_TABLE(usb, if_usb_ids);

static struct usb_driver if_usb_driver = {
	.name =		"cdc_modem",
	.probe =		if_usb_probe,
	.disconnect =	if_usb_disconnect,
	.id_table =	if_usb_ids,
	.suspend =	if_usb_suspend,
	.resume =	if_usb_resume,
	.reset_resume =	if_usb_reset_resume,
	.supports_autosuspend = 1,
};

static int if_usb_init(struct link_device *ld)
{
	int ret;
	int i;
	struct usb_link_device *usb_ld = to_usb_link_device(ld);
	struct if_usb_devdata *pipe_data;
	struct usb_id_info *id_info;

	/* to connect usb link device with usb interface driver */
	for (i = 0; i < ARRAY_SIZE(if_usb_ids); i++) {
		id_info = (struct usb_id_info *)if_usb_ids[i].driver_info;
		if (id_info)
			id_info->usb_ld = usb_ld;
	}

	ret = usb_register(&if_usb_driver);
	if (ret) {
		mif_err("usb_register_driver() fail : %d\n", ret);
		return ret;
	}

	/* allocate rx buffer for usb receive */
	for (i = 0; i < IF_USB_DEVNUM_MAX; i++) {
		pipe_data = &usb_ld->devdata[i];
		pipe_data->format = i;
		pipe_data->rx_buf_size = 16 * 1024;

		pipe_data->rx_buf = kmalloc(pipe_data->rx_buf_size,
						GFP_DMA | GFP_KERNEL);
		if (!pipe_data->rx_buf) {
			if_usb_free_pipe_data(usb_ld);
			ret = -ENOMEM;
			break;
		}

		pipe_data->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!pipe_data->urb) {
			mif_err("alloc urb fail\n");
			if_usb_free_pipe_data(usb_ld);
			return -ENOMEM;
		}
	}

	mif_info("if_usb_init() done : %d, usb_ld (0x%p)\n",
								ret, usb_ld);
	return ret;
}

static int usb_link_pm_init(struct usb_link_device *usb_ld, void *data)
{
	int r;
	struct platform_device *pdev = (struct platform_device *)data;
	struct modem_data *pdata =
			(struct modem_data *)pdev->dev.platform_data;
	struct modemlink_pm_data *pm_pdata = pdata->link_pm_data;
	struct link_pm_data *pm_data =
			kzalloc(sizeof(struct link_pm_data), GFP_KERNEL);
	if (!pm_data) {
		mif_err("link_pm_data is NULL\n");
		return -ENOMEM;
	}
	/* get link pm data from modemcontrol's platform data */
	pm_data->gpio_link_active = pm_pdata->gpio_link_active;
	pm_data->gpio_link_enable = pm_pdata->gpio_link_enable;
	pm_data->gpio_link_hostwake = pm_pdata->gpio_link_hostwake;
	pm_data->gpio_link_slavewake = pm_pdata->gpio_link_slavewake;
	pm_data->irq_link_hostwake = gpio_to_irq(pm_data->gpio_link_hostwake);
	pm_data->link_ldo_enable = pm_pdata->link_ldo_enable;
	pm_data->link_reconnect = pm_pdata->link_reconnect;

	pm_data->usb_ld = usb_ld;
	pm_data->link_pm_active = false;
	usb_ld->link_pm_data = pm_data;

	pm_data->miscdev.minor = MISC_DYNAMIC_MINOR;
	pm_data->miscdev.name = "link_pm";
	pm_data->miscdev.fops = &link_pm_fops;

	r = misc_register(&pm_data->miscdev);
	if (r < 0) {
		mif_err("fail to register pm device(%d)\n", r);
		goto err_misc_register;
	}

	r = request_irq(pm_data->irq_link_hostwake, link_pm_irq_handler,
		IRQF_NO_SUSPEND | IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
		"hostwake", (void *)pm_data);
	if (r) {
		mif_err("fail to request irq(%d)\n", r);
		goto err_request_irq;
	}

	r = enable_irq_wake(pm_data->irq_link_hostwake);
	if (r) {
		mif_err("failed to enable_irq_wake:%d\n", r);
		goto err_set_wake_irq;
	}
	disable_irq(pm_data->irq_link_hostwake);

	/* create work queue & init work for runtime pm */
	pm_data->wq = create_singlethread_workqueue("linkpmd");
	if (!pm_data->wq) {
		mif_err("fail to create wq\n");
		goto err_create_wq;
	}

	pm_data->pm_notifier.notifier_call = link_pm_notifier_event;
	register_pm_notifier(&pm_data->pm_notifier);

	init_completion(&pm_data->active_done);
	INIT_DELAYED_WORK(&pm_data->link_pm_work, link_pm_runtime_work);
	INIT_DELAYED_WORK(&pm_data->link_pm_start, link_pm_runtime_start);
	INIT_DELAYED_WORK(&pm_data->link_reconnect_work,
						link_pm_reconnect_work);
	wake_lock_init(&pm_data->l2_wake, WAKE_LOCK_SUSPEND, "l2_hsic");
	wake_lock_init(&pm_data->boot_wake, WAKE_LOCK_SUSPEND, "boot_hsic");
	wake_lock_init(&pm_data->rpm_wake, WAKE_LOCK_SUSPEND, "rpm_hsic");
	wake_lock_init(&pm_data->tx_async_wake, WAKE_LOCK_SUSPEND, "tx_hsic");

#if defined(CONFIG_SLP)
	device_init_wakeup(pm_data->miscdev.this_device, true);
#endif

	return 0;

err_create_wq:
	disable_irq_wake(pm_data->irq_link_hostwake);
err_set_wake_irq:
	free_irq(pm_data->irq_link_hostwake, (void *)pm_data);
err_request_irq:
	misc_deregister(&pm_data->miscdev);
err_misc_register:
	kfree(pm_data);
	return r;
}

struct link_device *hsic_create_link_device(void *data)
{
	int ret;
	struct usb_link_device *usb_ld;
	struct link_device *ld;

	usb_ld = kzalloc(sizeof(struct usb_link_device), GFP_KERNEL);
	if (!usb_ld)
		return NULL;

	INIT_LIST_HEAD(&usb_ld->ld.list);
	skb_queue_head_init(&usb_ld->ld.sk_fmt_tx_q);
	skb_queue_head_init(&usb_ld->ld.sk_raw_tx_q);

	ld = &usb_ld->ld;

	ld->name = "usb";
	ld->init_comm = usb_init_communication;
	ld->terminate_comm = usb_terminate_communication;
	ld->send = usb_send;
	ld->com_state = COM_NONE;
	ld->raw_tx_suspended = false;
	init_completion(&ld->raw_tx_resumed_by_cp);

	ld->tx_wq = create_singlethread_workqueue("usb_tx_wq");
	if (!ld->tx_wq) {
		mif_err("fail to create work Q.\n");
		goto err;
	}

	INIT_DELAYED_WORK(&ld->tx_delayed_work, usb_tx_work);
	INIT_DELAYED_WORK(&usb_ld->rx_retry_work, usb_rx_retry_work);
	usb_ld->rx_retry_cnt = 0;

	/* create link pm device */
	ret = usb_link_pm_init(usb_ld, data);
	if (ret)
		goto err;

	ret = if_usb_init(ld);
	if (ret)
		goto err;

	mif_info("%s : create_link_device DONE\n", usb_ld->ld.name);
	return (void *)ld;
err:
	kfree(usb_ld);
	return NULL;
}

static void __exit if_usb_exit(void)
{
	usb_deregister(&if_usb_driver);
}
