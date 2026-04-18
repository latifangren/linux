ifneq ($(KERNELRELEASE),)

obj-$(CONFIG_UNISOC_UWE5621) += uwe5621/
obj-$(CONFIG_UNISOC_SPRDWL) += sprdwl/

else

all: modules

UNISOC_BSP_SYMVERS := $(abspath $(M)/uwe5621/Module.symvers)
UNISOC_BSP_INCLUDE := $(abspath $(M)/uwe5621/include)

modules:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(M)/uwe5621 CFG_AML_WIFI_DEVICE_UWE5621=y modules
	$(CROSS_COMPILE)strip --strip-unneeded ${OUT_DIR}/$(M)/uwe5621/uwe5621.ko
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_SRC) M=$(M)/sprdwl CFG_AML_WIFI_DEVICE_UWE5621=y KBUILD_EXTRA_SYMBOLS=$(UNISOC_BSP_SYMVERS) UNISOC_BSP_INCLUDE=$(UNISOC_BSP_INCLUDE) modules
	$(CROSS_COMPILE)strip --strip-unneeded ${OUT_DIR}/$(M)/sprdwl/sprdwl.ko

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 M=$(M)/uwe5621 -C $(KERNEL_SRC) modules_install
	mkdir -p ${OUT_DIR}/../vendor_lib/modules
	cd ${OUT_DIR}/$(M); find -name "*.ko" -exec cp {} ${OUT_DIR}/../vendor_lib/modules/ \;

endif

