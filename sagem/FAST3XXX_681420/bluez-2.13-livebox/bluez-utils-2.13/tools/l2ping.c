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
 *
 *  $Id: l2ping.c,v 1.7 2004/10/11 10:31:33 holtmann Exp $
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <resolv.h>
#include <netdb.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

/* Defaults */
bdaddr_t bdaddr;
int size    = 20;
int ident   = 200;
int delay   = 1;
int count   = -1;
int timeout = 10;

/* Stats */
int sent_pkt = 0, recv_pkt = 0;

static float tv2fl(struct timeval tv)
{
	return (float)(tv.tv_sec*1000.0) + (float)(tv.tv_usec/1000.0);
}

static void stat(int sig)
{
	int loss = sent_pkt ? (float)((sent_pkt-recv_pkt)/(sent_pkt/100.0)) : 0;
	printf("%d sent, %d received, %d%% loss\n", sent_pkt, recv_pkt, loss);
	exit(0);
}

static void ping(char *svr)
{
	struct sockaddr_l2 addr;
	struct sigaction sa;
	char buf[2048];
	char str[18];
	int s, i, opt, lost;
	uint8_t id;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = stat;
	sigaction(SIGINT, &sa, NULL);

	if ((s = socket(PF_BLUETOOTH, SOCK_RAW, BTPROTO_L2CAP)) < 0) {
		perror("Can't create socket.");
		exit(1);
	}

	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	addr.l2_bdaddr = bdaddr;
	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Can't bind socket.");
		exit(1);
	}

	str2ba(svr, &addr.l2_bdaddr);
	if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Can't connect.");
		exit(1);
	}

	/* Get local address */
	opt = sizeof(addr);
	if (getsockname(s, (struct sockaddr *)&addr, &opt) < 0) {
		perror("Can't get local address.");
		exit(1);
	}
	ba2str(&addr.l2_bdaddr, str);

	printf("Ping: %s from %s (data size %d) ...\n", svr, str, size);

	/* Initialize buffer */
	for (i = L2CAP_CMD_HDR_SIZE; i < sizeof(buf); i++)
		buf[i] = (i % 40) + 'A';

	id = ident;

	while( count == -1 || count-- > 0 ){
		struct timeval tv_send, tv_recv, tv_diff;
		l2cap_cmd_hdr *cmd = (l2cap_cmd_hdr *) buf;

		/* Build command header */
		cmd->code  = L2CAP_ECHO_REQ;
		cmd->ident = id;
		cmd->len   = htobs(size);

		gettimeofday(&tv_send, NULL);

		/* Send Echo Request */
		if (send(s, buf, size + L2CAP_CMD_HDR_SIZE, 0) <= 0) {
			perror("Send failed");
			exit(1);
		}

		/* Wait for Echo Response */
		lost = 0;
		while (1) {
			struct pollfd pf[1];
			register int err;

			pf[0].fd = s; pf[0].events = POLLIN;
			if ((err = poll(pf, 1, timeout * 1000)) < 0) {
				perror("Poll failed");
				exit(1);
			}

			if (!err) {
				lost = 1;
				break;
			}

			if ((err = recv(s, buf, sizeof(buf), 0)) < 0) {
				perror("Recv failed");
				exit(1);
			}

			if (!err){
				printf("Disconnected\n");
				exit(1);
			}

			cmd->len = btohs(cmd->len);

			/* Check for our id */
			if( cmd->ident != id )
				continue;

			/* Check type */
			if (cmd->code == L2CAP_ECHO_RSP)
				break;
			if (cmd->code == L2CAP_COMMAND_REJ) {
				printf("Peer doesn't support Echo packets\n");
				exit(1);
			}

		}
		sent_pkt++;

		if (!lost) {
			recv_pkt++;

			gettimeofday(&tv_recv, NULL);
			timersub(&tv_recv, &tv_send, &tv_diff);

			printf("%d bytes from %s id %d time %.2fms\n", cmd->len, svr, id - ident, tv2fl(tv_diff));

			if (delay)
				sleep(delay);
		} else {
			printf("no response from %s: id %d\n", svr, id);
		}

		if (++id > 254)
			id = ident;
	}
	stat(0);
}

static void usage(void)
{
	printf("l2ping - L2CAP ping\n");
	printf("Usage:\n");
	printf("\tl2ping [-S source addr] [-s size] [-c count] [-t timeout] [-f] <bd_addr>\n");
}

extern int optind,opterr,optopt;
extern char *optarg;

int main(int argc, char *argv[])
{
	register int opt;

	/* Default options */
	bacpy(&bdaddr, BDADDR_ANY);

	while ((opt=getopt(argc,argv,"s:c:t:fS:")) != EOF) {
		switch(opt) {
		case 'f':
			/* Kinda flood ping */
			delay = 0;
			break;

		case 'c':
			count = atoi(optarg);
			break;

		case 't':
			timeout = atoi(optarg);
			break;

		case 's':
			size = atoi(optarg);
			break;

		case 'S':
			str2ba(optarg, &bdaddr);
			break;

		default:
			usage();
			exit(1);
		}
	}

	if (!(argc - optind)) {
		usage();
		exit(1);
	}

	ping(argv[optind]);

	return 0;
}
