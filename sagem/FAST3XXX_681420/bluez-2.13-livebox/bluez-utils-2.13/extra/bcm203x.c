/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2001-2002  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2003-2004  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation;
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
 *  CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
 *  COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
 *  SOFTWARE IS DISCLAIMED.
 *
 *
 *  $Id: bcm203x.c,v 1.3 2004/08/12 13:17:32 holtmann Exp $
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <usb.h>

#ifdef NEED_USB_GET_BUSSES
static inline struct usb_bus *usb_get_busses(void)
{
	return usb_busses;
}
#endif

#ifdef NEED_USB_INTERRUPT_READ
static inline int usb_interrupt_read(usb_dev_handle *dev, int ep, char *bytes, int size, int timeout)
{
	return usb_bulk_read(dev, ep, bytes, size, timeout);
}
#endif

static char *fw_path = "/lib/firmware";

static int load_file(struct usb_dev_handle *udev, char *filename)
{
	unsigned char buf[4096];
	int fd, err, len;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		syslog(LOG_ERR, "Can't open file %s", filename);
		return fd;
	}

	while (1) { 
		len = read(fd, buf, sizeof(buf)); 
		if (len < 0) {
			syslog(LOG_ERR, "Can't read from file %s", filename);
			close(fd);
			return len;
		}

		if (len == 0)
			break;

		err = usb_bulk_write(udev, 0x02, buf, len, 100000);
		if (err < 0) {
			syslog(LOG_ERR, "Can't write bulk data packet");
			close(fd);
			return err;
		}

		if (err != len) {
			syslog(LOG_ERR, "Partial bulk packet written");
			close(fd);
			return -EIO;
		}
	}

	close(fd);
	return 0;
}

static void load_firmware(struct usb_device *dev)
{
	struct usb_dev_handle *udev;
	char filename[PATH_MAX + 1];
	unsigned char buf[16];

	udev = usb_open(dev);
	if (!udev) {
		syslog(LOG_ERR, "Can't open USB device %s/%s",
					dev->bus->dirname, dev->filename);
		return;
	}

	if (usb_claim_interface(udev, 0) < 0) {
		usb_close(udev);
		return;
	}

	syslog(LOG_INFO, "Loading firmware to device %s/%s",
					dev->bus->dirname, dev->filename);

	snprintf(filename, PATH_MAX, "%s/%s", fw_path, "BCM2033-MD.hex");
	if (load_file(udev, filename) < 0)
		goto done;

	usleep(10);

	if (usb_bulk_write(udev, 0x02, "#", 1, 1000) < 0) {
		syslog(LOG_ERR, "Can't write bulk transfer");
		goto done;
	}

	memset(buf, 0, sizeof(buf));
	if (usb_interrupt_read(udev, 0x81, buf, 16, 1000) < 0) {
		syslog(LOG_ERR, "Can't read interrupt transfer");
		goto done;
	}

	if (buf[0] != '#') {
		syslog(LOG_ERR, "Memory select failed with '%c'", buf[0]);
		goto done;
	}

	snprintf(filename, PATH_MAX, "%s/%s", fw_path, "BCM2033-FW.bin");
	if (load_file(udev, filename) < 0)
		goto done;

	memset(buf, 0, sizeof(buf));
	if (usb_interrupt_read(udev, 0x81, buf, 16, 1000) < 0) {
		syslog(LOG_ERR, "Can't read interrupt transfer");
		goto done;
	}

	if (buf[0] == '.') {
		syslog(LOG_INFO, "Firmware loaded successful to device %s/%s",
					dev->bus->dirname, dev->filename);
	} else {
		syslog(LOG_ERR, "Firmware loading failed with '%c'", buf[0]);
		goto done;
	}

	usleep(500000);

done:
	sleep(1);

	usb_release_interface(udev, 0);
	usb_close(udev);
}

int main(int argc, char *argv[])
{
	struct usb_bus *bus;
	struct usb_device *dev;
	char *action, *device, *busname, *devname;

	action = getenv("ACTION");
	device = getenv("DEVICE");

	if (!action || (strcmp(action, "add") && strcmp(action, "register")))
		exit(0);

	openlog("bcm203x", LOG_NDELAY | LOG_PID, LOG_DAEMON);

	if (!device || strncmp(device, "/proc/bus/usb/", 14)) {
		syslog(LOG_ERR, "Unknown device path %s", device);
		closelog();
		exit(1);
	}

	busname = strtok(device + 14, "/");
	devname = strtok(NULL, "/");

	usb_init();

	usb_find_busses();
	usb_find_devices();

	for (bus = usb_get_busses(); bus; bus = bus->next)
		for (dev = bus->devices; dev; dev = dev->next)
			if (!strcmp(bus->dirname, busname) &&
					!strcmp(dev->filename, devname)) {
				load_firmware(dev);
				break;
			}

	closelog();

	return 0;
}
