# SPDX-License-Identifier: GPL-2.0
dtb-$(CONFIG_ARCH_SUNXI) += \
	sun50i-a64-i2c0.dtbo \
	sun50i-a64-i2c1.dtbo \
	sun50i-a64-pps-gpio.dtbo \
	sun50i-a64-spi-add-cs1.dtbo \
	sun50i-a64-spi-jedec-nor.dtbo \
	sun50i-a64-spi-spidev.dtbo \
	sun50i-a64-uart1.dtbo \
	sun50i-a64-uart2.dtbo \
	sun50i-a64-uart3.dtbo \
	sun50i-a64-uart4.dtbo \
	sun50i-a64-w1-gpio.dtbo \
	sun50i-h5-analog-codec.dtbo \
	sun50i-h5-cir.dtbo \
	sun50i-h5-i2c0.dtbo \
	sun50i-h5-i2c1.dtbo \
	sun50i-h5-i2c2.dtbo \
	sun50i-h5-pps-gpio.dtbo \
	sun50i-h5-pwm.dtbo \
	sun50i-h5-spdif-out.dtbo \
	sun50i-h5-spi-add-cs1.dtbo \
	sun50i-h5-spi-jedec-nor.dtbo \
	sun50i-h5-spi-spidev.dtbo \
	sun50i-h5-uart1.dtbo \
	sun50i-h5-uart2.dtbo \
	sun50i-h5-uart3.dtbo \
	sun50i-h5-usbhost0.dtbo \
	sun50i-h5-usbhost1.dtbo \
	sun50i-h5-usbhost2.dtbo \
	sun50i-h5-usbhost3.dtbo \
	sun50i-h5-w1-gpio.dtbo \
	sun50i-h616-emac0-rgmii-pi.dtbo \
	sun50i-h616-gpu.dtbo \
	sun50i-h616-i2c0-pi.dtbo \
	sun50i-h616-i2c1-pi.dtbo \
	sun50i-h616-i2c2-pi.dtbo \
	sun50i-h616-i2c2-ph.dtbo \
	sun50i-h616-i2c3-pg.dtbo \
	sun50i-h616-i2c3-ph.dtbo \
	sun50i-h616-i2c4-pg.dtbo \
	sun50i-h616-i2c4-ph.dtbo \
	sun50i-h616-uart2-pg.dtbo \
	sun50i-h616-uart2-pg-rts-cts.dtbo \
	sun50i-h616-uart2-ph.dtbo \
	sun50i-h616-uart2-ph-rts-cts.dtbo \
	sun50i-h616-uart5.dtbo \
	sun50i-h6-i2c0.dtbo \
	sun50i-h6-i2c1.dtbo \
	sun50i-h6-i2c2.dtbo \
	sun50i-h6-ruart.dtbo \
	sun50i-h6-spi-add-cs1.dtbo \
	sun50i-h6-spi-jedec-nor.dtbo \
	sun50i-h6-spi-spidev.dtbo \
	sun50i-h6-spi-spidev1.dtbo \
	sun50i-h6-uart1.dtbo \
	sun50i-h6-uart2.dtbo \
	sun50i-h6-uart3.dtbo \
	sun50i-h6-w1-gpio.dtbo

scr-$(CONFIG_ARCH_SUNXI) += \
	sun50i-a64-fixup.scr \
	sun50i-h5-fixup.scr \
	sun50i-h6-fixup.scr

dtbotxt-$(CONFIG_ARCH_SUNXI) += \
	README.sun50i-a64-overlays \
	README.sun50i-h5-overlays

targets += $(dtb-y) $(scr-y) $(dtbotxt-y)

always		:= $(dtb-y) $(scr-y) $(dtbotxt-y)
clean-files	:= *.dtbo *.scr
