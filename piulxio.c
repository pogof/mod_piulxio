/*
 * PIULXIO interface driver
 *
 * Copyright (C) 2025 
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This code is based on the PIUIO driver by Devin J. Pohly and adapted
 * for the newer PIULXIO board with menu buttons.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/sysfs.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/leds.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/usb/input.h>

/*
 * Device and protocol definitions
 */
#define USB_VENDOR_ID_LXIO 0x0D2F
#define USB_PRODUCT_ID_LXIO 0x1020

/* USB endpoints and message size */
#define LXIO_ENDPOINT_INPUT 0x81
#define LXIO_ENDPOINT_OUTPUT 0x02
#define LXIO_MSG_SZ 16
#define LXIO_MSG_LONGS (LXIO_MSG_SZ / sizeof(unsigned long))

/* Input keycode ranges */
#define LXIO_BTN_REG BTN_JOYSTICK
#define LXIO_NUM_REG (BTN_GAMEPAD - BTN_JOYSTICK)
#define LXIO_BTN_EXTRA BTN_TRIGGER_HAPPY
#define LXIO_NUM_EXTRA (KEY_MAX - BTN_TRIGGER_HAPPY)
#define LXIO_NUM_BTNS (LXIO_NUM_REG + LXIO_NUM_EXTRA)

/**
 * struct piulxio_led - auxiliary struct for led devices
 * @piu:	Pointer back to the enclosing structure
 * @dev:	Actual led device
 */
struct piulxio_led {
	struct piulxio *piu;
	struct led_classdev dev;
};

/**
 * struct piulxio_devtype - parameters for PIULXIO device
 * @led_names:	Array of LED names, of length @outputs, to use in sysfs
 * @inputs:	Number of input pins
 * @outputs:	Number of output pins
 */
struct piulxio_devtype {
	const char **led_names;
	int inputs;
	int outputs;
};

/**
 * struct piulxio - state of attached PIULXIO
 * @type:	Type of PIULXIO device
 * @idev:	Input device associated with this PIULXIO
 * @phys:	Physical path of the device. @idev's phys field points to this buffer
 * @udev:	USB device associated with this PIULXIO
 * @in:		URB for requesting the current state of inputs
 * @out:	URB for sending data to outputs
 * @old_inputs:	Previous state of input pins for change detection
 * @inputs:	Buffer for the @in URB
 * @outputs:	Buffer for the @out URB
 * @new_outputs: Staging for the @outputs buffer
 */
struct piulxio {
	struct piulxio_devtype *type;

	struct input_dev *idev;
	char phys[64];

	struct usb_device *udev;
	struct urb *in, *out;
	wait_queue_head_t shutdown_wait;

	unsigned long old_inputs[LXIO_MSG_LONGS];
	unsigned char inputs[LXIO_MSG_SZ];
	unsigned char outputs[LXIO_MSG_SZ];
	unsigned char new_outputs[LXIO_MSG_SZ];

	struct piulxio_led *led;
	struct timer_list poll_timer;
	spinlock_t io_lock;
};

static const char *led_names[] = {
	"piulxio::p1_lu",        // 0
	"piulxio::p1_ru",        // 1
	"piulxio::p1_cn",        // 2
	"piulxio::p1_ld",        // 3
	"piulxio::p1_rd",        // 4
	"piulxio::p2_lu",        // 5
	"piulxio::p2_ru",        // 6
	"piulxio::p2_cn",        // 7
	"piulxio::p2_ld",        // 8
	"piulxio::p2_rd",        // 9
	"piulxio::bass",         // 10
	"piulxio::halo_r2",      // 11
	"piulxio::halo_r1",      // 12
	"piulxio::halo_l2",      // 13
	"piulxio::halo_l1",      // 14
	"piulxio::coin_counter", // 15
	"piulxio::coin_counter2",// 16
	"piulxio::p1_lu_menu",   // 17
	"piulxio::p1_ru_menu",   // 18
	"piulxio::p1_cn_menu",   // 19
	"piulxio::p1_ld_menu",   // 20
	"piulxio::p1_rd_menu",   // 21
	"piulxio::p2_lu_menu",   // 22
	"piulxio::p2_ru_menu",   // 23
	"piulxio::p2_cn_menu",   // 24
	"piulxio::p2_ld_menu",   // 25
	"piulxio::p2_rd_menu",   // 26
};

/* PIULXIO device parameters */
static struct piulxio_devtype piulxio_dev = {
    .led_names = led_names,
    .inputs = 24,  // 10 pads + 4 operator buttons + 10 menu buttons
    .outputs = 27,
};

/*
 * Auxiliary functions for reporting input events
 */
static int keycode(unsigned int pin)
{
	/* Use joystick buttons first, then the extra "trigger happy" range. */
	if (pin < LXIO_NUM_REG)
		return LXIO_BTN_REG + pin;
	pin -= LXIO_NUM_REG;
	return LXIO_BTN_EXTRA + pin;
}

/*
 * Input processing function - extracts inputs from 16-byte buffer
 */
static void piulxio_process_inputs(struct piulxio *piu)
{
    unsigned long changed[LXIO_MSG_LONGS];
    unsigned long current_inputs[LXIO_MSG_LONGS];
    unsigned long b;
    int i;
    unsigned char *inBuffer = piu->inputs;

    /* Clear current inputs */
    memset(current_inputs, 0, sizeof(current_inputs));

    /* Invert pull-ups */
    for (i = 0; i < LXIO_MSG_SZ; i++) {
        inBuffer[i] ^= 0xFF;
    }

    /* Extract P1 pad sensors (combine 4 sensors into 1 button per pad) */
    // P1_LU (combine sensors 0-3)
    if ((inBuffer[0] & (1 << 0)) || (inBuffer[1] & (1 << 0)) || 
        (inBuffer[2] & (1 << 0)) || (inBuffer[3] & (1 << 0)))
        __set_bit(0, current_inputs);
    
    // P1_RU (combine sensors 0-3)
    if ((inBuffer[0] & (1 << 1)) || (inBuffer[1] & (1 << 1)) || 
        (inBuffer[2] & (1 << 1)) || (inBuffer[3] & (1 << 1)))
        __set_bit(1, current_inputs);
    
    // P1_CN (combine sensors 0-3)
    if ((inBuffer[0] & (1 << 2)) || (inBuffer[1] & (1 << 2)) || 
        (inBuffer[2] & (1 << 2)) || (inBuffer[3] & (1 << 2)))
        __set_bit(2, current_inputs);
    
    // P1_LD (combine sensors 0-3)
    if ((inBuffer[0] & (1 << 3)) || (inBuffer[1] & (1 << 3)) || 
        (inBuffer[2] & (1 << 3)) || (inBuffer[3] & (1 << 3)))
        __set_bit(3, current_inputs);
    
    // P1_RD (combine sensors 0-3)
    if ((inBuffer[0] & (1 << 4)) || (inBuffer[1] & (1 << 4)) || 
        (inBuffer[2] & (1 << 4)) || (inBuffer[3] & (1 << 4)))
        __set_bit(4, current_inputs);

    /* Extract P2 pad sensors (combine 4 sensors into 1 button per pad) */
    // P2_LU (combine sensors 0-3)
    if ((inBuffer[4] & (1 << 0)) || (inBuffer[5] & (1 << 0)) || 
        (inBuffer[6] & (1 << 0)) || (inBuffer[7] & (1 << 0)))
        __set_bit(5, current_inputs);
    
    // P2_RU (combine sensors 0-3)
    if ((inBuffer[4] & (1 << 1)) || (inBuffer[5] & (1 << 1)) || 
        (inBuffer[6] & (1 << 1)) || (inBuffer[7] & (1 << 1)))
        __set_bit(6, current_inputs);
    
    // P2_CN (combine sensors 0-3)
    if ((inBuffer[4] & (1 << 2)) || (inBuffer[5] & (1 << 2)) || 
        (inBuffer[6] & (1 << 2)) || (inBuffer[7] & (1 << 2)))
        __set_bit(7, current_inputs);
    
    // P2_LD (combine sensors 0-3)
    if ((inBuffer[4] & (1 << 3)) || (inBuffer[5] & (1 << 3)) || 
        (inBuffer[6] & (1 << 3)) || (inBuffer[7] & (1 << 3)))
        __set_bit(8, current_inputs);
    
    // P2_RD (combine sensors 0-3)
    if ((inBuffer[4] & (1 << 4)) || (inBuffer[5] & (1 << 4)) || 
        (inBuffer[6] & (1 << 4)) || (inBuffer[7] & (1 << 4)))
        __set_bit(9, current_inputs);

    /* Extract operator buttons (byte 8) */
    if (inBuffer[8] & (1 << 1)) __set_bit(10, current_inputs); // Test
    if (inBuffer[8] & (1 << 6)) __set_bit(11, current_inputs); // Service
    if (inBuffer[8] & (1 << 7)) __set_bit(12, current_inputs); // Clear
    if (inBuffer[8] & (1 << 2)) __set_bit(13, current_inputs); // Coin

    /* Extract P1 menu buttons (byte 10) */
    if (inBuffer[10] & (1 << 0)) __set_bit(14, current_inputs); // P1_LU_Menu
    if (inBuffer[10] & (1 << 1)) __set_bit(15, current_inputs); // P1_RU_Menu
    if (inBuffer[10] & (1 << 2)) __set_bit(16, current_inputs); // P1_CN_Menu
    if (inBuffer[10] & (1 << 3)) __set_bit(17, current_inputs); // P1_LD_Menu
    if (inBuffer[10] & (1 << 4)) __set_bit(18, current_inputs); // P1_RD_Menu

    /* Extract P2 menu buttons (byte 11) */
    if (inBuffer[11] & (1 << 0)) __set_bit(19, current_inputs); // P2_LU_Menu
    if (inBuffer[11] & (1 << 1)) __set_bit(20, current_inputs); // P2_RU_Menu
    if (inBuffer[11] & (1 << 2)) __set_bit(21, current_inputs); // P2_CN_Menu
    if (inBuffer[11] & (1 << 3)) __set_bit(22, current_inputs); // P2_LD_Menu
    if (inBuffer[11] & (1 << 4)) __set_bit(23, current_inputs); // P2_RD_Menu

    /* Note what has changed */
    for (i = 0; i < LXIO_MSG_LONGS; i++) {
        changed[i] = current_inputs[i] ^ piu->old_inputs[i];
        piu->old_inputs[i] = current_inputs[i];
    }

    /* For each input which has changed state, report whether it was pressed
     * or released based on the current value. */
    for_each_set_bit(b, changed, piu->type->inputs) {
        input_event(piu->idev, EV_MSC, MSC_SCAN, b + 1);
        input_report_key(piu->idev, keycode(b), test_bit(b, current_inputs));
    }

    /* Done reporting input events */
    input_sync(piu->idev);
}

/*
 * URB completion handlers
 */
static void piulxio_in_completed(struct urb *urb)
{
    struct piulxio *piu = urb->context;
    int ret = urb->status;

    if (ret) {
        dev_warn(&piu->udev->dev, "piulxio callback(in): error %d\n", ret);
        goto resubmit;
    }

    /* Process input data */
    piulxio_process_inputs(piu);

    /* Submit output URB to send LED data */
    ret = usb_submit_urb(piu->out, GFP_ATOMIC);
    if (ret == -EPERM)
        dev_info(&piu->udev->dev, "piulxio resubmit(out): shutdown\n");
    else if (ret)
        dev_err(&piu->udev->dev, "piulxio resubmit(out): error %d\n", ret);
    else
        return; /* Success, don't wake up shutdown_wait */

resubmit:
    /* Let any waiting threads know we're done here */
    wake_up(&piu->shutdown_wait);
}

static void piulxio_out_completed(struct urb *urb)
{
    struct piulxio *piu = urb->context;
    int ret = urb->status;

    if (ret) {
        dev_warn(&piu->udev->dev, "piulxio callback(out): error %d\n", ret);
        goto resubmit;
    }

    /* Copy in the new outputs */
    spin_lock(&piu->io_lock);
    memcpy(piu->outputs, piu->new_outputs, LXIO_MSG_SZ);
    spin_unlock(&piu->io_lock);

    /* Submit input URB to continue the cycle */
    ret = usb_submit_urb(piu->in, GFP_ATOMIC);
    if (ret == -EPERM)
        dev_info(&piu->udev->dev, "piulxio resubmit(in): shutdown\n");
    else if (ret)
        dev_err(&piu->udev->dev, "piulxio resubmit(in): error %d\n", ret);
    else
        return; /* Success, don't wake up shutdown_wait */

resubmit:
    /* Let any waiting threads know we're done here */
    wake_up(&piu->shutdown_wait);
}

/*
 * Input device events
 */
static int piulxio_open(struct input_dev *idev)
{
    struct piulxio *piu = input_get_drvdata(idev);
    
    /* Polling is already started in probe, so nothing to do here */
    return 0;
}

static void piulxio_close(struct input_dev *idev)
{
    /* Keep polling running even when input device is closed */
    /* This allows buttons to work immediately when needed */
}

/*
 * Led device event
 */
static void piulxio_led_set(struct led_classdev *dev, enum led_brightness b)
{
    struct piulxio_led *led = container_of(dev, struct piulxio_led, dev);
    struct piulxio *piu = led->piu;
    int n;

    n = led - piu->led;
    if (n >= piu->type->outputs) {
        dev_err(&piu->udev->dev, "piulxio led: bad number %d\n", n);
        return;
    }

    spin_lock(&piu->io_lock);
    if (b)
        __set_bit(n, (unsigned long *) piu->new_outputs);
    else
        __clear_bit(n, (unsigned long *) piu->new_outputs);
    spin_unlock(&piu->io_lock);

    /* If device is not open, send LED data directly */
    if (!piu->idev->users) {
        int ret, actual_length;
        memcpy(piu->outputs, piu->new_outputs, LXIO_MSG_SZ);
        ret = usb_interrupt_msg(piu->udev,
            usb_sndintpipe(piu->udev, LXIO_ENDPOINT_OUTPUT),
            piu->outputs, LXIO_MSG_SZ, &actual_length, 1000);
        if (ret)
            dev_err(&piu->udev->dev, "piulxio led: send failed %d\n", ret);
        else
            dev_info(&piu->udev->dev, "piulxio led: sent %d bytes\n", actual_length);
    }
}

/*
 * Structure initialization and destruction
 */
static void piulxio_input_init(struct piulxio *piu, struct device *parent)
{
	struct input_dev *idev = piu->idev;
	int i;

	/* Fill in basic fields */
	idev->name = "PIULXIO input";
	idev->phys = piu->phys;
	usb_to_input_id(piu->udev, &idev->id);
	idev->dev.parent = parent;

	/* HACK: Buttons are sufficient to trigger a /dev/input/js* device, but
	 * for systemd (and consequently udev and Xorg) to consider us a
	 * joystick, we have to have a set of XY absolute axes. */
	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_ABS, idev->evbit);

	/* Configure buttons */
	for (i = 0; i < piu->type->inputs; i++)
		set_bit(keycode(i), idev->keybit);
	clear_bit(0, idev->keybit);

	/* Configure fake axes */
	set_bit(ABS_X, idev->absbit);
	set_bit(ABS_Y, idev->absbit);
	input_set_abs_params(idev, ABS_X, 0, 0, 0, 0);
	input_set_abs_params(idev, ABS_Y, 0, 0, 0, 0);

	/* Set device callbacks */
	idev->open = piulxio_open;
	idev->close = piulxio_close;

	/* Link input device back to PIULXIO */
	input_set_drvdata(idev, piu);
}

static int piulxio_leds_init(struct piulxio *piu)
{
	int i;
	const struct attribute_group **ag;
	struct attribute **attr;
	int ret;

	for (i = 0; i < piu->type->outputs; i++) {
		/* Initialize led device and point back to piulxio struct */
		piu->led[i].dev.name = piu->type->led_names[i];
		piu->led[i].dev.brightness_set = piulxio_led_set;
		piu->led[i].piu = piu;

		/* Register led device */
		ret = led_classdev_register(&piu->udev->dev, &piu->led[i].dev);
		if (ret)
			goto out_unregister;

		/* Relax permissions on led attributes */
		for (ag = piu->led[i].dev.dev->class->dev_groups; *ag; ag++) {
			attr = (*piu->led[i].dev.dev->class->dev_groups)->attrs;
			ret = sysfs_chmod_file(&piu->led[i].dev.dev->kobj,
						*attr, S_IRUGO | S_IWUGO);
			if (ret) {
				led_classdev_unregister(&piu->led[i].dev);
				goto out_unregister;
			}
		}
	}

	return 0;

out_unregister:
	for (--i; i >= 0; i--)
		led_classdev_unregister(&piu->led[i].dev);
	return ret;
}

static void piulxio_leds_destroy(struct piulxio *piu)
{
	int i;
	for (i = 0; i < piu->type->outputs; i++)
		led_classdev_unregister(&piu->led[i].dev);
}

static int piulxio_init(struct piulxio *piu, struct input_dev *idev,
		struct usb_device *udev)
{
	/* Note: if this function returns an error, piulxio_destroy will still be
	 * called, so we don't need to clean up here */

	/* Allocate USB request blocks */
	piu->in = usb_alloc_urb(0, GFP_KERNEL);
	piu->out = usb_alloc_urb(0, GFP_KERNEL);
	if (!piu->in || !piu->out) {
		dev_err(&udev->dev, "piulxio init: failed to allocate URBs\n");
		return -ENOMEM;
	}

	/* Allocate LED array */
	piu->led = kzalloc(sizeof(*piu->led) * piu->type->outputs, GFP_KERNEL);
	if (!piu->led) {
		dev_err(&udev->dev, "piulxio init: failed to allocate led devices\n");
		return -ENOMEM;
	}

	init_waitqueue_head(&piu->shutdown_wait);
	spin_lock_init(&piu->io_lock);

	piu->idev = idev;
	piu->udev = udev;
	usb_make_path(udev, piu->phys, sizeof(piu->phys));
	strlcat(piu->phys, "/input0", sizeof(piu->phys));

	/* Prepare URB for outputs */
	usb_fill_int_urb(piu->out, udev,
			usb_sndintpipe(udev, LXIO_ENDPOINT_OUTPUT),
			piu->outputs, LXIO_MSG_SZ,
			piulxio_out_completed, piu, 1);

	/* Prepare URB for inputs */
	usb_fill_int_urb(piu->in, udev,
			usb_rcvintpipe(udev, LXIO_ENDPOINT_INPUT),
			piu->inputs, LXIO_MSG_SZ,
			piulxio_in_completed, piu, 1);

	return 0;
}

static void piulxio_destroy(struct piulxio *piu)
{
	/* These handle NULL gracefully, so we can call this to clean up if init fails */
	kfree(piu->led);
	usb_free_urb(piu->out);
	usb_free_urb(piu->in);
}

/*
 * USB connect and disconnect events
 */
static int piulxio_probe(struct usb_interface *intf,
             const struct usb_device_id *id)
{
    struct piulxio *piu;
    struct usb_device *udev = interface_to_usbdev(intf);
    struct input_dev *idev;
    int ret = -ENOMEM;

    dev_info(&intf->dev, "PIULXIO: probe called for device %04x:%04x\n",
         le16_to_cpu(udev->descriptor.idVendor),
         le16_to_cpu(udev->descriptor.idProduct));

    /* Allocate PIULXIO state */
    piu = kzalloc(sizeof(struct piulxio), GFP_KERNEL);
    if (!piu) {
        dev_err(&intf->dev, "piulxio probe: failed to allocate state\n");
        return ret;
    }

    piu->type = &piulxio_dev;

    /* Allocate input device for generating button presses */
    idev = input_allocate_device();
    if (!idev) {
        dev_err(&intf->dev, "piulxio probe: failed to allocate input dev\n");
        kfree(piu);
        return ret;
    }

    /* Initialize PIULXIO state and input device */
    ret = piulxio_init(piu, idev, udev);
    if (ret) {
        dev_err(&intf->dev, "piulxio probe: init failed with %d\n", ret);
        goto err;
    }

    piulxio_input_init(piu, &intf->dev);

    /* Initialize and register led devices */
    ret = piulxio_leds_init(piu);
    if (ret) {
        dev_err(&intf->dev, "piulxio probe: leds init failed with %d\n", ret);
        goto err;
    }

    /* Register input device */
    ret = input_register_device(piu->idev);
    if (ret) {
        dev_err(&intf->dev, "piulxio probe: failed to register input dev\n");
        piulxio_leds_destroy(piu);
        goto err;
    }

    /* Initialize outputs to all off and start polling immediately */
    memset(piu->outputs, 0, LXIO_MSG_SZ);
    memset(piu->new_outputs, 0, LXIO_MSG_SZ);
    
    /* Start the input polling cycle immediately */
    ret = usb_submit_urb(piu->in, GFP_KERNEL);
    if (ret) {
        dev_err(&intf->dev, "piulxio probe: failed to start polling %d\n", ret);
        piulxio_leds_destroy(piu);
        input_unregister_device(piu->idev);
        goto err;
    }

    /* Final USB setup */
    usb_set_intfdata(intf, piu);
    
    dev_info(&intf->dev, "PIULXIO: probe completed successfully\n");
    return 0;

err:
    piulxio_destroy(piu);
    input_free_device(idev);
    kfree(piu);
    return ret;
}

static void piulxio_disconnect(struct usb_interface *intf)
{
	struct piulxio *piu = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!piu) {
		dev_err(&intf->dev, "piulxio disconnect: uninitialized device?\n");
		return;
	}

	usb_kill_urb(piu->in);
	usb_kill_urb(piu->out);
	piulxio_leds_destroy(piu);
	input_unregister_device(piu->idev);
	piulxio_destroy(piu);
	kfree(piu);
}

/*
 * USB driver and module definitions
 */
static struct usb_device_id piulxio_id_table[] = {
	/* PIULXIO board */
	{ USB_DEVICE(USB_VENDOR_ID_LXIO, USB_PRODUCT_ID_LXIO) },
	{},
};

MODULE_DEVICE_TABLE(usb, piulxio_id_table);

static struct usb_driver piulxio_driver = {
	.name =		"piulxio",
	.probe =	piulxio_probe,
	.disconnect =	piulxio_disconnect,
	.id_table =	piulxio_id_table,
};

MODULE_AUTHOR("Based on PIUIO driver by Devin J. Pohly");
MODULE_DESCRIPTION("PIULXIO input/output driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");

module_usb_driver(piulxio_driver);
