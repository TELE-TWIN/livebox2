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
 *  $Id: lib.c,v 1.6 2004/12/13 13:58:56 holtmann Exp $
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "hcid.h"
#include "lib.h"

volatile sig_atomic_t __io_canceled;

/* 
 * Device name expansion 
 *   %d - device id
 */
char *expand_name(char *dst, int size, char *str, int dev_id)
{
	register int sp, np, olen;
	char *opt, buf[10];

	if (!str && !dst)
		return NULL;

	sp = np = 0;
	while (np < size - 1 && str[sp]) {
		switch (str[sp]) {
		case '%':
			opt = NULL;

			switch (str[sp+1]) {
			case 'd':
				sprintf(buf, "%d", dev_id);
				opt = buf;
				break;

			case 'h':
				opt = hcid.host_name;
				break;

			case '%':
				dst[np++] = str[sp++];
				/* fall through */
			default:
				sp++;
				continue;
			}

			if (opt) {
				/* substitute */
				olen = strlen(opt);
				if (np + olen < size - 1)
					memcpy(dst + np, opt, olen);
				np += olen;
			}
			sp += 2;
			continue;

		case '\\':
			sp++;
			/* fall through */
		default:
			dst[np++] = str[sp++];
			break;
		}
	}
	dst[np] = '\0';
	return dst;
}

/* Returns current host name */
char *get_host_name(void)
{
	char name[40];

	if (!gethostname(name, sizeof(name)-1)) {
		name[sizeof(name)-1] = 0;
		return strdup(name);
	}
	return strdup("noname");
}

/* Functions to manipulate program title */
extern char **environ;
char	*title_start;	/* start of the proc title space */
char	*title_end;	/* end of the proc title space */
int	title_size;

void init_title(int argc, char *argv[], char *envp[], const char *name)
{
	int i;

	/*
	 *  Move the environment so settitle can use the space at
	 *  the top of memory.
	 */

	for (i = 0; envp[i]; i++);

	environ = (char **) malloc(sizeof (char *) * (i + 1));

	for (i = 0; envp[i]; i++)
		environ[i] = strdup(envp[i]);
	environ[i] = NULL;

	/*
	 *  Save start and extent of argv for set_title.
	 */

	title_start = argv[0];

	/*
	 *  Determine how much space we can use for set_title.  
	 *  Use all contiguous argv and envp pointers starting at argv[0]
		 */
	for (i  =0; i < argc; i++)
		if (!i || title_end == argv[i])
			title_end = argv[i] + strlen(argv[i]) + 1;

	for (i = 0; envp[i]; i++)
		if (title_end == envp[i])
			title_end = envp[i] + strlen(envp[i]) + 1;

	strcpy(title_start, name);
	title_start += strlen(name);
	title_size = title_end - title_start;
}

void set_title(const char *fmt, ...)
{
	char buf[255];
	va_list ap;

	memset(title_start, 0, title_size);

	/* print the argument string */
	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	va_end(ap);

	if (strlen(buf) > title_size - 1)
		buf[title_size - 1] = '\0';

	strcat(title_start, buf);
}
