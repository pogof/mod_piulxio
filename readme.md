This is PIU LXIO Linux kernel module driver.

Works on both the 0x1020 and 0x1040 LXIO revisions.

It has been semi-vibecoded based on an existing PIUIO kernel module driver and existing non kernel driver of LXIO.

This Driver makes it so the LXIO is usable in any game or anything else that needs input. There is only basic reactive lighting (you step/press = it lights up)

## How to use

1. Build the module on the system it is intended for using the provided `Makefile`

2. The IO likes to and most likely will be bound by usbhid. You need to either find a way to unbound it every time or setup a `udev` rule (check how it works on your distribution, this is Ubuntu based instructions):

Make a file in `/etc/udev/rules.d/99-piulxio.rules` that contains: 
```
ACTION=="add", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_interface", ATTRS{idVendor}=="0d2f", ATTRS{idProduct}=="1020", ATTR{bInterfaceNumber}=="00", RUN+="/bin/sh -c 'echo -n %k > /sys/bus/usb/drivers/usbhid/unbind; echo -n %k > /sys/bus/usb/drivers/piulxio/bind'"

ACTION=="add", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_interface", ATTRS{idVendor}=="0d2f", ATTRS{idProduct}=="1040", ATTR{bInterfaceNumber}=="00", RUN+="/bin/sh -c 'echo -n %k > /sys/bus/usb/drivers/usbhid/unbind; echo -n %k > /sys/bus/usb/drivers/piulxio/bind'"
```

After you save the content of the file `sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=usb` or reboot your system.

3. Insert the kernel module using `sudo insmod piulxio.ko` 

4. Plug in your IO board. It should now bind to the module.
you can verify that with `sudo dmesg | grep -i piulxio | tail -10`
The output should be something like:
```
[60118.972603] input: PIULXIO input as /devices/pci0000:00/0000:00:01.2/0000:20:00.0/0000:21:08.0/0000:2a:00.3/usb7/7-4/7-4:1.0/input/input48
[60118.972720] piulxio 7-4:1.0: PIULXIO PROBE: initial output URB submitted successfully
[60118.972722] piulxio 7-4:1.0: PIULXIO PROBE: completed successfully
```

5. You should be able to bind the buttons and platform to whatever game you desire.

If you want to load the module on startup, you can do something like add `sudo insmod ~/build/piulxio/mod/piulxio.ko` into your `.bashrc_profile` file (that is an hidden file in your `/home` directory)


## No warranty, no guarantees, no nothing, if it bricks your IO its your fault (it shouldn't but I am not liable if it does)

### Shoutout to Dinsfire64 where I borrowed the piuio module from and voidedref of SGL, where I borrowed the LXIO code from.

Brought to you by Kuro
