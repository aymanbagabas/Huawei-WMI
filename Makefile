
obj-m		:= huawei-wmi.o
KERN_SRC	:= /lib/modules/$(shell uname -r)/build/
PWD			:= $(shell pwd)
KERNEL_CC_IS_CLANG := $(shell grep -q '^CONFIG_CC_IS_CLANG=y' $(KERN_SRC)/include/config/auto.conf 2>/dev/null && echo y)
KBUILD_TOOLCHAIN := $(if $(filter y,$(KERNEL_CC_IS_CLANG)),LLVM=1)

modules:
	make -C $(KERN_SRC) M=$(PWD) $(KBUILD_TOOLCHAIN) modules

install:
	make -C $(KERN_SRC) M=$(PWD) $(KBUILD_TOOLCHAIN) modules_install
	depmod -a

clean:
	make -C $(KERN_SRC) M=$(PWD) $(KBUILD_TOOLCHAIN) clean
