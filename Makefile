KDIR ?= /lib/modules/$(shell uname -r)/build

ifneq ($(CONFIG_OF),)
obj-m += leds-tlc5940.o
endif

default: modules
	exit

%::
	$(MAKE) -C $(KDIR) M=$(PWD) $@
