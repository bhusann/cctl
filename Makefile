obj-m += legacymethod/

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) CC=clang LD=ld.lld modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
