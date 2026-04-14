/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_USB_GPIO_VBUS_H
#define __LINUX_USB_GPIO_VBUS_H

#include <linux/types.h>

/*
 * Legacy platform-data used by old gpio-vbus based OTG glue.
 * Keep only fields consumed by in-tree users.
 */
struct gpio_vbus_mach_info {
	int gpio_vbus;
	int gpio_pullup;
	bool gpio_vbus_inverted;
	bool gpio_pullup_inverted;
	bool wakeup;
};

struct usb_bus;
struct usb_phy;

void rda_start_host(struct usb_bus *host);
void rda_stop_host(struct usb_bus *host);
void rda_usbid_set(int value);

void rda_vbus_release(void);
void rda_vbus_acquire(void);
int otg_usb_id_state(struct usb_phy *phy);

#endif /* __LINUX_USB_GPIO_VBUS_H */
