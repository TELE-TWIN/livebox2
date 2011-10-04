/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2004  Marcel Holtmann <marcel@holtmann.org>
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <time.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "glib-ectomy.h"

#include "hcid.h"
#include "lib.h"
#include <util/mgt_client.h>
#include <bluetooth/bluetooth_ssi.h>

#define MAX_RG_MGT_CMD_LEN 129

static GIOChannel *io_chan[HCI_MAX_DEV];

static int pairing;

void toggle_pairing(int enable)
{
	if (enable)
		pairing = hcid.pairing;
	else
		pairing = 0;

	syslog(LOG_INFO, "Pairing %s", pairing ? "enabled" : "disabled");
}

/* Link Key handling */

/* This function is not reentrable */
static struct link_key *__get_link_key(int f, bdaddr_t *sba, bdaddr_t *dba)
{
	static struct link_key k;
	struct link_key *key = NULL;
	int r;

	while ((r = read_n(f, &k, sizeof(k)))) {
		if (r < 0) {
			syslog(LOG_ERR, "Link key database read failed. %s(%d)",
					strerror(errno), errno);
			break;
		}

		if (!bacmp(&k.sba, sba) && !bacmp(&k.dba, dba)) {
			key = &k;
			break;
		}
	}
	return key;
}

static struct link_key *get_link_key(bdaddr_t *sba, bdaddr_t *dba)
{
	struct link_key *key = NULL;
	int f;

	f = open(hcid.key_file, O_RDONLY);
	if (f >= 0)
		key = __get_link_key(f, sba, dba);
	else if (errno != ENOENT)
		syslog(LOG_ERR, "Link key database open failed. %s(%d)",
				strerror(errno), errno);
	close(f);
	return key;
}

static int is_conn_request_valid(bdaddr_t *dba)
{
	char mgt_cmd[MAX_RG_MGT_CMD_LEN];
	char *mgt_result = NULL;
	int rc;
	char da[18];

	ba2str(dba, da);
	snprintf(mgt_cmd, sizeof(mgt_cmd), RG_BT_ACL_CHECK_SSI, da);
	rc = mgt_command(mgt_cmd, &mgt_result);
	if (!rc && !strcmp(mgt_result, RG_BT_ACL_ACCEPT))
	    return 1;

	return 0;
}

static void link_key_request(int dev, bdaddr_t *sba, bdaddr_t *dba)
{
	struct link_key *key = get_link_key(sba, dba);
	char sa[18], da[18];

	ba2str(sba, sa); ba2str(dba, da);
	syslog(LOG_INFO, "link_key_request (sba=%s, dba=%s)\n", sa, da);

	if (key) {
		/* Link key found */
		link_key_reply_cp lr;
		memcpy(lr.link_key, key->key, 16);
		bacpy(&lr.bdaddr, dba);
		if (is_conn_request_valid(dba))
		{
		    hci_send_cmd(dev, OGF_LINK_CTL, OCF_LINK_KEY_REPLY,
			LINK_KEY_REPLY_CP_SIZE, &lr);
		    key->time = time(0);
		    return;
		}
	} 
	/* Link key not found */
	hci_send_cmd(dev, OGF_LINK_CTL, OCF_LINK_KEY_NEG_REPLY, 6, dba);
}

static void save_link_key(struct link_key *key)
{
	struct link_key *exist;
	char sa[18], da[18];
	int f, err;

	f = open(hcid.key_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (f < 0) {
		syslog(LOG_ERR, "Link key database open failed. %s(%d)",
				strerror(errno), errno);
		return;
	}

	/* Check if key already exist */
	exist = __get_link_key(f, &key->sba, &key->dba);

	err = 0;

	if (exist) {
		off_t o = lseek(f, 0, SEEK_CUR);
		err = lseek(f, o - sizeof(*key), SEEK_SET);
	} else
		err = fcntl(f, F_SETFL, O_APPEND);

	if (err < 0) {
		syslog(LOG_ERR, "Link key database seek failed. %s(%d)",
				strerror(errno), errno);
		goto failed;
	}
	
	if (write_n(f, key, sizeof(*key)) < 0) {
		syslog(LOG_ERR, "Link key database write failed. %s(%d)",
				strerror(errno), errno);
	}

	ba2str(&key->sba, sa); ba2str(&key->dba, da);
	syslog(LOG_INFO, "%s link key %s %s", exist ? "Replacing" : "Saving", sa, da);

failed:
	close(f);
}

static void conn_complete_notify(int dev, bdaddr_t *sba, void *ptr)
{
	evt_conn_complete *evt = ptr;
	bdaddr_t *dba = &evt->bdaddr;
	char da[18];
	char mgt_cmd[MAX_RG_MGT_CMD_LEN];
	char *mgt_result = NULL;

	if (evt->status)
	    return;
	ba2str(dba, da);
	snprintf(mgt_cmd, sizeof(mgt_cmd), RG_BT_DEVICE_CONNECTED_SSI, da,
	    evt->status);
	mgt_command(mgt_cmd, &mgt_result);
}

static void link_key_notify(int dev, bdaddr_t *sba, void *ptr)
{
	evt_link_key_notify *evt = ptr;
	bdaddr_t *dba = &evt->bdaddr;
	struct link_key key;
	char sa[18];

	ba2str(sba, sa);
	syslog(LOG_INFO, "link_key_notify (sba=%s)\n", sa);

	memcpy(key.key, evt->link_key, 16);
	bacpy(&key.sba, sba);
	bacpy(&key.dba, dba);
	key.type = evt->key_type;
	key.time = time(0);

	save_link_key(&key);
}

/* PIN code handling */

int read_pin_code(void)
{
	char buf[17];
	FILE *f; 
	int len;

	if (!(f = fopen(hcid.pin_file, "r"))) {
		syslog(LOG_ERR, "Can't open PIN file %s. %s(%d)",
				hcid.pin_file, strerror(errno), errno);
		return -1;
	}

	if (fgets(buf, sizeof(buf), f)) {
		strtok(buf, "\n\r");
		len = strlen(buf); 
		memcpy(hcid.pin_code, buf, len);
		hcid.pin_len = len;
	} else {
		syslog(LOG_ERR, "Can't read PIN file %s. %s(%d)",
				hcid.pin_file, strerror(errno), errno);
		len = -1;
	}
	fclose(f);
	return len;
}

/*
  PIN helper is an external app that asks user for a PIN. It can 
  implement its own PIN  code generation policy and methods like
  PIN look up in some database, etc. 
  HCId expects following output from PIN helper:
	PIN:12345678	-	PIN code
	ERR		-	No PIN available
*/

static void call_pin_helper(int dev, struct hci_conn_info *ci)
{
	pin_code_reply_cp pr;
	struct sigaction sa;
	char addr[18], str[255], *pin, name[20];
	FILE *pipe;
	int ret, len;

	/* Run PIN helper in the separate process */
	switch (fork()) {
		case 0:
			break;
		case -1:
			syslog(LOG_ERR, "Can't fork PIN helper. %s(%d)",
					strerror(errno), errno);
		default:
			return;
	}

	if (access(hcid.pin_helper, R_OK | X_OK)) {
		syslog(LOG_ERR, "Can't exec PIN helper %s. %s(%d)",
				hcid.pin_helper, strerror(errno), errno);
		goto reject;
	}

	name[0] = 0;
	//hci_remote_name(dev, &ci->bdaddr, sizeof(name), name, 0);

	ba2str(&ci->bdaddr, addr);
	sprintf(str, "%s %s %s \'%s\'", hcid.pin_helper,
			ci->out ? "out" : "in", addr, name);

	setenv("PATH", "/bin:/usr/bin:/usr/local/bin", 1);

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);

	pipe = popen(str, "r");
	if (!pipe) {
		syslog(LOG_ERR, "Can't exec PIN helper. %s(%d)", strerror(errno), errno);
		goto reject;
	}

	pin = fgets(str, sizeof(str), pipe);
	ret = pclose(pipe);

	if (!pin || strlen(pin) < 5)
		goto nopin;

	strtok(pin, "\n\r");

	if (strncmp("PIN:", pin, 4))
		goto nopin;

	pin += 4;
	len  = strlen(pin);

	memset(&pr, 0, sizeof(pr));
	bacpy(&pr.bdaddr, &ci->bdaddr);
	memcpy(pr.pin_code, pin, len);
	pr.pin_len = len;
	hci_send_cmd(dev, OGF_LINK_CTL, OCF_PIN_CODE_REPLY,
			PIN_CODE_REPLY_CP_SIZE, &pr);
	exit(0);

nopin:
	if (!pin || strncmp("ERR", pin, 3))
		syslog(LOG_ERR, "PIN helper exited abnormally with code %d", ret);

reject:
	hci_send_cmd(dev, OGF_LINK_CTL, OCF_PIN_CODE_NEG_REPLY, 6, &ci->bdaddr);
	exit(0);
}

static void request_pin(int dev, struct hci_conn_info *ci)
{
#ifdef ENABLE_DBUS
	if (hcid.dbus_pin_helper) {
		hcid_dbus_request_pin(dev, ci);
		return;
	}
#endif
	call_pin_helper(dev, ci);
}

static void pin_code_request(int dev, bdaddr_t *sba, bdaddr_t *dba)
{
	struct hci_conn_info_req *cr;
	struct hci_conn_info *ci;
	char sa[18], da[18];

	ba2str(sba, sa); ba2str(dba, da);
	syslog(LOG_INFO, "pin_code_request (sba=%s, dba=%s)\n", sa, da);

	cr = malloc(sizeof(*cr) + sizeof(*ci));
	if (!cr)
		return;

	bacpy(&cr->bdaddr, dba);
	cr->type = ACL_LINK;
	if (ioctl(dev, HCIGETCONNINFO, (unsigned long) cr) < 0) {
		syslog(LOG_ERR, "Can't get conn info %s(%d)",
					strerror(errno), errno);
		goto reject;
	}
	ci = cr->conn_info;

	if (pairing == HCID_PAIRING_ONCE) {
		struct link_key *key = get_link_key(sba, dba);
		if (key) {
			ba2str(dba, da);
			syslog(LOG_WARNING, "PIN code request for already paired device %s", da);
			goto reject;
		}
	} else if (pairing == HCID_PAIRING_NONE)
		goto reject;

	if (!is_conn_request_valid(dba))
	    goto reject;

	if (hcid.security == HCID_SEC_AUTO) {
		if (!ci->out) {
			/* Incomming connection */
			pin_code_reply_cp pr;
			memset(&pr, 0, sizeof(pr));
			bacpy(&pr.bdaddr, dba);
			memcpy(pr.pin_code, hcid.pin_code, hcid.pin_len);
			pr.pin_len = hcid.pin_len;
			hci_send_cmd(dev, OGF_LINK_CTL, OCF_PIN_CODE_REPLY,
				PIN_CODE_REPLY_CP_SIZE, &pr);
		} else {
			/* Outgoing connection */

			/* Let PIN helper handle that */ 
			request_pin(dev, ci);
		}
	} else {
		/* Let PIN helper handle that */ 
		request_pin(dev, ci);
	}
	free(cr);
	return;

reject:
	hci_send_cmd(dev, OGF_LINK_CTL, OCF_PIN_CODE_NEG_REPLY, 6, dba);
	free(cr);
	return;
}

gboolean io_security_event(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	unsigned char buf[HCI_MAX_EVENT_SIZE], *ptr = buf;
	struct hci_dev_info *di = (void *) data;
	int type, dev;
	size_t len;
	hci_event_hdr *eh;
	GIOError err;

	if (cond & (G_IO_NVAL | G_IO_HUP | G_IO_ERR)) {
		g_io_channel_close(chan);
		free(data);
		return FALSE;
	}

	if ((err = g_io_channel_read(chan, buf, sizeof(buf), &len))) {
		if (err == G_IO_ERROR_AGAIN)
			return TRUE;
		g_io_channel_close(chan);
		free(data);
		return FALSE;
	}

	type = *ptr++;

	if (type != HCI_EVENT_PKT)
		return TRUE;

	eh = (hci_event_hdr *) ptr;
	ptr += HCI_EVENT_HDR_SIZE;

	dev = g_io_channel_unix_get_fd(chan);

	ioctl(dev, HCIGETDEVINFO, (void *) di);

	if (hci_test_bit(HCI_SECMGR, &di->flags))
		return TRUE;

	switch (eh->evt) {
	case EVT_PIN_CODE_REQ:
		pin_code_request(dev, &di->bdaddr, (bdaddr_t *) ptr);
		break;

	case EVT_LINK_KEY_REQ:
		link_key_request(dev, &di->bdaddr, (bdaddr_t *) ptr);
		break;

	case EVT_LINK_KEY_NOTIFY:
		link_key_notify(dev, &di->bdaddr, ptr);
		break;

	case EVT_CONN_COMPLETE:
		conn_complete_notify(dev, &di->bdaddr, ptr);
		break;
	}

	return TRUE;
}

void start_security_manager(int hdev)
{
	GIOChannel *chan = io_chan[hdev];
	struct hci_dev_info *di;
	struct hci_filter flt;
	int dev;

	if (chan)
		return;

	syslog(LOG_INFO, "Starting security manager %d", hdev);

	if ((dev = hci_open_dev(hdev)) < 0) {
		syslog(LOG_ERR, "Can't open device hci%d. %s(%d)",
				hdev, strerror(errno), errno);
		return;
	}

	/* Set filter */
	hci_filter_clear(&flt);
	hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
	hci_filter_set_event(EVT_PIN_CODE_REQ, &flt);
	hci_filter_set_event(EVT_LINK_KEY_REQ, &flt);
	hci_filter_set_event(EVT_LINK_KEY_NOTIFY, &flt);
	hci_filter_set_event(EVT_CONN_COMPLETE, &flt);
	if (setsockopt(dev, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		syslog(LOG_ERR, "Can't set filter on hci%d. %s(%d)", 
				hdev, strerror(errno), errno);
		close(dev);
		return;
	}

	di = malloc(sizeof(*di));
	if (!di) {
		syslog(LOG_ERR, "Can't allocate device info buffer. %s(%d)",
				strerror(errno), errno);
		close(dev);
		return;
	}

	di->dev_id = hdev;
	if (ioctl(dev, HCIGETDEVINFO, (void *)di)) {
		syslog(LOG_ERR, "Can't get device info. %s(%d)",
				strerror(errno), errno);
		close(dev);
		return;
	}

	chan = g_io_channel_unix_new(dev);
	g_io_add_watch(chan, G_IO_IN | G_IO_NVAL | G_IO_HUP | G_IO_ERR,
			io_security_event, (void *) di);

	io_chan[hdev] = chan;
}

void stop_security_manager(int hdev)
{
	GIOChannel *chan = io_chan[hdev];

	if (!chan)
		return;

	syslog(LOG_INFO, "Stoping security manager %d", hdev);

	/* this is a bit sneaky. closing the fd will cause the event
	   loop to call us right back with G_IO_NVAL set, at which
	   point we will see it and clean things up */
	close(g_io_channel_unix_get_fd(chan));
	io_chan[hdev] = NULL;
}

void init_security_data(void)
{
	/* Set local PIN code */
	if (read_pin_code() < 0) {
		strcpy(hcid.pin_code, "BlueZ");
		hcid.pin_len = 5;
	}

	pairing = hcid.pairing;
	return;
}
