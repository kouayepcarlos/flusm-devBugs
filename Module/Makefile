PWD   := $(shell pwd)
obj-m += usm_lkmRM.o

all:
	make -C ../Kernel M=$(PWD) modules
install:
	make -C ../Kernel M=$(PWD) modules_install
clean:
	make -C ../Kernel M=$(PWD) clean
