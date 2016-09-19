/*
Navcom SF 3050 Driver
Created by: Joseph Woolf
License: GNU GENERAL PUBLIC LICENSE 3.0.

Note:  This driver requires at least Linux 3.0 to run properly.
*/
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/usb.h>
#include <linux/usbdevice_fs.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>
#include <asm/spinlock.h>

#define VENDOR_ID 0x1c45
#define PRODUCT_ID 0x3050

#define to_skel_dev(d) container_of(d, struct navcom_3050_device, kref)
#define USB_SKEL_MINOR_BASE	192

//Make a prototype driver for the Navcom
static struct usb_driver nav_3050_device;

struct navcom_3050_device {
	struct usb_device * udev;
	struct usb_interface * interface;
	unsigned char * interrupt_in_buffer;
	__u8		interrupt_in_size;
	__u8		interrupt_in_endpointAddr;
	unsigned char * bulk_in_buffer;
	size_t		bulk_in_size;
	__u8		bulk_in_endpointAddr;
	__u8		bulk_out_endpointAddr;
	struct kref	kref;
};



static void navcom_3050_delete(struct kref *kref)
{
	struct navcom_3050_device *dev = to_skel_dev(kref);

	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}


static int navcom_3050_open(struct inode *inode, struct file *file)
{
	struct navcom_3050_device *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&nav_3050_device, subminor);
	if(!interface) {
		printk(KERN_ALERT "%s - error, can't find device for minor %d\n",__FUNCTION__,subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if(!dev) {
		retval = -ENODEV;
		goto exit;
	}

	kref_get(&dev->kref);

	file->private_data = dev;

exit:
	return retval;
}

static int navcom_3050_release(struct inode *inode, struct file *file)
{
	struct navcom_3050_device *dev;

	dev = (struct nav_3050_device *)file->private_data;
	if(dev == NULL)
		return -ENODEV;

	kref_put(&dev->kref, navcom_3050_delete);
	return 0;
}

static ssize_t navcom_3050_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	struct navcom_3050_device *dev;
	int retval = 0;

	dev = (struct nav_3050_device *)file->private_data;
	retval = usb_bulk_msg(dev->udev,
			      usb_rcvbulkpipe(dev->udev,dev->bulk_in_endpointAddr),
				dev->bulk_in_buffer,
				min(dev->bulk_in_size, count),
				&count, HZ*10);

	if (!retval) {
		if(copy_to_user(buffer, dev->bulk_in_buffer,count))
			retval = -EFAULT;
		else
			retval = count;
	}
	
	return retval;
}

static void navcom_3050_write_bulk_callback(struct urb *urb, struct pt_regs *regs)
{

	/* sync/async unlink faults aren't errors */
	if (urb->status && 
	    !(urb->status == -ENOENT || 
	      urb->status == -ECONNRESET ||
	      urb->status == -ESHUTDOWN)) {
		printk(KERN_INFO "%s - nonzero write bulk status received: %d",
		    __FUNCTION__, urb->status);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length, 
			urb->transfer_buffer, urb->transfer_dma);	
}

static ssize_t navcom_3050_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
	struct navcom_3050_device *dev;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;

	dev = (struct nav_3050_device *)file->private_data;

	if (count == 0)
		goto exit;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, count, GFP_KERNEL, &urb->transfer_dma);
	if(!buf) {
		retval = -ENOMEM;
		goto error;
	}
	if(copy_from_user(buf, user_buffer, count)) {
		retval = -EFAULT;
		goto error;
	}
	
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  buf, count, navcom_3050_write_bulk_callback, dev);

	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	retval = usb_submit_urb(urb,GFP_KERNEL);
	if(retval) {
		printk(KERN_ALERT "%s - failed submitting write urb, error %d", __FUNCTION__, retval);
		goto error;
	}

	usb_free_urb(urb);

exit:
	return count;

error:
	usb_free_coherent(dev->udev, count, buf, urb->transfer_dma);
	usb_free_urb(urb);
	kfree(buf);
	return retval;
}

static struct file_operations navcom_3050_fops = {
	//.owner =	THIS_MODULE,
	.read =		navcom_3050_read,
	.write =	navcom_3050_write,
	.open =		navcom_3050_open,
	.release =	navcom_3050_release,
};

static struct usb_class_driver navcom_3050_class_driver = {
	.name = "usb/navcom-SF-3050-%d",
	.fops = &navcom_3050_fops,
	//.mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH,
	.minor_base = USB_SKEL_MINOR_BASE,
};

/*
Function: navcom_3050_probe
*/
int navcom_3050_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct navcom_3050_device *dev = NULL;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	printk(KERN_INFO "Allocating %d bytes!\n", sizeof(struct navcom_3050_device));
	dev = kmalloc(sizeof(struct navcom_3050_device), GFP_KERNEL);
	if (dev == NULL) {
		printk(KERN_ALERT "Out of memory");
		goto error;
	}

	memset(dev, 0x00, sizeof(*dev));
	kref_init(&dev->kref);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;

	iface_desc = interface->cur_altsetting;
	
	for(i = 0; i < iface_desc->desc.bNumEndpoints; ++i){
		endpoint = &iface_desc->endpoint[i].desc;
		printk(KERN_INFO "Current endpoint's address is: %d\n",endpoint->bEndpointAddress);
		//Look for a bulk IN endpoint
		if(!dev->bulk_in_endpointAddr && 
		  ((endpoint->bEndpointAddress & USB_DIR_IN) != 0) &&
 		 ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)) {
			printk(KERN_INFO "This is a bulk in endpoint!\n");
			buffer_size = endpoint->wMaxPacketSize;
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			dev->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if(!dev->bulk_in_buffer) {
				printk(KERN_ALERT "Could not allocate bulk_in_buffer");
				goto error;
			}
		}

		//Look for a bulk OUT endpoint
		if(!dev->bulk_out_endpointAddr &&
		  !(endpoint->bEndpointAddress & USB_DIR_IN) &&
		  ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)) {
			printk(KERN_INFO "This is a bulk out endpoint!\n");
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
		}

		//Look for an interrupt IN endpoint
		if(!dev->interrupt_in_endpointAddr &&
		   (endpoint->bEndpointAddress & USB_DIR_IN) &&
		   ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)) {
			printk(KERN_INFO "This is an interrupt in endpoint!\n");
			buffer_size = endpoint->wMaxPacketSize;
			dev->interrupt_in_size = buffer_size;
			dev->interrupt_in_endpointAddr = endpoint->bEndpointAddress;
			dev->interrupt_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if(!dev->interrupt_in_buffer) {
				printk(KERN_ALERT "Could not allocate bulk_in_buffer");
				goto error;
			}
		}

	}
	printk(KERN_INFO "%d\n",iface_desc->desc.bInterfaceNumber);
	if(iface_desc->desc.bInterfaceNumber == 0)
	{
		usb_set_intfdata(interface, dev);
		return 0;

	}
		
	if(!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
		printk(KERN_ALERT "Could not find both bulk-in and bulk-out endpoints");
		goto error;
	}

	//device_create_file(dev,
	usb_set_intfdata(interface, dev);

	retval = usb_register_dev(interface, &navcom_3050_class_driver);
	if (retval) {
		printk(KERN_ALERT "Not able to get a minor for this device.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	printk(KERN_INFO "Probing success!\n");
	return 0;
	
	error:
	if(dev)
		kref_put(&dev->kref, navcom_3050_delete);
	printk(KERN_INFO "Probing failed!\n");
	return retval;
}

/*
Function: navcom_3050_disconnect

*/
void navcom_3050_disconnect(struct usb_interface *interface)
{
	struct navcom_3050_device *dev;
	int minor = interface->minor;

	//lock_kernel();

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);
	
	if (interface->cur_altsetting->desc.bInterfaceNumber == 0)
		return;
	usb_deregister_dev(interface, &navcom_3050_class_driver);

	//unlock_kernel();

	kref_put(&dev->kref, navcom_3050_delete);
	printk(KERN_INFO "disconnecting Navcom SF 3050!\n");
}


static struct usb_device_id navcom_table [] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
	{ },
};

static struct usb_driver nav_3050_device = {
	//.owner =	THIS_MODULE,
	.name =		"Navcom SF 3050",
	.probe =	navcom_3050_probe,
	.disconnect =	navcom_3050_disconnect,
	.id_table =	navcom_table,
};

MODULE_DEVICE_TABLE (usb, navcom_table);
/*
Function: navcom_3050_start
*/
static int __init navcom_3050_start(void)
{
	printk(KERN_INFO "Starting up Navcom SF-3050 driver!\n");

	int retval;
	retval = usb_register(&nav_3050_device);
	if (retval)
	{
        	printk(KERN_ALERT "usb_register failed. "
            		"Error number %d", retval);
		return retval;
	}
	else
	{
		printk(KERN_INFO "Navcom SF-3050 driver successfully set up!\n");
	}
	return 0;	
}

/*
Function: navcom_3050_end
*/
static void __exit navcom_3050_end(void)
{
	printk(KERN_INFO "Shutting down Navcom SF-3050 driver!\n");
	usb_deregister(&nav_3050_device);
}

module_init(navcom_3050_start);
module_exit(navcom_3050_end);

MODULE_LICENSE("GPL");
