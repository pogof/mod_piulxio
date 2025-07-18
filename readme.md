Currently targetting Din's ITGmania distro v22

Should just be matter of moving the files to a folder and compiling it using the Make file

Ideally make a folder like so `~/build/piulxio/mod/` and put the Make file and C file there, then compile

To `.bashrc_profile` that is hidden in the home directory add `sudo insmod ~/build/piulxio/mod/piulxio.ko` above where the `usbhid` is inserted and add `,0x0D2F:0x1020:0x00000004` to the usbhid quirks. That should blacklist the module from taking over the IO board.