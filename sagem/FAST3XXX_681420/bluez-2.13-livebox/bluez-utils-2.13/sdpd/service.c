/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2001-2002  Nokia Corporation
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2004  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2002-2003  Stephen Crane <steve.crane@rococosoft.com>
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
 *  $Id: service.c,v 1.5 2004/10/01 11:40:15 holtmann Exp $
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <netinet/in.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "sdpd.h"

extern void update_db_timestamp(void);

// FIXME: refactor for server-side
static sdp_record_t *extract_pdu_server(char *p, uint32_t handleExpected, int *scanned)
{
	int extractStatus = -1, localExtractedLength = 0;
	uint8_t dtd;
	int seqlen = 0;
	sdp_record_t *rec = NULL;
	uint16_t attrId, lookAheadAttrId;
	sdp_data_t *pAttr = NULL;
	uint32_t handle = 0xffffffff;

	*scanned = sdp_extract_seqtype(p, &dtd, &seqlen);
	p += *scanned;
	lookAheadAttrId = ntohs(sdp_get_unaligned((uint16_t *)(p + sizeof(uint8_t))));

	SDPDBG("Look ahead attr id : %d\n", lookAheadAttrId);

	if (lookAheadAttrId == SDP_ATTR_RECORD_HANDLE) {
		handle = ntohl(sdp_get_unaligned((uint32_t *)(p +
				sizeof(uint8_t) + sizeof(uint16_t) +
				sizeof(uint8_t))));
		SDPDBG("SvcRecHandle : 0x%x\n", handle);
		rec = sdp_record_find(handle);
	} else if (handleExpected != 0xffffffff)
		rec = sdp_record_find(handleExpected);

	if (rec == NULL) {
		rec = sdp_record_alloc();
		rec->attrlist = NULL;
		if (lookAheadAttrId == SDP_ATTR_RECORD_HANDLE) {
			rec->handle = handle;
			sdp_record_add(rec);
		} else if (handleExpected != 0xffffffff) {
			rec->handle = handleExpected;
			sdp_record_add(rec);
		}
	}

	while (localExtractedLength < seqlen) {
		int attrSize = sizeof(uint8_t);
		int attrValueLength = 0;

		SDPDBG("Extract PDU, sequenceLength: %d localExtractedLength: %d", seqlen, localExtractedLength);
		dtd = *(uint8_t *)p;

		attrId = ntohs(sdp_get_unaligned((uint16_t *)(p + attrSize)));
		attrSize += sizeof(uint16_t);
		
		SDPDBG("DTD of attrId : %d Attr id : 0x%x \n", dtd, attrId);

		pAttr = sdp_extract_attr(p + attrSize, &attrValueLength, rec);

		SDPDBG("Attr id : 0x%x attrValueLength : %d\n", attrId, attrValueLength);

		attrSize += attrValueLength;
		if (pAttr == NULL) {
			SDPDBG("Terminating extraction of attributes");
			break;
		}
		localExtractedLength += attrSize;
		p += attrSize;
		sdp_attr_replace(rec, attrId, pAttr);
		extractStatus = 0;
		SDPDBG("Extract PDU, seqLength: %d localExtractedLength: %d",
					seqlen, localExtractedLength);
	}

	if (extractStatus == 0) {
		SDPDBG("Successful extracting of Svc Rec attributes\n");
#ifdef SDP_DEBUG
		sdp_print_service_attr(rec->attrlist);
#endif
		*scanned += seqlen;
	}
	return rec;
}

/*
 * Add the newly created service record to the service repository
 */
int service_register_req(sdp_req_t *req, sdp_buf_t *rsp)
{
	int scanned = 0;
	sdp_data_t *handle;
	char *p = req->buf + sizeof(sdp_pdu_hdr_t);
	sdp_record_t *rec;

	req->flags = *p++;

	// save image of PDU: we need it when clients request this attribute
	rec = extract_pdu_server(p, 0xffffffff, &scanned);
	if (rec == NULL) {
		sdp_put_unaligned(htons(SDP_INVALID_SYNTAX), (uint16_t *)rsp->data);
		rsp->data_size = sizeof(uint16_t);
		return -1;
	}

	rec->handle = sdp_next_handle();
	if (rec->handle < 0x10000)
		return -1;

	sdp_record_add(rec);
	if (!(req->flags & SDP_RECORD_PERSIST))
		sdp_svcdb_set_collectable(rec, req->sock);
	handle = sdp_data_alloc(SDP_UINT32, &rec->handle);
	sdp_attr_replace(rec, SDP_ATTR_RECORD_HANDLE, handle);
	/*
	 * if the browse group descriptor is NULL,
	 * ensure that the record belongs to the ROOT group
	 */
	if (sdp_data_get(rec, SDP_ATTR_BROWSE_GRP_LIST) == NULL) {
		 uuid_t uuid;
		 sdp_uuid16_create(&uuid, PUBLIC_BROWSE_GROUP);
		 sdp_pattern_add_uuid(rec, &uuid);
	}
	update_db_timestamp();

	/* Build a rsp buffer */
	sdp_put_unaligned(htonl(rec->handle), (uint32_t *)rsp->data);
	rsp->data_size = sizeof(uint32_t);
	return 0;
}

/*
 * Update a service record
 */
int service_update_req(sdp_req_t *req, sdp_buf_t *rsp)
{
	sdp_record_t *orec;
	int status = 0, scanned = 0;
	char *p = req->buf + sizeof(sdp_pdu_hdr_t);
	uint32_t handle = ntohl(sdp_get_unaligned((uint32_t *)p));

	SDPDBG("");

	SDPDBG("Svc Rec Handle: 0x%x\n", handle);
	
	p += sizeof(uint32_t);

	orec = sdp_record_find(handle);
	
	SDPDBG("SvcRecOld: 0x%x\n", (uint32_t)orec);

	if (orec) {
		sdp_record_t *nrec = extract_pdu_server(p, handle, &scanned);
		if (nrec && handle == nrec->handle)
			update_db_timestamp();
		else {
			SDPDBG("SvcRecHandle : 0x%x\n", handle);
			SDPDBG("SvcRecHandleNew : 0x%x\n", nrec->handle);
			SDPDBG("SvcRecNew : 0x%x\n", (uint32_t) nrec);
			SDPDBG("SvcRecOld : 0x%x\n", (uint32_t) orec);
			SDPDBG("Failure to update, restore old value\n");

			if (nrec)
				sdp_record_free(nrec);
			status = SDP_INVALID_SYNTAX;
		}
	} else
		status = SDP_INVALID_RECORD_HANDLE;

	p = rsp->data;
	sdp_put_unaligned(htons(status), (uint16_t *)p);
	rsp->data_size = sizeof(uint16_t);
	return status;
}

/*
 * Remove a registered service record
 */
int service_remove_req(sdp_req_t *req, sdp_buf_t *rsp)
{
	char *p = req->buf + sizeof(sdp_pdu_hdr_t);
	uint32_t handle = ntohl(sdp_get_unaligned((uint32_t *)p));
	sdp_record_t *rec;
	int status = 0;

	SDPDBG("");
	
	/* extract service record handle */
	p += sizeof(uint32_t);

	rec = sdp_record_find(handle);
	if (rec) {
		sdp_svcdb_collect(rec);
		status = sdp_record_remove(handle);
		sdp_record_free(rec);
		if (status == 0)
			update_db_timestamp();
	} else {
		status = SDP_INVALID_RECORD_HANDLE;
		SDPDBG("Could not find record : 0x%x\n", handle);
	}

	p = rsp->data;
	sdp_put_unaligned(htons(status), (uint16_t *)p);
	rsp->data_size = sizeof(uint16_t);

	return status;
}
