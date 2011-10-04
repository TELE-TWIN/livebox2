/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
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
 *  $Id: sdp.c,v 1.3 2004/05/02 21:55:36 holtmann Exp $
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

int sdp_search_hcrp(sdp_session_t *sdp, unsigned short *ctrl_psm, unsigned short *data_psm)
{
	sdp_list_t *srch, *attrs, *rsp;
	uuid_t svclass;
	uint16_t attr1, attr2;
	int err;

	if (!sdp)
		return -1;

	sdp_uuid16_create(&svclass, HCR_PRINT_SVCLASS_ID);
	srch = sdp_list_append(NULL, &svclass);

	attr1 = SDP_ATTR_PROTO_DESC_LIST;
	attrs = sdp_list_append(NULL, &attr1);
	attr2 = SDP_ATTR_ADD_PROTO_DESC_LIST;
	attrs = sdp_list_append(attrs, &attr2);

	err = sdp_service_search_attr_req(sdp, srch, SDP_ATTR_REQ_INDIVIDUAL, attrs, &rsp);
	if (err)
		return -1;

	for (; rsp; rsp = rsp->next) {
		sdp_record_t *rec = (sdp_record_t *) rsp->data;
		sdp_list_t *protos;

		if (!sdp_get_access_protos(rec, &protos)) {
			unsigned short psm = sdp_get_proto_port(protos, L2CAP_UUID);
			if (psm > 0) {
				*ctrl_psm = psm;
			}
		}

		if (!sdp_get_add_access_protos(rec, &protos)) {
			unsigned short psm = sdp_get_proto_port(protos, L2CAP_UUID);
			if (psm > 0 && *ctrl_psm > 0) {
				*data_psm = psm;
				return 0;
			}
		}
	}

	return -1;
}

int sdp_search_spp(sdp_session_t *sdp, uint8_t *channel)
{
	sdp_list_t *srch, *attrs, *rsp;
	uuid_t svclass;
	uint16_t attr;
	int err;

	if (!sdp)
		return -1;

	sdp_uuid16_create(&svclass, SERIAL_PORT_SVCLASS_ID);
	srch = sdp_list_append(NULL, &svclass);

	attr = SDP_ATTR_PROTO_DESC_LIST;
	attrs = sdp_list_append(NULL, &attr);

	err = sdp_service_search_attr_req(sdp, srch, SDP_ATTR_REQ_INDIVIDUAL, attrs, &rsp);
	if (err)
		return -1;

	for (; rsp; rsp = rsp->next) {
		sdp_record_t *rec = (sdp_record_t *) rsp->data;
		sdp_list_t *protos;

		if (!sdp_get_access_protos(rec, &protos)) {
			uint8_t ch = sdp_get_proto_port(protos, RFCOMM_UUID);
			if (ch > 0) {
				*channel = ch;
				return 0;
			}
		}
	}

	return -1;
}
