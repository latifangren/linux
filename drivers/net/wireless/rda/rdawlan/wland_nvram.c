
/*
 * Copyright (c) 2014 Rdamicro Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <linux/fs.h>
#include <asm/uaccess.h>

#include <linux_osl.h>

#include <wland_defs.h>
#include <wland_dbg.h>

#define WIFI_NVRAM_FILE_NAME    "/data/misc/wifi/WLANMAC"

int wland_get_mac_address(char *buf);
int wland_set_mac_address(char *buf);
#ifdef USE_MAC_FROM_RDA_NVRAM
int wlan_read_mac_from_nvram(char *buf);
int wlan_write_mac_to_nvram(const char *buf);
#endif

static int nvram_read(char *filename, char *buf, ssize_t len, int offset)
{
	struct file *fd;
	loff_t pos = offset;
	ssize_t ret_len;

	fd = filp_open(filename, O_RDONLY, 0);
	if (IS_ERR(fd)) {
		WLAND_DBG(BUS, DEBUG, "[nvram_read] : failed to open\n");
		return PTR_ERR(fd);
	}

	ret_len = kernel_read(fd, buf, len, &pos);
	filp_close(fd, NULL);

	return ret_len;
}

static int nvram_write(char *filename, char *buf, ssize_t len, int offset)
{
	struct file *fd;
	loff_t pos = offset;
	ssize_t ret_len;

	fd = filp_open(filename, O_WRONLY | O_CREAT, 0644);
	if (IS_ERR(fd)) {
		WLAND_ERR("[nvram_write] : failed to open!\n");
		return PTR_ERR(fd);
	}

	ret_len = kernel_write(fd, buf, len, &pos);
	filp_close(fd, NULL);

	return ret_len;
}

int wland_get_mac_address(char *buf)
{
	return nvram_read(WIFI_NVRAM_FILE_NAME, buf, 6, 0);
}

int wland_set_mac_address(char *buf)
{
	return nvram_write(WIFI_NVRAM_FILE_NAME, buf, 6, 0);
}

#ifdef USE_MAC_FROM_RDA_NVRAM
int wlan_read_mac_from_nvram(char *buf)
{
	return nvram_read(WIFI_NVRAM_FILE_NAME, buf, ETH_ALEN, 0);
}

int wlan_write_mac_to_nvram(const char *buf)
{
	return nvram_write(WIFI_NVRAM_FILE_NAME, (char *)buf, ETH_ALEN, 0);
}
#endif
