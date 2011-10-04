/*
 *  Unix SMB/Netbios implementation.
 *  Version 1.9.
 *  RPC Pipe client / server routines
 *  Copyright (C) Andrew Tridgell              1992-1998
 *  Copyright (C) Luke Kenneth Casson Leighton 1996-1998,
 *  Copyright (C) Paul Ashton                  1997-1998.
 *  Copyright (C) Jeremy Allison                    1999.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*  this module apparently provides an implementation of DCE/RPC over a
 *  named pipe (IPC$ connection using SMBtrans).  details of DCE/RPC
 *  documentation are available (in on-line form) from the X-Open group.
 *
 *  this module should provide a level of abstraction between SMB
 *  and DCE/RPC, while minimising the amount of mallocs, unnecessary
 *  data copies, and network traffic.
 *
 *  in this version, which takes a "let's learn what's going on and
 *  get something running" approach, there is additional network
 *  traffic generated, but the code should be easier to understand...
 *
 *  ... if you read the docs.  or stare at packets for weeks on end.
 *
 */

#include "includes.h"

static void NTLMSSPcalc_p( pipes_struct *p, unsigned char *data, int len)
{
DEBUG(1,("function ommited\n"));
	/* function omited*/
}

/*******************************************************************
 Generate the next PDU to be returned from the data in p->rdata.
 We cheat here as this function doesn't handle the special auth
 footers of the authenticated bind response reply.
 ********************************************************************/

BOOL create_next_pdu(pipes_struct *p)
{
	RPC_HDR_RESP hdr_resp;
	BOOL auth_verify = ((p->ntlmssp_chal_flags & NTLMSSP_NEGOTIATE_SIGN) != 0);
	BOOL auth_seal   = ((p->ntlmssp_chal_flags & NTLMSSP_NEGOTIATE_SEAL) != 0);
	uint32 ss_padding_len = 0;
	uint32 data_len;
	uint32 data_space_available;
	uint32 data_len_left;
	prs_struct outgoing_pdu;
	char *data;
	char *data_from;
	uint32 data_pos;

	/*
	 * If we're in the fault state, keep returning fault PDU's until
	 * the pipe gets closed. JRA.
	 */

	if(p->fault_state) {
		setup_fault_pdu(p, NT_STATUS(0x1c010002));
		return True;
	}

	memset((char *)&hdr_resp, '\0', sizeof(hdr_resp));

	/* Change the incoming request header to a response. */
	p->hdr.pkt_type = RPC_RESPONSE;

	/* Set up rpc header flags. */
	if (p->out_data.data_sent_length == 0)
		p->hdr.flags = RPC_FLG_FIRST;
	else
		p->hdr.flags = 0;

	/*
	 * Work out how much we can fit in a single PDU.
	 */

	data_space_available = sizeof(p->out_data.current_pdu) - RPC_HEADER_LEN - RPC_HDR_RESP_LEN;
	if(p->ntlmssp_auth_validated)
		data_space_available -= (RPC_HDR_AUTH_LEN + RPC_AUTH_NTLMSSP_CHK_LEN);

	/*
	 * The amount we send is the minimum of the available
	 * space and the amount left to send.
	 */

	data_len_left = prs_offset(&p->out_data.rdata) - p->out_data.data_sent_length;

	/*
	 * Ensure there really is data left to send.
	 */

	if(!data_len_left) {
		DEBUG(0,("create_next_pdu: no data left to send !\n"));
		return False;
	}

	data_len = MIN(data_len_left, data_space_available);

	/*
	 * Set up the alloc hint. This should be the data left to
	 * send.
	 */

	hdr_resp.alloc_hint = data_len_left;

	/*
	 * Work out if this PDU will be the last.
	 */

	if(p->out_data.data_sent_length + data_len >= prs_offset(&p->out_data.rdata)) {
		p->hdr.flags |= RPC_FLG_LAST;
		if ((auth_seal || auth_verify) && (data_len_left % 8)) {
			ss_padding_len = 8 - (data_len_left % 8);
			DEBUG(10,("create_next_pdu: adding sign/seal padding of %u\n",
				ss_padding_len ));
		}
	}

	/*
	 * Set up the header lengths.
	 */

	if (p->ntlmssp_auth_validated) {
		p->hdr.frag_len = RPC_HEADER_LEN + RPC_HDR_RESP_LEN + data_len + ss_padding_len +
					RPC_HDR_AUTH_LEN + RPC_AUTH_NTLMSSP_CHK_LEN;
		p->hdr.auth_len = RPC_AUTH_NTLMSSP_CHK_LEN;
	} else {
		p->hdr.frag_len = RPC_HEADER_LEN + RPC_HDR_RESP_LEN + data_len;
		p->hdr.auth_len = 0;
	}

	/*
	 * Init the parse struct to point at the outgoing
	 * data.
	 */

	prs_init( &outgoing_pdu, 0, p->mem_ctx, MARSHALL);
	prs_give_memory( &outgoing_pdu, (char *)p->out_data.current_pdu, sizeof(p->out_data.current_pdu), False);

	/* Store the header in the data stream. */
	if(!smb_io_rpc_hdr("hdr", &p->hdr, &outgoing_pdu, 0)) {
		DEBUG(0,("create_next_pdu: failed to marshall RPC_HDR.\n"));
		prs_mem_free(&outgoing_pdu);
		return False;
	}

	if(!smb_io_rpc_hdr_resp("resp", &hdr_resp, &outgoing_pdu, 0)) {
		DEBUG(0,("create_next_pdu: failed to marshall RPC_HDR_RESP.\n"));
		prs_mem_free(&outgoing_pdu);
		return False;
	}

	/* Store the current offset. */
	data_pos = prs_offset(&outgoing_pdu);

	/* Copy the data into the PDU. */
	data_from = prs_data_p(&p->out_data.rdata) + p->out_data.data_sent_length;

	if(!prs_append_data(&outgoing_pdu, data_from, data_len)) {
		DEBUG(0,("create_next_pdu: failed to copy %u bytes of data.\n", (unsigned int)data_len));
		prs_mem_free(&outgoing_pdu);
		return False;
	}

	/* Copy the sign/seal padding data. */
	if (ss_padding_len) {
		char pad[8];
		memset(pad, '\0', 8);
		if (!prs_append_data(&outgoing_pdu, pad, ss_padding_len)) {
			DEBUG(0,("create_next_pdu: failed to add %u bytes of pad data.\n", (unsigned int)ss_padding_len));
			prs_mem_free(&outgoing_pdu);
			return False;
		}
	}

	/*
	 * Set data to point to where we copied the data into.
	 */

	data = prs_data_p(&outgoing_pdu) + data_pos;

	if (p->hdr.auth_len > 0) {
		uint32 crc32 = 0;

		DEBUG(5,("create_next_pdu: sign: %s seal: %s data %d auth %d\n",
			 BOOLSTR(auth_verify), BOOLSTR(auth_seal), data_len + ss_padding_len, p->hdr.auth_len));

		if (auth_seal) {
			crc32 = crc32_calc_buffer(data, data_len + ss_padding_len);
			NTLMSSPcalc_p(p, (uchar*)data, data_len + ss_padding_len);
		}

		if (auth_seal || auth_verify) {
			RPC_HDR_AUTH auth_info;

			init_rpc_hdr_auth(&auth_info, NTLMSSP_AUTH_TYPE, NTLMSSP_AUTH_LEVEL,
					(auth_verify ? ss_padding_len : 0), (auth_verify ? 1 : 0));
			if(!smb_io_rpc_hdr_auth("hdr_auth", &auth_info, &outgoing_pdu, 0)) {
				DEBUG(0,("create_next_pdu: failed to marshall RPC_HDR_AUTH.\n"));
				prs_mem_free(&outgoing_pdu);
				return False;
			}
		}

		if (auth_verify) {
			RPC_AUTH_NTLMSSP_CHK ntlmssp_chk;
			char *auth_data = prs_data_p(&outgoing_pdu);

			p->ntlmssp_seq_num++;
			init_rpc_auth_ntlmssp_chk(&ntlmssp_chk, NTLMSSP_SIGN_VERSION,
					crc32, p->ntlmssp_seq_num++);
			auth_data = prs_data_p(&outgoing_pdu) + prs_offset(&outgoing_pdu) + 4;
			if(!smb_io_rpc_auth_ntlmssp_chk("auth_sign", &ntlmssp_chk, &outgoing_pdu, 0)) {
				DEBUG(0,("create_next_pdu: failed to marshall RPC_AUTH_NTLMSSP_CHK.\n"));
				prs_mem_free(&outgoing_pdu);
				return False;
			}
			NTLMSSPcalc_p(p, (uchar*)auth_data, RPC_AUTH_NTLMSSP_CHK_LEN - 4);
		}
	}

	/*
	 * Setup the counts for this PDU.
	 */

	p->out_data.data_sent_length += data_len;
	p->out_data.current_pdu_len = p->hdr.frag_len;
	p->out_data.current_pdu_sent = 0;

	prs_mem_free(&outgoing_pdu);
	return True;
}

/*******************************************************************
 Process an NTLMSSP authentication response.
 If this function succeeds, the user has been authenticated
 and their domain, name and calling workstation stored in
 the pipe struct.
 The initial challenge is stored in p->challenge.
 *******************************************************************/

static BOOL api_pipe_ntlmssp_verify(pipes_struct *p, RPC_AUTH_NTLMSSP_RESP *ntlmssp_resp)
{
	DEBUG(1,("function ommited\n"));
	/* function omited*/
}

/*******************************************************************
 The switch table for the pipe names and the functions to handle them.
 *******************************************************************/

struct api_cmd
{
  const char * pipe_clnt_name;
  const char * pipe_srv_name;
  BOOL (*fn) (pipes_struct *);
};

static struct api_cmd api_fd_commands[] =
{
    { "lsarpc",   "lsass",   api_ntlsa_rpc },
    { "samr",     "lsass",   api_samr_rpc },
    { "srvsvc",   "ntsvcs",  api_srvsvc_rpc },
    { "wkssvc",   "ntsvcs",  api_wkssvc_rpc },
    { "NETLOGON", "lsass",   api_netlog_rpc },
    { "winreg",   "winreg",  api_reg_rpc },
    { "spoolss",  "spoolss", api_spoolss_rpc },
#ifdef WITH_MSDFS
    { "netdfs",   "netdfs" , api_netdfs_rpc },
#endif
    { NULL,       NULL,      NULL }
};

/*******************************************************************
 This is the client reply to our challenge for an authenticated
 bind request. The challenge we sent is in p->challenge.
*******************************************************************/

BOOL api_pipe_bind_auth_resp(pipes_struct *p, prs_struct *rpc_in_p)
{
	DEBUG(1,("function ommited\n"));
	/* function omited*/
}

/*******************************************************************
 Marshall a bind_nak pdu.
*******************************************************************/

static BOOL setup_bind_nak(pipes_struct *p)
{
	DEBUG(1,("function ommited\n"));
	/* function omited*/
}

/*******************************************************************
 Marshall a fault pdu.
*******************************************************************/

BOOL setup_fault_pdu(pipes_struct *p, NTSTATUS status)
{
	prs_struct outgoing_pdu;
	RPC_HDR fault_hdr;
	RPC_HDR_RESP hdr_resp;
	RPC_HDR_FAULT fault_resp;

	/* Free any memory in the current return data buffer. */
	prs_mem_free(&p->out_data.rdata);

	/*
	 * Marshall directly into the outgoing PDU space. We
	 * must do this as we need to set to the bind response
	 * header and are never sending more than one PDU here.
	 */

	prs_init( &outgoing_pdu, 0, p->mem_ctx, MARSHALL);
	prs_give_memory( &outgoing_pdu, (char *)p->out_data.current_pdu, sizeof(p->out_data.current_pdu), False);

	/*
	 * Initialize a fault header.
	 */

	init_rpc_hdr(&fault_hdr, RPC_FAULT, RPC_FLG_FIRST | RPC_FLG_LAST | RPC_FLG_NOCALL,
            p->hdr.call_id, RPC_HEADER_LEN + RPC_HDR_RESP_LEN + RPC_HDR_FAULT_LEN, 0);

	/*
	 * Initialize the HDR_RESP and FAULT parts of the PDU.
	 */

	memset((char *)&hdr_resp, '\0', sizeof(hdr_resp));

	fault_resp.status = status;
	fault_resp.reserved = 0;

	/*
	 * Marshall the header into the outgoing PDU.
	 */

	if(!smb_io_rpc_hdr("", &fault_hdr, &outgoing_pdu, 0)) {
		DEBUG(0,("setup_fault_pdu: marshalling of RPC_HDR failed.\n"));
		prs_mem_free(&outgoing_pdu);
		return False;
	}

	if(!smb_io_rpc_hdr_resp("resp", &hdr_resp, &outgoing_pdu, 0)) {
		DEBUG(0,("setup_fault_pdu: failed to marshall RPC_HDR_RESP.\n"));
		prs_mem_free(&outgoing_pdu);
		return False;
	}

	if(!smb_io_rpc_hdr_fault("fault", &fault_resp, &outgoing_pdu, 0)) {
		DEBUG(0,("setup_fault_pdu: failed to marshall RPC_HDR_FAULT.\n"));
		prs_mem_free(&outgoing_pdu);
		return False;
	}

	p->out_data.data_sent_length = 0;
	p->out_data.current_pdu_len = prs_offset(&outgoing_pdu);
	p->out_data.current_pdu_sent = 0;

	prs_mem_free(&outgoing_pdu);
	return True;
}

/*******************************************************************
 Ensure a bind request has the correct abstract & transfer interface.
 Used to reject unknown binds from Win2k.
*******************************************************************/

BOOL check_bind_req(char* pipe_name, RPC_IFACE* abstract,
					RPC_IFACE* transfer)
{
	extern struct pipe_id_info pipe_names[];
	int i=0;
	fstring pname;
	fstrcpy(pname,"\\PIPE\\");
	fstrcat(pname,pipe_name);

	for(i=0;pipe_names[i].client_pipe; i++) {
		if(strequal(pipe_names[i].client_pipe, pname))
			break;
	}

	if(pipe_names[i].client_pipe == NULL)
		return False;

	/* check the abstract interface */
	if((abstract->version != pipe_names[i].abstr_syntax.version) ||
		(memcmp(&abstract->uuid, &pipe_names[i].abstr_syntax.uuid,
			sizeof(RPC_UUID)) != 0))
		return False;

	/* check the transfer interface */
	if((transfer->version != pipe_names[i].trans_syntax.version) ||
		(memcmp(&transfer->uuid, &pipe_names[i].trans_syntax.uuid,
			sizeof(RPC_UUID)) != 0))
		return False;

	return True;
}

/*******************************************************************
 Respond to a pipe bind request.
*******************************************************************/

BOOL api_pipe_bind_req(pipes_struct *p, prs_struct *rpc_in_p)
{
	RPC_HDR_BA hdr_ba;
	RPC_HDR_RB hdr_rb;
	RPC_HDR_AUTH auth_info;
	uint16 assoc_gid;
	fstring ack_pipe_name;
	prs_struct out_hdr_ba;
	prs_struct out_auth;
	prs_struct outgoing_rpc;
	int i = 0;
	int auth_len = 0;
	enum RPC_PKT_TYPE reply_pkt_type;

	p->ntlmssp_auth_requested = False;

	DEBUG(5,("api_pipe_bind_req: decode request. %d\n", __LINE__));

	/*
	 * Try and find the correct pipe name to ensure
	 * that this is a pipe name we support.
	 */

	for (i = 0; api_fd_commands[i].pipe_clnt_name; i++) {
		if (strequal(api_fd_commands[i].pipe_clnt_name, p->name) &&
		    api_fd_commands[i].fn != NULL) {
			DEBUG(3,("api_pipe_bind_req: \\PIPE\\%s -> \\PIPE\\%s\n",
				   api_fd_commands[i].pipe_clnt_name,
				   api_fd_commands[i].pipe_srv_name));
			fstrcpy(p->pipe_srv_name, api_fd_commands[i].pipe_srv_name);
			break;
		}
	}

	if (api_fd_commands[i].fn == NULL) {
		DEBUG(3,("api_pipe_bind_req: Unknown pipe name %s in bind request.\n",
			p->name ));
		if(!setup_bind_nak(p))
			return False;
		return True;
	}

	/* decode the bind request */
	if(!smb_io_rpc_hdr_rb("", &hdr_rb, rpc_in_p, 0))  {
		DEBUG(0,("api_pipe_bind_req: unable to unmarshall RPC_HDR_RB struct.\n"));
		return False;
	}

	/*
	 * Check if this is an authenticated request.
	 */

	if (p->hdr.auth_len != 0) {
		RPC_AUTH_VERIFIER auth_verifier;
		RPC_AUTH_NTLMSSP_NEG ntlmssp_neg;

		/*
		 * Decode the authentication verifier.
		 */

		if(!smb_io_rpc_hdr_auth("", &auth_info, rpc_in_p, 0)) {
			DEBUG(0,("api_pipe_bind_req: unable to unmarshall RPC_HDR_AUTH struct.\n"));
			return False;
		}

		/*
		 * We only support NTLMSSP_AUTH_TYPE requests.
		 */

		if(auth_info.auth_type != NTLMSSP_AUTH_TYPE) {
			DEBUG(0,("api_pipe_bind_req: unknown auth type %x requested.\n",
				auth_info.auth_type ));
			return False;
		}

		if(!smb_io_rpc_auth_verifier("", &auth_verifier, rpc_in_p, 0)) {
			DEBUG(0,("api_pipe_bind_req: unable to unmarshall RPC_HDR_AUTH struct.\n"));
			return False;
		}

		if(!strequal(auth_verifier.signature, "NTLMSSP")) {
			DEBUG(0,("api_pipe_bind_req: auth_verifier.signature != NTLMSSP\n"));
			return False;
		}

		if(auth_verifier.msg_type != NTLMSSP_NEGOTIATE) {
			DEBUG(0,("api_pipe_bind_req: auth_verifier.msg_type (%d) != NTLMSSP_NEGOTIATE\n",
				auth_verifier.msg_type));
			return False;
		}

		if(!smb_io_rpc_auth_ntlmssp_neg("", &ntlmssp_neg, rpc_in_p, 0)) {
			DEBUG(0,("api_pipe_bind_req: Failed to unmarshall RPC_AUTH_NTLMSSP_NEG.\n"));
			return False;
		}

		p->ntlmssp_chal_flags = SMBD_NTLMSSP_NEG_FLAGS;
		p->ntlmssp_auth_requested = True;
	}

	switch(p->hdr.pkt_type) {
		case RPC_BIND:
			/* name has to be \PIPE\xxxxx */
			fstrcpy(ack_pipe_name, "\\PIPE\\");
			fstrcat(ack_pipe_name, p->pipe_srv_name);
			reply_pkt_type = RPC_BINDACK;
			break;
		case RPC_ALTCONT:
			/* secondary address CAN be NULL
			 * as the specs say it's ignored.
			 * It MUST NULL to have the spoolss working.
			 */
			fstrcpy(ack_pipe_name,"");
			reply_pkt_type = RPC_ALTCONTRESP;
			break;
		default:
			return False;
	}

	DEBUG(5,("api_pipe_bind_req: make response. %d\n", __LINE__));

	/*
	 * Marshall directly into the outgoing PDU space. We
	 * must do this as we need to set to the bind response
	 * header and are never sending more than one PDU here.
	 */

	prs_init( &outgoing_rpc, 0, p->mem_ctx, MARSHALL);
	prs_give_memory( &outgoing_rpc, (char *)p->out_data.current_pdu, sizeof(p->out_data.current_pdu), False);

	/*
	 * Setup the memory to marshall the ba header, and the
	 * auth footers.
	 */

	if(!prs_init(&out_hdr_ba, 1024, p->mem_ctx, MARSHALL)) {
		DEBUG(0,("api_pipe_bind_req: malloc out_hdr_ba failed.\n"));
		prs_mem_free(&outgoing_rpc);
		return False;
	}

	if(!prs_init(&out_auth, 1024, p->mem_ctx, MARSHALL)) {
		DEBUG(0,("pi_pipe_bind_req: malloc out_auth failed.\n"));
		prs_mem_free(&outgoing_rpc);
		prs_mem_free(&out_hdr_ba);
		return False;
	}

	if (p->ntlmssp_auth_requested)
		assoc_gid = 0x7a77;
	else
		assoc_gid = hdr_rb.bba.assoc_gid ? hdr_rb.bba.assoc_gid : 0x53f0;

	/*
	 * Create the bind response struct.
	 */

	/* If the requested abstract synt uuid doesn't match our client pipe,
		reject the bind_ack & set the transfer interface synt to all 0's,
		ver 0 (observed when NT5 attempts to bind to abstract interfaces
		unknown to NT4)
		Needed when adding entries to a DACL from NT5 - SK */

	if(check_bind_req(p->name, &hdr_rb.abstract, &hdr_rb.transfer)) {
		init_rpc_hdr_ba(&hdr_ba,
			MAX_PDU_FRAG_LEN,
			MAX_PDU_FRAG_LEN,
			assoc_gid,
			ack_pipe_name,
			0x1, 0x0, 0x0,
			&hdr_rb.transfer);
	} else {
		RPC_IFACE null_interface;
		ZERO_STRUCT(null_interface);
		/* Rejection reason: abstract syntax not supported */
		init_rpc_hdr_ba(&hdr_ba, MAX_PDU_FRAG_LEN,
					MAX_PDU_FRAG_LEN, assoc_gid,
					ack_pipe_name, 0x1, 0x2, 0x1,
					&null_interface);
	}

	/*
	 * and marshall it.
	 */

	if(!smb_io_rpc_hdr_ba("", &hdr_ba, &out_hdr_ba, 0)) {
		DEBUG(0,("api_pipe_bind_req: marshalling of RPC_HDR_BA failed.\n"));
		goto err_exit;
	}

	/*
	 * Now the authentication.
	 */

	if (p->ntlmssp_auth_requested) {
		RPC_AUTH_VERIFIER auth_verifier;
		RPC_AUTH_NTLMSSP_CHAL ntlmssp_chal;

		generate_random_buffer(p->challenge, 8, False);

		/*** Authentication info ***/

		init_rpc_hdr_auth(&auth_info, NTLMSSP_AUTH_TYPE, NTLMSSP_AUTH_LEVEL, RPC_HDR_AUTH_LEN, 1);
		if(!smb_io_rpc_hdr_auth("", &auth_info, &out_auth, 0)) {
			DEBUG(0,("api_pipe_bind_req: marshalling of RPC_HDR_AUTH failed.\n"));
			goto err_exit;
		}

		/*** NTLMSSP verifier ***/

		init_rpc_auth_verifier(&auth_verifier, "NTLMSSP", NTLMSSP_CHALLENGE);
		if(!smb_io_rpc_auth_verifier("", &auth_verifier, &out_auth, 0)) {
			DEBUG(0,("api_pipe_bind_req: marshalling of RPC_AUTH_VERIFIER failed.\n"));
			goto err_exit;
		}

		/* NTLMSSP challenge ***/

		init_rpc_auth_ntlmssp_chal(&ntlmssp_chal, p->ntlmssp_chal_flags, p->challenge);
		if(!smb_io_rpc_auth_ntlmssp_chal("", &ntlmssp_chal, &out_auth, 0)) {
			DEBUG(0,("api_pipe_bind_req: marshalling of RPC_AUTH_NTLMSSP_CHAL failed.\n"));
			goto err_exit;
		}

		/* Auth len in the rpc header doesn't include auth_header. */
		auth_len = prs_offset(&out_auth) - RPC_HDR_AUTH_LEN;
	}

	/*
	 * Create the header, now we know the length.
	 */

	init_rpc_hdr(&p->hdr, reply_pkt_type, RPC_FLG_FIRST | RPC_FLG_LAST,
			p->hdr.call_id,
			RPC_HEADER_LEN + prs_offset(&out_hdr_ba) + prs_offset(&out_auth),
			auth_len);

	/*
	 * Marshall the header into the outgoing PDU.
	 */

	if(!smb_io_rpc_hdr("", &p->hdr, &outgoing_rpc, 0)) {
		DEBUG(0,("pi_pipe_bind_req: marshalling of RPC_HDR failed.\n"));
		goto err_exit;
	}

	/*
	 * Now add the RPC_HDR_BA and any auth needed.
	 */

	if(!prs_append_prs_data( &outgoing_rpc, &out_hdr_ba)) {
		DEBUG(0,("api_pipe_bind_req: append of RPC_HDR_BA failed.\n"));
		goto err_exit;
	}

	if(p->ntlmssp_auth_requested && !prs_append_prs_data( &outgoing_rpc, &out_auth)) {
		DEBUG(0,("api_pipe_bind_req: append of auth info failed.\n"));
		goto err_exit;
	}

	if(!p->ntlmssp_auth_requested)
		p->pipe_bound = True;

	/*
	 * Setup the lengths for the initial reply.
	 */

	p->out_data.data_sent_length = 0;
	p->out_data.current_pdu_len = prs_offset(&outgoing_rpc);
	p->out_data.current_pdu_sent = 0;

	prs_mem_free(&out_hdr_ba);
	prs_mem_free(&out_auth);

	return True;

  err_exit:

	prs_mem_free(&outgoing_rpc);
	prs_mem_free(&out_hdr_ba);
	prs_mem_free(&out_auth);
	return False;
}

/****************************************************************************
 Deal with sign & seal processing on an RPC request.
****************************************************************************/

BOOL api_pipe_auth_process(pipes_struct *p, prs_struct *rpc_in)
{
	DEBUG(1,("function ommited\n"));
	/* function omited*/
}

/****************************************************************************
 Return a user struct for a pipe user.
****************************************************************************/

struct current_user *get_current_user(struct current_user *user, pipes_struct *p)
{
	if (p->ntlmssp_auth_validated) {
		memcpy(user, &p->pipe_user, sizeof(struct current_user));
	} else {
		extern struct current_user current_user;
		memcpy(user, &current_user, sizeof(struct current_user));
	}

	return user;
}

/****************************************************************************
 Find the correct RPC function to call for this request.
 If the pipe is authenticated then become the correct UNIX user
 before doing the call.
****************************************************************************/

BOOL api_pipe_request(pipes_struct *p)
{
	int i = 0;
	BOOL ret = False;

	if (p->ntlmssp_auth_validated) {

		if(!become_authenticated_pipe_user(p)) {
			prs_mem_free(&p->out_data.rdata);
			return False;
		}
	}

	for (i = 0; api_fd_commands[i].pipe_clnt_name; i++) {
		if (strequal(api_fd_commands[i].pipe_clnt_name, p->name) &&
		    api_fd_commands[i].fn != NULL) {
			DEBUG(3,("Doing \\PIPE\\%s\n", api_fd_commands[i].pipe_clnt_name));
			set_current_rpc_talloc(p->mem_ctx);
			ret = api_fd_commands[i].fn(p);
			set_current_rpc_talloc(NULL);
		}
	}

	if (p->ntlmssp_auth_validated)
		unbecome_authenticated_pipe_user();

	return ret;
}

/*******************************************************************
 Calls the underlying RPC function for a named pipe.
 ********************************************************************/

BOOL api_rpcTNP(pipes_struct *p, const char *rpc_name, 
		struct api_struct *api_rpc_cmds)
{
	int fn_num;
	fstring name;
	uint32 offset1, offset2;
 
	/* interpret the command */
	DEBUG(4,("api_rpcTNP: %s op 0x%x - ", rpc_name, p->hdr_req.opnum));

	slprintf(name, sizeof(name)-1, "in_%s", rpc_name);
	prs_dump(name, p->hdr_req.opnum, &p->in_data.data);

	for (fn_num = 0; api_rpc_cmds[fn_num].name; fn_num++) {
		if (api_rpc_cmds[fn_num].opnum == p->hdr_req.opnum && api_rpc_cmds[fn_num].fn != NULL) {
			DEBUG(3,("api_rpcTNP: pipe %u rpc command: %s\n", (unsigned int)p->pnum, api_rpc_cmds[fn_num].name));
			break;
		}
	}

	if (api_rpc_cmds[fn_num].name == NULL) {
		/*
		 * For an unknown RPC just return a fault PDU but
		 * return True to allow RPC's on the pipe to continue
		 * and not put the pipe into fault state. JRA.
		 */
		DEBUG(4, ("unknown\n"));
		setup_fault_pdu(p, NT_STATUS(0x1c010002));
		return True;
	}

	offset1 = prs_offset(&p->out_data.rdata);

	/* do the actual command */
	if(!api_rpc_cmds[fn_num].fn(p)) {
		DEBUG(0,("api_rpcTNP: %s: %s failed.\n", rpc_name, api_rpc_cmds[fn_num].name));
		prs_mem_free(&p->out_data.rdata);
		return False;
	}

	if (p->bad_handle_fault_state) {
		DEBUG(4,("api_rpcTNP: bad handle fault return.\n"));
		p->bad_handle_fault_state = False;
		setup_fault_pdu(p, NT_STATUS(0x1C00001A));
		return True;
	}

	slprintf(name, sizeof(name)-1, "out_%s", rpc_name);
	offset2 = prs_offset(&p->out_data.rdata);
	prs_set_offset(&p->out_data.rdata, offset1);
	prs_dump(name, p->hdr_req.opnum, &p->out_data.rdata);
	prs_set_offset(&p->out_data.rdata, offset2);

	DEBUG(5,("api_rpcTNP: called %s successfully\n", rpc_name));

	/* Check for buffer underflow in rpc parsing */

	if ((DEBUGLEVEL >= 10) && 
	    (p->in_data.data.data_offset != p->in_data.data.buffer_size)) {
		int data_len = p->in_data.data.buffer_size - 
			p->in_data.data.data_offset;
		char *data;

		data = malloc(data_len);

		DEBUG(10, ("api_rpcTNP: rpc input buffer underflow (parse error?)\n"));
		if (data) {
			prs_uint8s(False, "", &p->in_data.data, 0, (unsigned char *)data,
				   data_len);
			SAFE_FREE(data);
		}

	}

	return True;
}
