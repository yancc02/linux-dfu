/*
 * usbdfu.c
 *
 * Copyright (c) 2017 Dashi Cao        <dscao999@hotmail.com, caods1@lenovo.com>
 *
 * USB Abstract Control Model driver for USB Device Firmware Upgrade
 *
*/
#include <stdarg.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>
#include "usbdfu.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dashi Cao");
MODULE_DESCRIPTION("USB DFU Class Driver");

static int max_dfus = 8;
module_param(max_dfus, int, S_IRUGO);
static int urb_timeout = 200; /* milliseconds */
module_param(urb_timeout, int, S_IRUGO | S_IWUSR);
static int detach_timeout = 2000; /* 2 seconds */
module_param(detach_timeout, int, S_IRUGO | S_IWUSR);

static const struct usb_device_id dfu_ids[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_APP_SPEC, USB_DFU_SUBCLASS,
		USB_DFU_PROTO_RUNTIME) },
	{ }
};
MODULE_DEVICE_TABLE(usb, dfu_ids);

static dev_t dfu_devnum;
struct class *dfu_class;
EXPORT_SYMBOL_GPL(dfu_class);

static atomic_t dfu_index = ATOMIC_INIT(-1);

static void dfu_ctrlurb_done(struct urb *urb)
{
	struct dfu_control *ctrl;

	ctrl = urb->context;
	ctrl->status = urb->status;
	ctrl->nxfer = urb->actual_length;
	complete(&ctrl->urbdone);
}

static void dfu_urb_timeout(const struct dfu_device *dfudev,
		struct dfu_control *ctrl)
{
	usb_unlink_urb(ctrl->urb);
	wait_for_completion(&ctrl->urbdone);
	if (ctrl->req.bRequest != USB_DFU_ABORT)
		dev_err(&dfudev->intf->dev,
			"URB req type: %2.2x, req: %2.2x cancelled\n",
			(int)ctrl->req.bRequestType,
			(int)ctrl->req.bRequest);
}

static int dfu_do_switch(struct dfu_device *dfudev, struct dfu_control *ctrl)
{
	int tmout, retusb;

	tmout = dfudev->dettmout > detach_timeout ?
				detach_timeout : dfudev->dettmout;
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_DETACH;
	ctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	ctrl->req.wValue = cpu_to_le16(tmout);
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(dfudev->usbdev, 0);
	ctrl->buff = NULL;
	ctrl->len = 0;
	ctrl->urb = NULL;
	retusb = dfu_submit_urb(dfudev, ctrl);
	if (retusb == 0 && (dfudev->attr & 0x08) == 0)
		dev_info(&dfudev->intf->dev, "Need reset to switch to DFU\n");
	return retusb;
}

static ssize_t dfu_switch(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct dfu_device *dfudev;
	struct dfu_control *ctrl;

	dfudev = container_of(attr, struct dfu_device, actattr);
	ctrl = kmalloc(sizeof(struct dfu_control), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	if (count > 0 && *buf == '-' && (*(buf+1) == '\n' || *(buf+1) == 0))
		dfu_do_switch(dfudev, ctrl);
	else
		dev_err(dev, "Invalid Command: %c\n", *buf);

	kfree(ctrl);
	return count;
}

static ssize_t dfu_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct dfu_device *dfudev;
	int retv;
	const char *fmt = "Attribute: %#02.2x Timeout: %d Transfer Size: %d\n";

	retv = 0;
	dfudev = container_of(attr, struct dfu_device, actattr);
	retv = snprintf(buf, 128, fmt, dfudev->attr, dfudev->dettmout,
			dfudev->xfersize);
	return retv;
}

static int dfu_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	int retv;
	struct dfu_device *dfudev;

	retv = dfu_prepare(&dfudev, intf, id);
	if (retv)
		return retv;

	dfudev->actattr.attr.name = "detach";
	dfudev->actattr.attr.mode = S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH;
	dfudev->actattr.show = dfu_show;
	dfudev->actattr.store = dfu_switch;
	retv = device_create_file(&intf->dev, &dfudev->actattr);
	if (retv != 0)
		dev_err(&intf->dev, "Cannot create sysfs file %d\n", retv);
	dfudev->proto = 1;

	return retv;
}

static void dfu_disconnect(struct usb_interface *intf)
{
	struct dfu_device *dfudev;

	dfudev = usb_get_intfdata(intf);
	device_remove_file(&intf->dev, &dfudev->actattr);
	dfu_cleanup(intf, dfudev);
}

static struct usb_driver dfu_driver = {
	.name = "usbdfu",
	.probe = dfu_probe,
	.disconnect = dfu_disconnect,
	.id_table = dfu_ids,
};

static int __init usbdfu_init(void)
{
	int retv;

	retv = alloc_chrdev_region(&dfu_devnum, 0, max_dfus, DFUDEV_NAME);
	if (retv != 0) {
		pr_err("Cannot allocate a char major number: %d\n", retv);
		return retv;
	}
	dfu_class = class_create(THIS_MODULE, DFUDEV_NAME);
	if (IS_ERR(dfu_class)) {
		retv = (int)PTR_ERR(dfu_class);
		pr_err("Cannot create DFU class, Out of Memory!\n");
		goto err_10;
	}
	retv = usb_register(&dfu_driver);
	if (retv) {
		pr_err("Cannot register USB DFU driver: %d\n", retv);
		goto err_20;
	}

	return retv;

err_20:
	class_destroy(dfu_class);
err_10:
	unregister_chrdev_region(dfu_devnum, max_dfus);
	return retv;
}

static void __exit usbdfu_exit(void)
{
	usb_deregister(&dfu_driver);
	class_destroy(dfu_class);
	unregister_chrdev_region(dfu_devnum, max_dfus);
}

module_init(usbdfu_init);
module_exit(usbdfu_exit);

int dfu_submit_urb(const struct dfu_device *dfudev,
		struct dfu_control *ctrl)
{
	int retusb, retv, alloc;
	unsigned long jiff_wait;

	alloc = 0;
	if (ctrl->urb == NULL) {
		ctrl->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!ctrl->urb) {
			dev_err(&dfudev->intf->dev, "Cannot allocate URB\n");
			return -ENOMEM;
		}
		alloc = 1;
	}
	usb_fill_control_urb(ctrl->urb, dfudev->usbdev, ctrl->pipe,
			(__u8 *)&ctrl->req, ctrl->buff, ctrl->len,
			dfu_ctrlurb_done, ctrl);
	init_completion(&ctrl->urbdone);
	ctrl->status = USB_DFU_ERROR_CODE;
	retusb = usb_submit_urb(ctrl->urb, GFP_KERNEL);
	if (retusb == 0) {
		jiff_wait = msecs_to_jiffies(urb_timeout);
		if (!wait_for_completion_timeout(&ctrl->urbdone, jiff_wait))
			dfu_urb_timeout(dfudev, ctrl);
	} else
		dev_err(&dfudev->intf->dev,
			"URB type: %2.2x, req: %2.2x submit failed: %d\n",
			(int)ctrl->req.bRequestType,
			(int)ctrl->req.bRequest, retusb);
	if (alloc) {
		usb_free_urb(ctrl->urb);
		ctrl->urb = NULL;
	}
	retv = ACCESS_ONCE(ctrl->status);
	if (retv && ctrl->req.bRequest != USB_DFU_ABORT)
		dev_err(&dfudev->intf->dev,
			"URB type: %2.2x, req: %2.2x request failed: %d\n",
			(int)ctrl->req.bRequestType, (int)ctrl->req.bRequest,
			ctrl->status);

	return ctrl->status;
}
EXPORT_SYMBOL_GPL(dfu_submit_urb);

int dfu_prepare(struct dfu_device **dfudevp, struct usb_interface *intf,
			const struct usb_device_id *d)
{
	struct dfufdsc *dfufdsc;
	struct dfu_device *dfudev;
	int retv, index, dfufdsc_len;

	retv = 0;
	dfufdsc = (struct dfufdsc *)intf->cur_altsetting->extra;
	dfufdsc_len = intf->cur_altsetting->extralen;
	if (!dfufdsc || dfufdsc_len != USB_DFU_FUNC_DSCLEN ||
			dfufdsc->dsctyp != USB_DFU_FUNC_DSCTYP) {
		dev_err(&intf->dev, "Invalid DFU functional descriptor\n");
		return -ENODEV;
	}
	index = atomic_inc_return(&dfu_index);
	if (index >= max_dfus) {
		retv = -ENODEV;
		dev_err(&intf->dev, "Maximum supported USB DFU reached: %d\n",
			max_dfus);
		goto err_10;
	}

	*dfudevp = kmalloc(sizeof(struct dfu_device), GFP_KERNEL);
	if (!*dfudevp) {
		retv = -ENOMEM;
		goto err_10;
	}
	dfudev = *dfudevp;
	dfudev->index = index;
	dfudev->attr = dfufdsc->attr;
	dfudev->dettmout = le16_to_cpu(dfufdsc->tmout);
	dfudev->xfersize = le16_to_cpu(dfufdsc->xfersize);
	dfudev->intf = intf;
	dfudev->usbdev = interface_to_usbdev(intf);
	dfudev->intfnum = intf->cur_altsetting->desc.bInterfaceNumber;
	dfudev->devnum = MKDEV(MAJOR(dfu_devnum), index);
	if (dfudev->usbdev->bus->controller->dma_mask)
		dfudev->dma = 1;
	else
		dfudev->dma = 0;
	mutex_init(&dfudev->dfulock);
	usb_set_intfdata(intf, dfudev);
	
	return retv;

err_10:
	atomic_dec(&dfu_index);
	return retv;
}
EXPORT_SYMBOL_GPL(dfu_prepare);

void dfu_cleanup(struct usb_interface *intf, struct dfu_device *dfudev)
{
	usb_set_intfdata(intf, NULL);
	kfree(dfudev);
	atomic_dec(&dfu_index);
}
EXPORT_SYMBOL_GPL(dfu_cleanup);

int dfu_abort(struct dfu_device *dfudev, struct dfu_control *ctrl)
{
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_ABORT;
	ctrl->req.wValue = 0;
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(dfudev->usbdev, 0);
	ctrl->buff = NULL;
	ctrl->len = 0;
	ctrl->urb = NULL;
	return dfu_submit_urb(dfudev, ctrl);
}
EXPORT_SYMBOL_GPL(dfu_abort);

int dfu_get_status(const struct dfu_device *dfudev, struct dfu_control *ctrl)
{
	int retv;

	ctrl->req.bRequestType = 0xa1;
	ctrl->req.bRequest = USB_DFU_GETSTATUS;
	ctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = cpu_to_le16(6);
	ctrl->pipe = usb_rcvctrlpipe(dfudev->usbdev, 0);
	ctrl->buff = &ctrl->dfuStatus;
	ctrl->len = 6;
	ctrl->urb = NULL;
	retv = dfu_submit_urb(dfudev, ctrl);

	return retv;
}
EXPORT_SYMBOL_GPL(dfu_get_status);

int dfu_get_state(const struct dfu_device *dfudev, struct dfu_control *ctrl)
{
	int retv;

	ctrl->req.bRequestType = 0xa1;
	ctrl->req.bRequest = USB_DFU_GETSTATE;
	ctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = cpu_to_le16(1);
	ctrl->pipe = usb_rcvctrlpipe(dfudev->usbdev, 0);
	ctrl->buff = &ctrl->dfuState;
	ctrl->len = 1;
	ctrl->urb = NULL;
	retv = dfu_submit_urb(dfudev, ctrl);
	if (retv == 0)
		retv = ctrl->dfuState;
	return retv;
}
EXPORT_SYMBOL_GPL(dfu_get_state);

int dfu_clr_status(const struct dfu_device *dfudev, struct dfu_control *ctrl)
{
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_CLRSTATUS;
	ctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(dfudev->usbdev, 0);
	ctrl->buff = NULL;
	ctrl->len = 0;
	ctrl->urb = NULL;
	return dfu_submit_urb(dfudev, ctrl);
}
EXPORT_SYMBOL_GPL(dfu_clr_status);
