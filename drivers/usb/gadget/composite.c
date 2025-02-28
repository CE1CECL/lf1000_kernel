/*
 * composite.c - infrastructure for Composite USB Gadgets
 *
 * Copyright (C) 2006-2008 David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* #define VERBOSE_DEBUG */

#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>

#include <linux/usb/composite.h>


/*
 * The code in this file is utility code, used to build a gadget driver
 * from one or more "function" drivers, one or more "configuration"
 * objects, and a "usb_composite_driver" by gluing them together along
 * with the relevant device-wide data.
 */

/* big enough to hold our biggest descriptor */
#define USB_BUFSIZ	512

static struct usb_composite_driver *composite;

/* Some systems will need runtime overrides for the  product identifers
 * published in the device descriptor, either numbers or strings or both.
 * String parameters are in UTF-8 (superset of ASCII's 7 bit characters).
 */

static ushort idVendor;
module_param(idVendor, ushort, 0);
MODULE_PARM_DESC(idVendor, "USB Vendor ID");

static ushort idProduct;
module_param(idProduct, ushort, 0);
MODULE_PARM_DESC(idProduct, "USB Product ID");

static ushort bcdDevice;
module_param(bcdDevice, ushort, 0);
MODULE_PARM_DESC(bcdDevice, "USB Device version (BCD)");

static char *iManufacturer;
module_param(iManufacturer, charp, 0);
MODULE_PARM_DESC(iManufacturer, "USB Manufacturer string");

static char *iProduct;
module_param(iProduct, charp, 0);
MODULE_PARM_DESC(iProduct, "USB Product string");

static char *iSerialNumber;
module_param(iSerialNumber, charp, 0);
MODULE_PARM_DESC(iSerialNumber, "SerialNumber string");

/*-------------------------------------------------------------------------*/

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usb_function *f = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", !f->hidden);
}

static ssize_t enable_store(
		struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct usb_function *f = dev_get_drvdata(dev);
	struct usb_composite_driver	*driver = f->config->cdev->driver;
	int value;

	sscanf(buf, "%d", &value);
	if (driver->enable_function)
		driver->enable_function(f, value);
	else
		f->hidden = !value;

	return size;
}

static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR, enable_show, enable_store);


/**
 * usb_add_function() - add a function to a configuration
 * @config: the configuration
 * @function: the function being added
 * Context: single threaded during gadget setup
 *
 * After initialization, each configuration must have one or more
 * functions added to it.  Adding a function involves calling its @bind()
 * method to allocate resources such as interface and string identifiers
 * and endpoints.
 *
 * This function returns the value of the function's bind(), which is
 * zero for success else a negative errno value.
 */
int __init usb_add_function(struct usb_configuration *config,
		struct usb_function *function)
{
	struct usb_composite_dev	*cdev = config->cdev;
	int	value = -EINVAL;
	int index;

	DBG(cdev, "adding '%s'/%p to config '%s'/%p\n",
			function->name, function,
			config->label, config);

	if (!function->set_alt || !function->disable)
		goto done;

	index = atomic_inc_return(&cdev->driver->function_count);
	function->dev = device_create(cdev->driver->class, NULL,
		MKDEV(0, index), NULL, function->name);
	if (IS_ERR(function->dev))
		return PTR_ERR(function->dev);

	value = device_create_file(function->dev, &dev_attr_enable);
	if (value < 0) {
		device_destroy(cdev->driver->class, MKDEV(0, index));
		return value;
	}
	dev_set_drvdata(function->dev, function);

	function->config = config;
	list_add_tail(&function->list, &config->functions);

	/* REVISIT *require* function->bind? */
	if (function->bind) {
		value = function->bind(config, function);
		if (value < 0) {
			list_del(&function->list);
			function->config = NULL;
		}
	} else
		value = 0;

	/* We allow configurations that don't work at both speeds.
	 * If we run into a lowspeed Linux system, treat it the same
	 * as full speed ... it's the function drivers that will need
	 * to avoid bulk and ISO transfers.
	 */
	if (!config->fullspeed && function->descriptors)
		config->fullspeed = true;
	if (!config->highspeed && function->hs_descriptors)
		config->highspeed = true;

done:
	if (value)
		DBG(cdev, "adding '%s'/%p --> %d\n",
				function->name, function, value);
	return value;
}

/**
 * usb_function_deactivate - prevent function and gadget enumeration
 * @function: the function that isn't yet ready to respond
 *
 * Blocks response of the gadget driver to host enumeration by
 * preventing the data line pullup from being activated.  This is
 * normally called during @bind() processing to change from the
 * initial "ready to respond" state, or when a required resource
 * becomes available.
 *
 * For example, drivers that serve as a passthrough to a userspace
 * daemon can block enumeration unless that daemon (such as an OBEX,
 * MTP, or print server) is ready to handle host requests.
 *
 * Not all systems support software control of their USB peripheral
 * data pullups.
 *
 * Returns zero on success, else negative errno.
 */
int usb_function_deactivate(struct usb_function *function)
{
	struct usb_composite_dev	*cdev = function->config->cdev;
	unsigned long			flags;
	int				status = 0;

	spin_lock_irqsave(&cdev->lock, flags);

	if (cdev->deactivations == 0)
		status = usb_gadget_disconnect(cdev->gadget);
	if (status == 0)
		cdev->deactivations++;

	spin_unlock_irqrestore(&cdev->lock, flags);
	return status;
}

/**
 * usb_function_activate - allow function and gadget enumeration
 * @function: function on which usb_function_activate() was called
 *
 * Reverses effect of usb_function_deactivate().  If no more functions
 * are delaying their activation, the gadget driver will respond to
 * host enumeration procedures.
 *
 * Returns zero on success, else negative errno.
 */
int usb_function_activate(struct usb_function *function)
{
	struct usb_composite_dev	*cdev = function->config->cdev;
	int				status = 0;

	spin_lock(&cdev->lock);

	if (WARN_ON(cdev->deactivations == 0))
		status = -EINVAL;
	else {
		cdev->deactivations--;
		if (cdev->deactivations == 0)
			status = usb_gadget_connect(cdev->gadget);
	}

	spin_unlock(&cdev->lock);
	return status;
}

/**
 * usb_interface_id() - allocate an unused interface ID
 * @config: configuration associated with the interface
 * @function: function handling the interface
 * Context: single threaded during gadget setup
 *
 * usb_interface_id() is called from usb_function.bind() callbacks to
 * allocate new interface IDs.  The function driver will then store that
 * ID in interface, association, CDC union, and other descriptors.  It
 * will also handle any control requests targetted at that interface,
 * particularly changing its altsetting via set_alt().  There may
 * also be class-specific or vendor-specific requests to handle.
 *
 * All interface identifier should be allocated using this routine, to
 * ensure that for example different functions don't wrongly assign
 * different meanings to the same identifier.  Note that since interface
 * identifers are configuration-specific, functions used in more than
 * one configuration (or more than once in a given configuration) need
 * multiple versions of the relevant descriptors.
 *
 * Returns the interface ID which was allocated; or -ENODEV if no
 * more interface IDs can be allocated.
 */
int __init usb_interface_id(struct usb_configuration *config,
		struct usb_function *function)
{
	unsigned id = config->next_interface_id;

	if (id < MAX_CONFIG_INTERFACES) {
		config->interface[id] = function;
		config->next_interface_id = id + 1;
		return id;
	}
	return -ENODEV;
}

static int config_buf(struct usb_configuration *config,
		enum usb_device_speed speed, void *buf, u8 type)
{
	struct usb_config_descriptor	*c = buf;
	struct usb_interface_descriptor *intf;
	void				*next = buf + USB_DT_CONFIG_SIZE;
	int				len = USB_BUFSIZ - USB_DT_CONFIG_SIZE;
	struct usb_function		*f;
	int				status;
	int				interfaceCount = 0;
	u8 *dest;

	/* write the config descriptor */
	c = buf;
	c->bLength = USB_DT_CONFIG_SIZE;
	c->bDescriptorType = type;
	/* wTotalLength and bNumInterfaces are written later */
	c->bConfigurationValue = config->bConfigurationValue;
	c->iConfiguration = config->iConfiguration;
	c->bmAttributes = USB_CONFIG_ATT_ONE | config->bmAttributes;
	c->bMaxPower = config->bMaxPower ? : (CONFIG_USB_GADGET_VBUS_DRAW / 2);

	/* There may be e.g. OTG descriptors */
	if (config->descriptors) {
		status = usb_descriptor_fillbuf(next, len,
				config->descriptors);
		if (status < 0)
			return status;
		len -= status;
		next += status;
	}

	/* add each function's descriptors */
	list_for_each_entry(f, &config->functions, list) {
		struct usb_descriptor_header **descriptors;
		struct usb_descriptor_header *descriptor;

		if (speed == USB_SPEED_HIGH)
			descriptors = f->hs_descriptors;
		else
			descriptors = f->descriptors;
		if (f->hidden || !descriptors || descriptors[0] == NULL)
			continue;
		status = usb_descriptor_fillbuf(next, len,
			(const struct usb_descriptor_header **) descriptors);
		if (status < 0)
			return status;

		/* set interface numbers dynamically */
		dest = next;
		while ((descriptor = *descriptors++) != NULL) {
			intf = (struct usb_interface_descriptor *)dest;
			if (intf->bDescriptorType == USB_DT_INTERFACE)
				intf->bInterfaceNumber = interfaceCount++;
			dest += intf->bLength;
		}

		len -= status;
		next += status;
	}

	len = next - buf;
	c->wTotalLength = cpu_to_le16(len);
	c->bNumInterfaces = interfaceCount;
	return len;
}

static int config_desc(struct usb_composite_dev *cdev, unsigned w_value)
{
	struct usb_gadget		*gadget = cdev->gadget;
	struct usb_configuration	*c;
	u8				type = w_value >> 8;
	enum usb_device_speed		speed = USB_SPEED_UNKNOWN;

	if (gadget_is_dualspeed(gadget)) {
		int			hs = 0;

		if (gadget->speed == USB_SPEED_HIGH)
			hs = 1;
		if (type == USB_DT_OTHER_SPEED_CONFIG)
			hs = !hs;
		if (hs)
			speed = USB_SPEED_HIGH;

	}

	/* This is a lookup by config *INDEX* */
	w_value &= 0xff;
	list_for_each_entry(c, &cdev->configs, list) {
		/* ignore configs that won't work at this speed */
		if (speed == USB_SPEED_HIGH) {
			if (!c->highspeed)
				continue;
		} else {
			if (!c->fullspeed)
				continue;
		}
		if (w_value == 0)
			return config_buf(c, speed, cdev->req->buf, type);
		w_value--;
	}
	return -EINVAL;
}

static int count_configs(struct usb_composite_dev *cdev, unsigned type)
{
	struct usb_gadget		*gadget = cdev->gadget;
	struct usb_configuration	*c;
	unsigned			count = 0;
	int				hs = 0;

	if (gadget_is_dualspeed(gadget)) {
		if (gadget->speed == USB_SPEED_HIGH)
			hs = 1;
		if (type == USB_DT_DEVICE_QUALIFIER)
			hs = !hs;
	}
	list_for_each_entry(c, &cdev->configs, list) {
		/* ignore configs that won't work at this speed */
		if (hs) {
			if (!c->highspeed)
				continue;
		} else {
			if (!c->fullspeed)
				continue;
		}
		count++;
	}
	return count;
}

static void device_qual(struct usb_composite_dev *cdev)
{
	struct usb_qualifier_descriptor	*qual = cdev->req->buf;

	qual->bLength = sizeof(*qual);
	qual->bDescriptorType = USB_DT_DEVICE_QUALIFIER;
	/* POLICY: same bcdUSB and device type info at both speeds */
	qual->bcdUSB = cdev->desc.bcdUSB;
	qual->bDeviceClass = cdev->desc.bDeviceClass;
	qual->bDeviceSubClass = cdev->desc.bDeviceSubClass;
	qual->bDeviceProtocol = cdev->desc.bDeviceProtocol;
	/* ASSUME same EP0 fifo size at both speeds */
	qual->bMaxPacketSize0 = cdev->desc.bMaxPacketSize0;
	qual->bNumConfigurations = count_configs(cdev, USB_DT_DEVICE_QUALIFIER);
	qual->bRESERVED = 0;
}

/*-------------------------------------------------------------------------*/

static void reset_config(struct usb_composite_dev *cdev)
{
	struct usb_function		*f;

	DBG(cdev, "reset config\n");

	list_for_each_entry(f, &cdev->config->functions, list) {
		if (f->disable)
			f->disable(f);
	}
	cdev->config = NULL;
}

static int set_config(struct usb_composite_dev *cdev,
		const struct usb_ctrlrequest *ctrl, unsigned number)
{
	struct usb_gadget	*gadget = cdev->gadget;
	struct usb_configuration *c = NULL;
	int			result = -EINVAL;
	unsigned		power = gadget_is_otg(gadget) ? 8 : 100;
	int			tmp;

	if (cdev->config)
		reset_config(cdev);

	if (number) {
		list_for_each_entry(c, &cdev->configs, list) {
			if (c->bConfigurationValue == number) {
				result = 0;
				break;
			}
		}
		if (result < 0)
			goto done;
	} else
		result = 0;

	INFO(cdev, "%s speed config #%d: %s\n",
		({ char *speed;
		switch (gadget->speed) {
		case USB_SPEED_LOW:	speed = "low"; break;
		case USB_SPEED_FULL:	speed = "full"; break;
		case USB_SPEED_HIGH:	speed = "high"; break;
		default:		speed = "?"; break;
		} ; speed; }), number, c ? c->label : "unconfigured");

	if (!c)
		goto done;

	cdev->config = c;

	/* Initialize all interfaces by setting them to altsetting zero. */
	for (tmp = 0; tmp < MAX_CONFIG_INTERFACES; tmp++) {
		struct usb_function	*f = c->interface[tmp];

		if (!f)
			break;

		result = f->set_alt(f, tmp, 0);
		if (result < 0) {
			DBG(cdev, "interface %d (%s/%p) alt 0 --> %d\n",
					tmp, f->name, f, result);

			reset_config(cdev);
			goto done;
		}
	}

	/* when we return, be sure our power usage is valid */
	power = c->bMaxPower ? (2 * c->bMaxPower) : CONFIG_USB_GADGET_VBUS_DRAW;
done:
	usb_gadget_vbus_draw(gadget, power);
	return result;
}

/**
 * usb_add_config() - add a configuration to a device.
 * @cdev: wraps the USB gadget
 * @config: the configuration, with bConfigurationValue assigned
 * Context: single threaded during gadget setup
 *
 * One of the main tasks of a composite driver's bind() routine is to
 * add each of the configurations it supports, using this routine.
 *
 * This function returns the value of the configuration's bind(), which
 * is zero for success else a negative errno value.  Binding configurations
 * assigns global resources including string IDs, and per-configuration
 * resources such as interface IDs and endpoints.
 */
int __init usb_add_config(struct usb_composite_dev *cdev,
		struct usb_configuration *config)
{
	int				status = -EINVAL;
	struct usb_configuration	*c;

	DBG(cdev, "adding config #%u '%s'/%p\n",
			config->bConfigurationValue,
			config->label, config);

	if (!config->bConfigurationValue || !config->bind)
		goto done;

	/* Prevent duplicate configuration identifiers */
	list_for_each_entry(c, &cdev->configs, list) {
		if (c->bConfigurationValue == config->bConfigurationValue) {
			status = -EBUSY;
			goto done;
		}
	}

	config->cdev = cdev;
	list_add_tail(&config->list, &cdev->configs);

	INIT_LIST_HEAD(&config->functions);
	config->next_interface_id = 0;

	status = config->bind(config);
	if (status < 0) {
		list_del(&config->list);
		config->cdev = NULL;
	} else {
		unsigned	i;

		DBG(cdev, "cfg %d/%p speeds:%s%s\n",
			config->bConfigurationValue, config,
			config->highspeed ? " high" : "",
			config->fullspeed
				? (gadget_is_dualspeed(cdev->gadget)
					? " full"
					: " full/low")
				: "");

		for (i = 0; i < MAX_CONFIG_INTERFACES; i++) {
			struct usb_function	*f = config->interface[i];

			if (!f)
				continue;
			DBG(cdev, "  interface %d = %s/%p\n",
				i, f->name, f);
		}
	}

	/* set_alt(), or next config->bind(), sets up
	 * ep->driver_data as needed.
	 */
	usb_ep_autoconfig_reset(cdev->gadget);

done:
	if (status)
		DBG(cdev, "added config '%s'/%u --> %d\n", config->label,
				config->bConfigurationValue, status);
	return status;
}

/*-------------------------------------------------------------------------*/

/* We support strings in multiple languages ... string descriptor zero
 * says which languages are supported.  The typical case will be that
 * only one language (probably English) is used, with I18N handled on
 * the host side.
 */

static void collect_langs(struct usb_gadget_strings **sp, __le16 *buf)
{
	const struct usb_gadget_strings	*s;
	u16				language;
	__le16				*tmp;

	while (*sp) {
		s = *sp;
		language = cpu_to_le16(s->language);
		for (tmp = buf; *tmp && tmp < &buf[126]; tmp++) {
			if (*tmp == language)
				goto repeat;
		}
		*tmp++ = language;
repeat:
		sp++;
	}
}

static int lookup_string(
	struct usb_gadget_strings	**sp,
	void				*buf,
	u16				language,
	int				id
)
{
	struct usb_gadget_strings	*s;
	int				value;

	while (*sp) {
		s = *sp++;
		if (s->language != language)
			continue;
		value = usb_gadget_get_string(s, id, buf);
		if (value > 0)
			return value;
	}
	return -EINVAL;
}

static int get_string(struct usb_composite_dev *cdev,
		void *buf, u16 language, int id)
{
	struct usb_configuration	*c;
	struct usb_function		*f;
	int				len;

	/* Yes, not only is USB's I18N support probably more than most
	 * folk will ever care about ... also, it's all supported here.
	 * (Except for UTF8 support for Unicode's "Astral Planes".)
	 */

	/* 0 == report all available language codes */
	if (id == 0) {
		struct usb_string_descriptor	*s = buf;
		struct usb_gadget_strings	**sp;

		memset(s, 0, 256);
		s->bDescriptorType = USB_DT_STRING;

		sp = composite->strings;
		if (sp)
			collect_langs(sp, s->wData);

		list_for_each_entry(c, &cdev->configs, list) {
			sp = c->strings;
			if (sp)
				collect_langs(sp, s->wData);

			list_for_each_entry(f, &c->functions, list) {
				sp = f->strings;
				if (sp)
					collect_langs(sp, s->wData);
			}
		}

		for (len = 0; s->wData[len] && len <= 126; len++)
			continue;
		if (!len)
			return -EINVAL;

		s->bLength = 2 * (len + 1);
		return s->bLength;
	}

	/* Otherwise, look up and return a specified string.  String IDs
	 * are device-scoped, so we look up each string table we're told
	 * about.  These lookups are infrequent; simpler-is-better here.
	 */
	if (composite->strings) {
		len = lookup_string(composite->strings, buf, language, id);
		if (len > 0)
			return len;
	}
	list_for_each_entry(c, &cdev->configs, list) {
		if (c->strings) {
			len = lookup_string(c->strings, buf, language, id);
			if (len > 0)
				return len;
		}
		list_for_each_entry(f, &c->functions, list) {
			if (!f->strings)
				continue;
			len = lookup_string(f->strings, buf, language, id);
			if (len > 0)
				return len;
		}
	}
	return -EINVAL;
}

/**
 * usb_string_id() - allocate an unused string ID
 * @cdev: the device whose string descriptor IDs are being allocated
 * Context: single threaded during gadget setup
 *
 * @usb_string_id() is called from bind() callbacks to allocate
 * string IDs.  Drivers for functions, configurations, or gadgets will
 * then store that ID in the appropriate descriptors and string table.
 *
 * All string identifier should be allocated using this routine, to
 * ensure that for example different functions don't wrongly assign
 * different meanings to the same identifier.
 */
int __init usb_string_id(struct usb_composite_dev *cdev)
{
	if (cdev->next_string_id < 254) {
		/* string id 0 is reserved */
		cdev->next_string_id++;
		return cdev->next_string_id;
	}
	return -ENODEV;
}

/*-------------------------------------------------------------------------*/

static void composite_setup_complete(struct usb_ep *ep, struct usb_request *req)
{
	if (req->status || req->actual != req->length)
		DBG((struct usb_composite_dev *) ep->driver_data,
				"setup complete --> %d, %d/%d\n",
				req->status, req->actual, req->length);
}

/*
 * The setup() callback implements all the ep0 functionality that's
 * not handled lower down, in hardware or the hardware driver(like
 * device and endpoint feature flags, and their status).  It's all
 * housekeeping for the gadget function we're implementing.  Most of
 * the work is in config and function specific setup.
 */
static int
composite_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	struct usb_request		*req = cdev->req;
	int				value = -EOPNOTSUPP;
	u16				w_index = le16_to_cpu(ctrl->wIndex);
	u8				intf = w_index & 0xFF;
	u16				w_value = le16_to_cpu(ctrl->wValue);
	u16				w_length = le16_to_cpu(ctrl->wLength);
	struct usb_function		*f = NULL;

	/* partial re-init of the response message; the function or the
	 * gadget might need to intercept e.g. a control-OUT completion
	 * when we delegate to it.
	 */
	req->zero = 0;
	req->complete = composite_setup_complete;
	req->length = USB_BUFSIZ;
	gadget->ep0->driver_data = cdev;

	switch (ctrl->bRequest) {

	/* we handle all standard USB descriptors */
	case USB_REQ_GET_DESCRIPTOR:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unknown;
		switch (w_value >> 8) {

		case USB_DT_DEVICE:
			cdev->desc.bNumConfigurations =
				count_configs(cdev, USB_DT_DEVICE);
			value = min(w_length, (u16) sizeof cdev->desc);
			memcpy(req->buf, &cdev->desc, value);
			break;
		case USB_DT_DEVICE_QUALIFIER:
			if (!gadget_is_dualspeed(gadget))
				break;
			device_qual(cdev);
			value = min_t(int, w_length,
				sizeof(struct usb_qualifier_descriptor));
			break;
		case USB_DT_OTHER_SPEED_CONFIG:
			if (!gadget_is_dualspeed(gadget))
				break;
			/* FALLTHROUGH */
		case USB_DT_CONFIG:
			value = config_desc(cdev, w_value);
			if (value >= 0)
				value = min(w_length, (u16) value);
			break;
		case USB_DT_STRING:
			value = get_string(cdev, req->buf,
					w_index, w_value & 0xff);
			if (value >= 0)
				value = min(w_length, (u16) value);
			break;
		}
		break;

	/* any number of configs can work */
	case USB_REQ_SET_CONFIGURATION:
		if (ctrl->bRequestType != 0)
			goto unknown;
		if (gadget_is_otg(gadget)) {
			if (gadget->a_hnp_support)
				DBG(cdev, "HNP available\n");
			else if (gadget->a_alt_hnp_support)
				DBG(cdev, "HNP on another port\n");
			else
				VDBG(cdev, "HNP inactive\n");
		}
		spin_lock(&cdev->lock);
		value = set_config(cdev, ctrl, w_value);
		spin_unlock(&cdev->lock);
		break;
	case USB_REQ_GET_CONFIGURATION:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unknown;
		if (cdev->config) {
			*(u8 *)req->buf = cdev->config->bConfigurationValue;
			value = min(w_length, (u16) 1);
		} else
			*(u8 *)req->buf = 0;
		break;

	/* function drivers must handle get/set altsetting; if there's
	 * no get() method, we know only altsetting zero works.
	 */
	case USB_REQ_SET_INTERFACE:
		if (ctrl->bRequestType != USB_RECIP_INTERFACE)
			goto unknown;
		if (!cdev->config || w_index >= MAX_CONFIG_INTERFACES)
			break;
		f = cdev->config->interface[intf];
		if (!f)
			break;
		if (w_value && !f->set_alt)
			break;
		value = f->set_alt(f, w_index, w_value);
		break;
	case USB_REQ_GET_INTERFACE:
		if (ctrl->bRequestType != (USB_DIR_IN|USB_RECIP_INTERFACE))
			goto unknown;
		if (!cdev->config || w_index >= MAX_CONFIG_INTERFACES)
			break;
		f = cdev->config->interface[intf];
		if (!f)
			break;
		/* lots of interfaces only need altsetting zero... */
		value = f->get_alt ? f->get_alt(f, w_index) : 0;
		if (value < 0)
			break;
		*((u8 *)req->buf) = value;
		value = min(w_length, (u16) 1);
		break;
	default:
unknown:
		VDBG(cdev,
			"non-core control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);

		/* functions always handle their interfaces ... punt other
		 * recipients (endpoint, other, WUSB, ...) to the current
		 * configuration code.
		 *
		 * REVISIT it could make sense to let the composite device
		 * take such requests too, if that's ever needed:  to work
		 * in config 0, etc.
		 */
		if ((ctrl->bRequestType & USB_RECIP_MASK)
				== USB_RECIP_INTERFACE) {
			if (cdev->config == NULL)
				return value;

			f = cdev->config->interface[intf];
			if (f && f->setup)
				value = f->setup(f, ctrl);
			else
				f = NULL;
		}
		if (value < 0 && !f) {
			struct usb_configuration	*c;

			c = cdev->config;
			if (c && c->setup)
				value = c->setup(c, ctrl);
		}

		/* If the vendor request is not processed (value < 0),
		 * call all device registered configure setup callbacks
		 * to process it.
		 * This is used to handle the following cases:
		 * - vendor request is for the device and arrives before
		 * setconfiguration.
		 * - Some devices are required to handle vendor request before
		 * setconfiguration such as MTP, USBNET.
		 */

		if (value < 0) {
			struct usb_configuration        *cfg;

			list_for_each_entry(cfg, &cdev->configs, list) {
			if (cfg && cfg->setup)
				value = cfg->setup(cfg, ctrl);
			}
		}

		goto done;
	}

	/* respond with data transfer before status phase? */
	if (value >= 0) {
		req->length = value;
		req->zero = value < w_length;
		value = usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			DBG(cdev, "ep_queue --> %d\n", value);
			req->status = 0;
			composite_setup_complete(gadget->ep0, req);
		}
	}

done:
	/* device either stalls (value < 0) or reports success */
	return value;
}

static void composite_disconnect(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	unsigned long			flags;

	/* REVISIT:  should we have config and device level
	 * disconnect callbacks?
	 */
	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config)
		reset_config(cdev);
	spin_unlock_irqrestore(&cdev->lock, flags);
}

/*-------------------------------------------------------------------------*/

static void /* __init_or_exit */
composite_unbind(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);

	/* composite_disconnect() must already have been called
	 * by the underlying peripheral controller driver!
	 * so there's no i/o concurrency that could affect the
	 * state protected by cdev->lock.
	 */
	WARN_ON(cdev->config);

	while (!list_empty(&cdev->configs)) {
		struct usb_configuration	*c;

		c = list_first_entry(&cdev->configs,
				struct usb_configuration, list);
		while (!list_empty(&c->functions)) {
			struct usb_function		*f;

			f = list_first_entry(&c->functions,
					struct usb_function, list);
			list_del(&f->list);
			if (f->unbind) {
				DBG(cdev, "unbind function '%s'/%p\n",
						f->name, f);
				f->unbind(c, f);
				/* may free memory for "f" */
			}
		}
		list_del(&c->list);
		if (c->unbind) {
			DBG(cdev, "unbind config '%s'/%p\n", c->label, c);
			c->unbind(c);
			/* may free memory for "c" */
		}
	}
	if (composite->unbind)
		composite->unbind(cdev);

	if (cdev->req) {
		kfree(cdev->req->buf);
		usb_ep_free_request(gadget->ep0, cdev->req);
	}
	kfree(cdev);
	set_gadget_data(gadget, NULL);
	composite = NULL;
}

static void __init
string_override_one(struct usb_gadget_strings *tab, u8 id, const char *s)
{
	struct usb_string		*str = tab->strings;

	for (str = tab->strings; str->s; str++) {
		if (str->id == id) {
			str->s = s;
			return;
		}
	}
}

static void __init
string_override(struct usb_gadget_strings **tab, u8 id, const char *s)
{
	while (*tab) {
		string_override_one(*tab, id, s);
		tab++;
	}
}

static int __init composite_bind(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev;
	int				status = -ENOMEM;

	cdev = kzalloc(sizeof *cdev, GFP_KERNEL);
	if (!cdev)
		return status;

	spin_lock_init(&cdev->lock);
	cdev->gadget = gadget;
	set_gadget_data(gadget, cdev);
	INIT_LIST_HEAD(&cdev->configs);

	/* preallocate control response and buffer */
	cdev->req = usb_ep_alloc_request(gadget->ep0, GFP_KERNEL);
	if (!cdev->req)
		goto fail;
	cdev->req->buf = kmalloc(USB_BUFSIZ, GFP_KERNEL);
	if (!cdev->req->buf)
		goto fail;
	cdev->req->complete = composite_setup_complete;
	gadget->ep0->driver_data = cdev;

	cdev->bufsiz = USB_BUFSIZ;
	cdev->driver = composite;

	usb_gadget_set_selfpowered(gadget);

	/* interface and string IDs start at zero via kzalloc.
	 * we force endpoints to start unassigned; few controller
	 * drivers will zero ep->driver_data.
	 */
	usb_ep_autoconfig_reset(cdev->gadget);

	/* composite gadget needs to assign strings for whole device (like
	 * serial number), register function drivers, potentially update
	 * power state and consumption, etc
	 */
	status = composite->bind(cdev);
	if (status < 0)
		goto fail;

	cdev->desc = *composite->dev;
	cdev->desc.bMaxPacketSize0 = gadget->ep0->maxpacket;

	/* standardized runtime overrides for device ID data */
	if (idVendor)
		cdev->desc.idVendor = cpu_to_le16(idVendor);
	if (idProduct)
		cdev->desc.idProduct = cpu_to_le16(idProduct);
	if (bcdDevice)
		cdev->desc.bcdDevice = cpu_to_le16(bcdDevice);

	/* strings can't be assigned before bind() allocates the
	 * releavnt identifiers
	 */
	if (cdev->desc.iManufacturer && iManufacturer)
		string_override(composite->strings,
			cdev->desc.iManufacturer, iManufacturer);
	if (cdev->desc.iProduct && iProduct)
		string_override(composite->strings,
			cdev->desc.iProduct, iProduct);
	if (cdev->desc.iSerialNumber && iSerialNumber)
		string_override(composite->strings,
			cdev->desc.iSerialNumber, iSerialNumber);

	INFO(cdev, "%s ready\n", composite->name);
	return 0;

fail:
	composite_unbind(gadget);
	return status;
}

/*-------------------------------------------------------------------------*/

static void
composite_suspend(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	struct usb_function		*f;

	/* REVISIT:  should we have config level
	 * suspend/resume callbacks?
	 */
	DBG(cdev, "suspend\n");
	if (cdev->config) {
		list_for_each_entry(f, &cdev->config->functions, list) {
			if (f->suspend)
				f->suspend(f);
		}
	}
	if (composite->suspend)
		composite->suspend(cdev);
}

static void
composite_resume(struct usb_gadget *gadget)
{
	struct usb_composite_dev	*cdev = get_gadget_data(gadget);
	struct usb_function		*f;

	/* REVISIT:  should we have config level
	 * suspend/resume callbacks?
	 */
	DBG(cdev, "resume\n");
	if (composite->resume)
		composite->resume(cdev);
	if (cdev->config) {
		list_for_each_entry(f, &cdev->config->functions, list) {
			if (f->resume)
				f->resume(f);
		}
	}
}

/*-------------------------------------------------------------------------*/

static struct usb_gadget_driver composite_driver = {
	.speed		= USB_SPEED_HIGH,

	.bind		= composite_bind,
	.unbind		= __exit_p(composite_unbind),

	.setup		= composite_setup,
	.disconnect	= composite_disconnect,

	.suspend	= composite_suspend,
	.resume		= composite_resume,

	.driver	= {
		.owner		= THIS_MODULE,
	},
};

/**
 * usb_composite_register() - register a composite driver
 * @driver: the driver to register
 * Context: single threaded during gadget setup
 *
 * This function is used to register drivers using the composite driver
 * framework.  The return value is zero, or a negative errno value.
 * Those values normally come from the driver's @bind method, which does
 * all the work of setting up the driver to match the hardware.
 *
 * On successful return, the gadget is ready to respond to requests from
 * the host, unless one of its components invokes usb_gadget_disconnect()
 * while it was binding.  That would usually be done in order to wait for
 * some userspace participation.
 */
int __init usb_composite_register(struct usb_composite_driver *driver)
{
	if (!driver || !driver->dev || !driver->bind || composite)
		return -EINVAL;

	if (!driver->name)
		driver->name = "composite";
	composite_driver.function =  (char *) driver->name;
	composite_driver.driver.name = driver->name;
	composite = driver;

	driver->class = class_create(THIS_MODULE, "usb_composite");
	if (IS_ERR(driver->class))
		return PTR_ERR(driver->class);

	return usb_gadget_register_driver(&composite_driver);
}

/**
 * usb_composite_unregister() - unregister a composite driver
 * @driver: the driver to unregister
 *
 * This function is used to unregister drivers using the composite
 * driver framework.
 */
void __exit usb_composite_unregister(struct usb_composite_driver *driver)
{
	if (composite != driver)
		return;
	usb_gadget_unregister_driver(&composite_driver);
}
