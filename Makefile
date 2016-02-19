ifneq ($(CONFIG_OF),)
obj-m += leds-tlc5940.o
endif

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean
