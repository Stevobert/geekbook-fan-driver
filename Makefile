obj-m += geekbook_ec.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) LLVM=1 modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) LLVM=1 clean
