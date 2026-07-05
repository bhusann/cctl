obj-m += legacygpu.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) CC=clang LD=ld.lld modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
