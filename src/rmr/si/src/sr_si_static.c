// vim: ts=4 sw=4 noet :
/*
==================================================================================
	Copyright (c) 2019-2020 Nokia
	Copyright (c) 2018-2020 AT&T Intellectual Property.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
==================================================================================
*/

/*
	Mnemonic:	sr_si_static.c
	Abstract:	These are static send/receive primatives which (sadly)
				differ based on the underlying protocol (nng vs SI95).
				Split from rmr_nng.c  for easier wormhole support.

	Author:		E. Scott Daniels
	Date:		13 February 2019
*/

#ifndef _sr_si_static_c
#define _sr_si_static_c

static void dump_40( char *p, char* label ) {
	int i;

	if( label )
		fprintf( stderr, ">>>>> %s p=%p\n", label, p );

	for( i = 0; i < 40; i++ ) {
		fprintf( stderr, "%02x ", (unsigned char) *(p+i) );
	}
	fprintf( stderr, "\n" );
}

/*
	Translates the nng state passed in to one of ours that is suitable to put
	into the message, and sets errno to something that might be useful.
	If we don't have a specific RMr state, then we return the default (e.g.
	receive failed).

	The addition of the connection shut error code to the switch requires
	that the NNG version at commit e618abf8f3db2a94269a (or after) be
	used for compiling RMR. 
*/
static inline int xlate_si_state( int state, int def_state ) {

	switch( state ) {
		case SI_ERR_NONE:
			errno = 0;
			state = RMR_OK;
			break;

		default:
			state = def_state;
			errno = EAGAIN;
			break;

		case SI_ERR_QUEUED:
		case SI_ERR_TPORT:
		case SI_ERR_UPORT:
		case SI_ERR_FORK:
		case SI_ERR_HANDLE:
		case SI_ERR_SESSID:
		case SI_ERR_TP:
		case SI_ERR_SHUTD:
		case SI_ERR_NOFDS:
		case SI_ERR_SIGUSR1:
		case SI_ERR_SIGUSR2:
		case SI_ERR_DISC:
		case SI_ERR_TIMEOUT:
		case SI_ERR_ORDREL:
		case SI_ERR_SIGALRM:
		case SI_ERR_NOMEM:
		case SI_ERR_ADDR:
			errno  = ENOTSUP;
			state = def_state;
			break;
	}

	return state;
}

/*
	Alloc a new nano zero copy buffer and put into msg. If msg is nil, then we will alloc
	a new message struct as well. Size is the size of the zc buffer to allocate (not
	including our header). If size is 0, then the buffer allocated is the size previously
	allocated (if msg is !nil) or the default size given at initialisation).

	The trlo (trace data lengh override) is used for trace length if >0. If <= 0, then
	the context value is used.

	NOTE:  while accurate, the nng doc implies that both the msg buffer and data buffer
		are zero copy, however ONLY the message is zero copy. We now allocate and use
		nng messages.
*/
static rmr_mbuf_t* alloc_zcmsg( uta_ctx_t* ctx, rmr_mbuf_t* msg, int size, int state, int trlo ) {
	size_t		mlen = -1;			// size of the transport buffer that we'll allocate
	uta_mhdr_t*	hdr;			// convenience pointer
	int			tr_len;			// trace data len (default or override)
	int*		alen;			// convenience pointer to set allocated len

	tr_len = trlo > 0 ? trlo : ctx->trace_data_len;

	mlen = sizeof( uta_mhdr_t ) + tr_len + ctx->d1_len + ctx->d2_len;	// start with header and trace/data lengths
	mlen += (size > 0 ? size  : ctx->max_plen);							// add user requested size or size set during init
	mlen = sizeof( char ) * (mlen + TP_HDR_LEN);						// finally add the transport header len

	if( msg == NULL && (msg = (rmr_mbuf_t *) uta_ring_extract( ctx->zcb_mring )) == NULL ) {
		msg = (rmr_mbuf_t *) malloc( sizeof *msg );
		if( msg == NULL ) {
			fprintf( stderr, "[CRI] rmr_alloc_zc: cannot get memory for message\n" );
			return NULL;								// we used to exit -- that seems wrong
		}
		memset( msg, 0, sizeof( *msg ) );	// tp_buffer will be allocated below
	} else {								// user message or message from the ring
		if( mlen > msg->alloc_len ) {		// current allocation is too small
			msg->alloc_len = 0;				// force tp_buffer realloc below
			if( msg->tp_buf ) {
				free( msg->tp_buf );
			}
		} else {
			mlen = msg->alloc_len;							// msg given, allocate the same size as before
		}
	}

	msg->rts_fd = -1;					// must force to be invalid; not a received message that can be returned

	if( !msg->alloc_len && (msg->tp_buf = (void *) malloc( mlen )) == NULL ) {
		fprintf( stderr, "[CRI] rmr_alloc_zc: cannot get memory for zero copy buffer: %d bytes\n", (int) mlen );
		abort( );											// toss out a core file for this
	}

/*
	memset( msg->tp_buf, 0, mlen );    // NOT for production (debug only)	valgrind will complain about uninitalised use if we don't set
	memcpy( msg->tp_buf, "@@!!@@!!@@!!@@!!@@!!@@!!@@!!@@!!**", 34 );		// NOT for production -- debugging eyecatcher
*/
	alen = (int *) msg->tp_buf;
	*alen = mlen;						// FIX ME: need a stuct to go in these first bytes, not just dummy len

	msg->header = ((char *) msg->tp_buf) + TP_HDR_LEN;
	memset( msg->header, 0, sizeof( uta_mhdr_t ) );				// ensure no junk in the header area
	if( (hdr = (uta_mhdr_t *) msg->header) != NULL ) {
		hdr->rmr_ver = htonl( RMR_MSG_VER );					// set current version
		hdr->sub_id = htonl( UNSET_SUBID );
		SET_HDR_LEN( hdr );										// ensure these are converted to net byte order
		SET_HDR_TR_LEN( hdr, ctx->trace_data_len );
		SET_HDR_D1_LEN( hdr, ctx->d1_len );
		//SET_HDR_D2_LEN( hdr, ctx->d2_len );				// future
	}
	msg->len = 0;											// length of data in the payload
	msg->alloc_len = mlen;									// length of allocated transport buffer (caller size + rmr header)
	msg->sub_id = UNSET_SUBID;
	msg->mtype = UNSET_MSGTYPE;
	msg->payload = PAYLOAD_ADDR( hdr );						// point to payload (past all header junk)
	msg->xaction = ((uta_mhdr_t *)msg->header)->xid;		// point at transaction id in header area
	msg->state = state;										// fill in caller's state (likely the state of the last operation)
	msg->flags |= MFL_ZEROCOPY;								// this is a zerocopy sendable message
	msg->ring = ctx->zcb_mring;								// original msg_free() api doesn't get context so must dup on eaach :(
	strncpy( (char *) ((uta_mhdr_t *)msg->header)->src, ctx->my_name, RMR_MAX_SRC );
	strncpy( (char *) ((uta_mhdr_t *)msg->header)->srcip, ctx->my_ip, RMR_MAX_SRC );

	if( DEBUG > 1 ) fprintf( stderr, "[DBUG] alloc_zcmsg mlen=%ld size=%d mpl=%d flags=%02x\n", (long) mlen, size, ctx->max_plen, msg->flags );

	return msg;
}

/*
	Allocates only the mbuf and does NOT allocate an underlying transport buffer since
	transport receive should allocate that on its own.
*/
static rmr_mbuf_t* alloc_mbuf( uta_ctx_t* ctx, int state ) {
	size_t	mlen;
	uta_mhdr_t* hdr;			// convenience pointer
	rmr_mbuf_t* msg;

	if( (msg = (rmr_mbuf_t *) uta_ring_extract( ctx->zcb_mring )) != NULL ) {
		if( msg->tp_buf ) {
			free( msg->tp_buf );		// caller doesn't want it -- future put this on an accumulation ring
		}
	} else {
		if( (msg = (rmr_mbuf_t *) malloc( sizeof *msg )) == NULL ) {
			fprintf( stderr, "[CRI] rmr_alloc_mbuf: cannot get memory for message\n" );
			return NULL;							// this used to exit, but that seems wrong
		}
	}

	memset( msg, 0, sizeof( *msg ) );

	msg->sub_id = UNSET_SUBID;
	msg->mtype = UNSET_MSGTYPE;
	msg->tp_buf = NULL;
	msg->header = NULL;
	msg->len = -1;											// no payload; invalid len
	msg->alloc_len = 0;
	msg->payload = NULL;
	msg->xaction = NULL;
	msg->state = state;
	msg->flags = 0;
	msg->ring = ctx->zcb_mring;							// original msg_free() api doesn't get context so must dup on eaach :(

	return msg;
}

/*
	This accepts a message with the assumption that only the tp_buf pointer is valid. It
	sets all of the various header/payload/xaction pointers in the mbuf to the proper
	spot in the transport layer buffer.  The len in the header is assumed to be the
	allocated len (a receive buffer that nng created);

	The alen parm is the assumed allocated length; assumed because it's a value likely
	to have come from si receive and the actual alloc len might be larger, but we
	can only assume this is the total usable space. Because we are managing a transport
	header in the first n bytes of the real msg, we must adjust this length down by the
	size of the tp header (for testing 50 bytes, but this should change to a struct if
	we adopt this interface).

	This function returns the message with an error state set if it detects that the
	received message might have been truncated.  Check is done here as the calculation
	is somewhat based on header version.
*/
static void ref_tpbuf( rmr_mbuf_t* msg, size_t alen )  {
	uta_mhdr_t* hdr = NULL;			// current header
	uta_v1mhdr_t* v1hdr;			// version 1 header
	int ver;
	int	hlen;						// header len to use for a truncation check

	msg->header = ((char *) msg->tp_buf) + TP_HDR_LEN;

	v1hdr = (uta_v1mhdr_t *) msg->header;					// v1 will always allow us to suss out the version

	if( v1hdr->rmr_ver == 1 ) {			// bug in verion 1 didn't encode the version in network byte order
		ver = 1;
		v1hdr->rmr_ver = htonl( 1 );		// save it correctly in case we clone the message
	} else {
		ver = ntohl( v1hdr->rmr_ver );
	}

	switch( ver ) {
		case 1:
			msg->len = ntohl( v1hdr->plen );						// length sender says is in the payload (received length could be larger)
			msg->alloc_len = alen;									// length of whole tp buffer (including header, trace and data bits)
			msg->payload = msg->header + sizeof( uta_v1mhdr_t );	// point past header to payload (single buffer allocation above)

			msg->xaction = &v1hdr->xid[0];							// point at transaction id in header area
			msg->flags |= MFL_ZEROCOPY;								// this is a zerocopy sendable message
			msg->mtype = ntohl( v1hdr->mtype );						// capture and convert from network order to local order
			msg->sub_id = UNSET_SUBID;								// type 1 messages didn't have this
			msg->state = RMR_OK;
			hlen = sizeof( uta_v1mhdr_t );
			break;

		default:													// current version always lands here
			hdr = (uta_mhdr_t *) msg->header;
			msg->len = ntohl( hdr->plen );							// length sender says is in the payload (received length could be larger)
			msg->alloc_len = alen;									// length of whole tp buffer (including header, trace and data bits)

			msg->payload = PAYLOAD_ADDR( hdr );						// at user payload
			msg->xaction = &hdr->xid[0];							// point at transaction id in header area
			msg->flags |= MFL_ZEROCOPY;								// this is a zerocopy sendable message
			msg->mtype = ntohl( hdr->mtype );						// capture and convert from network order to local order
			msg->sub_id = ntohl( hdr->sub_id );
			hlen = RMR_HDR_LEN( hdr );								// len to use for truncated check later
			break;
	}

	if( msg->len > (msg->alloc_len - hlen ) ) {
		msg->state = RMR_ERR_TRUNC;
		msg->len = msg->alloc_len -  hlen;							// adjust len down so user app doesn't overrun
	} else {
		msg->state = RMR_OK;
	}
}

/*
	This will clone a message into a new zero copy buffer and return the cloned message.
*/
static inline rmr_mbuf_t* clone_msg( rmr_mbuf_t* old_msg  ) {
	rmr_mbuf_t* nm;			// new message buffer
	size_t	mlen;
	int state;
	uta_mhdr_t* hdr;
	uta_v1mhdr_t* v1hdr;

	nm = (rmr_mbuf_t *) malloc( sizeof *nm );
	if( nm == NULL ) {
		fprintf( stderr, "[CRI] rmr_clone: cannot get memory for message buffer\n" );
		exit( 1 );
	}
	memset( nm, 0, sizeof( *nm ) );

	mlen = old_msg->alloc_len;										// length allocated before
	if( (nm->tp_buf = (void *) malloc( sizeof( char ) * (mlen + TP_HDR_LEN) )) == NULL ) {
		fprintf( stderr, "[CRI] rmr_si_clone: cannot get memory for zero copy buffer: %d\n", (int) mlen );
		abort();
	}

	nm->header = ((char *) nm->tp_buf) + TP_HDR_LEN;
	v1hdr = (uta_v1mhdr_t *) old_msg->header;				// v1 will work to dig header out of any version
	switch( ntohl( v1hdr->rmr_ver ) ) {
		case 1:
			memcpy( v1hdr, old_msg->header, sizeof( *v1hdr ) );	 	// copy complete header
			nm->payload = (void *) v1hdr + sizeof( *v1hdr );
			break;

		default:											// current message always caught  here
			hdr = nm->header;
			memcpy( hdr, old_msg->header, RMR_HDR_LEN( old_msg->header ) + RMR_TR_LEN( old_msg->header ) + RMR_D1_LEN( old_msg->header ) + RMR_D2_LEN( old_msg->header )); 	// copy complete header, trace and other data
			nm->payload = PAYLOAD_ADDR( hdr );				// at user payload
			break;
	}

	// --- these are all version agnostic -----------------------------------
	nm->mtype = old_msg->mtype;
	nm->sub_id = old_msg->sub_id;
	nm->len = old_msg->len;									// length of data in the payload
	nm->alloc_len = mlen;									// length of allocated payload

	nm->xaction = hdr->xid;									// reference xaction
	nm->state = old_msg->state;								// fill in caller's state (likely the state of the last operation)
	nm->flags = old_msg->flags | MFL_ZEROCOPY;				// this is a zerocopy sendable message
	memcpy( nm->payload, old_msg->payload, old_msg->len );

	return nm;
}

/*
	This will clone a message with a change to the trace area in the header such that
	it will be tr_len passed in. The trace area in the cloned message will be uninitialised.
	The orignal message will be left unchanged, and a pointer to the new message is returned.
	It is not possible to realloc buffers and change the data sizes.
*/
static inline rmr_mbuf_t* realloc_msg( rmr_mbuf_t* old_msg, int tr_len  ) {
	rmr_mbuf_t* nm;			// new message buffer
	size_t	mlen;
	int state;
	uta_mhdr_t* hdr;
	uta_v1mhdr_t* v1hdr;
	int	tr_old_len;			// tr size in new buffer
	int*	alen;			// convenience pointer to set toal xmit len FIX ME!
	int		tpb_len;		// total transmit buffer len (user space, rmr header and tp header)

	nm = (rmr_mbuf_t *) malloc( sizeof *nm );
	if( nm == NULL ) {
		fprintf( stderr, "[CRI] rmr_clone: cannot get memory for message buffer\n" );
		exit( 1 );
	}
	memset( nm, 0, sizeof( *nm ) );

	hdr = old_msg->header;
	tr_old_len = RMR_TR_LEN( hdr );				// bytes in old header for trace

	mlen = old_msg->alloc_len + (tr_len - tr_old_len);							// new length with trace adjustment
	if( DEBUG ) fprintf( stderr, "[DBUG] tr_realloc old size=%d new size=%d new tr_len=%d\n", (int) old_msg->alloc_len, (int) mlen, (int) tr_len );

	tpb_len = mlen + TP_HDR_LEN;
	if( (nm->tp_buf = (void *) malloc( tpb_len)) == NULL ) {
		fprintf( stderr, "[CRI] rmr_clone: cannot get memory for zero copy buffer: %d\n", ENOMEM );
		exit( 1 );
	}
	memset( nm->tp_buf, 0, tpb_len );
	memcpy( nm->tp_buf, "@@!!@@!!@@!!@@!!@@!!@@!!@@!!@@!!**", 34 );		// DEBUGGING
	alen = (int *) nm->tp_buf;
	*alen = tpb_len;						// FIX ME: need a stuct to go in these first bytes, not just dummy len

	nm->header = ((char *) nm->tp_buf) + TP_HDR_LEN;

	v1hdr = (uta_v1mhdr_t *) old_msg->header;				// v1 will work to dig header out of any version
	switch( ntohl( v1hdr->rmr_ver ) ) {
		case 1:
			memcpy( v1hdr, old_msg->header, sizeof( *v1hdr ) );	 	// copy complete header
			nm->payload = (void *) v1hdr + sizeof( *v1hdr );
			break;

		default:											// current message version always caught  here
			hdr = nm->header;
			memcpy( hdr, old_msg->header, sizeof( uta_mhdr_t ) );	 	// ONLY copy the header portion; trace and data offsets might have changed
			SET_HDR_TR_LEN( hdr, tr_len );								// must adjust trace len in new message before copy

			if( RMR_D1_LEN( hdr )  ) {
				memcpy( DATA1_ADDR( hdr ), DATA1_ADDR( old_msg->header), RMR_D1_LEN( hdr ) );	 	// copy data1 and data2 if necessary
			}
			if( RMR_D2_LEN( hdr )  ) {
				memcpy( DATA2_ADDR( hdr ), DATA2_ADDR( old_msg->header), RMR_D2_LEN( hdr ) );
			}

			nm->payload = PAYLOAD_ADDR( hdr );									// directly at the payload
			break;
	}

	// --- these are all version agnostic -----------------------------------
	nm->mtype = old_msg->mtype;
	nm->sub_id = old_msg->sub_id;
	nm->len = old_msg->len;									// length of data in the payload
	nm->alloc_len = mlen;									// length of allocated payload

	nm->xaction = hdr->xid;									// reference xaction
	nm->state = old_msg->state;								// fill in caller's state (likely the state of the last operation)
	nm->flags = old_msg->flags | MFL_ZEROCOPY;				// this is a zerocopy sendable message
	memcpy( nm->payload, old_msg->payload, old_msg->len );

	return nm;
}

/*
	Realloc the message such that the payload is at least payload_len bytes.  
	The clone and copy options affect what portion of the original payload is copied to
	the reallocated message, and whether or not the original payload is lost after the
	reallocation process has finished.

		copy == true
		The entire payload from the original message will be coppied to the reallocated
		payload.

		copy == false
		Only the header (preserving return to sender information, message type, etc)
		is preserved after reallocation; the payload used lengrh is set to 0 and the
		payload is NOT initialised/cleared.

		clone == true
		The orignal message is preserved and a completely new message buffer and payload
		are allocated (even if the size given is the same). A pointer to the new message
		buffer is returned and it is the user application's responsibility to manage the
		old buffer (e.g. free when not needed).

		clone == false
		The old payload will be lost after reallocation. The message buffer pointer which
		is returned will likely reference the same structure (don't depend on that).
		

	CAUTION:
	If the message is not a message which was received, the mtype, sub-id, length values in the
	RMR header in the allocated transport buffer will NOT be accurate and will cause the resulting
	mbuffer information for mtype and subid to be reset even when copy is true. To avoid silently
	resetting information in the mbuffer, this funciton will reset the mbuf values from the current
	settings and NOT from the copied RMR header in transport buffer.
*/
static inline rmr_mbuf_t* realloc_payload( rmr_mbuf_t* old_msg, int payload_len, int copy, int clone ) {
	rmr_mbuf_t* nm = NULL;	// new message buffer when cloning
	size_t	mlen;
	uta_mhdr_t* omhdr;		// old message header
	int		tr_old_len;		// tr size in new buffer
	int		old_psize = 0;	// size of payload in the message passed in (alloc size - tp header and rmr header lengths)
	int		hdr_len = 0;	// length of RMR and transport headers in old msg
	void*	old_tp_buf;		// pointer to the old tp buffer
	int		free_tp = 1;	// free the transport buffer (old) when done (when not cloning)
	int		old_mt;			// msg type and sub-id from the message passed in
	int		old_sid;
	int		old_len;
	int		old_rfd;		// rts file descriptor from old message

	if( old_msg == NULL || payload_len <= 0 ) {
		errno = EINVAL;
		return NULL;
	}

	old_mt = old_msg->mtype;			// preserve mbuf info
	old_sid = old_msg->sub_id;
	old_len = old_msg->len;
	old_rfd = old_msg->rts_fd;

	old_psize = old_msg->alloc_len - (RMR_HDR_LEN( old_msg->header ) + TP_HDR_LEN);		// user payload size in orig message

	if( !clone  && payload_len <= old_psize ) {										// not cloning and old is large enough; nothing to do
		if( DEBUG ) fprintf( stderr, "[DBUG] rmr_realloc_payload: old msg payload larger than requested: cur=%d need=%d\n", old_psize, payload_len );
		return old_msg;
	}

	hdr_len = RMR_HDR_LEN( old_msg->header ) + TP_HDR_LEN;				// with SI we manage the transport header; must include in len
	old_tp_buf = old_msg->tp_buf;

	if( clone ) {
		if( DEBUG ) fprintf( stderr, "[DBUG] rmr_realloc_payload: cloning message\n" );
		free_tp = 0;

		nm = (rmr_mbuf_t *) malloc( sizeof( *nm ) );
		if( nm == NULL ) {
			fprintf( stderr, "[CRI] rmr_realloc_payload: cannot get memory for message buffer. bytes requested: %d\n", (int) sizeof(*nm) );
			return NULL;
		}
		memset( nm, 0, sizeof( *nm ) );
		nm->rts_fd = old_rfd;				// this is managed only in the mbuf; dup now
	} else {
		nm = old_msg;
	}

	omhdr = old_msg->header;
	mlen = hdr_len + (payload_len > old_psize ? payload_len : old_psize);		// must have larger in case copy is true

	if( DEBUG ) fprintf( stderr, "[DBUG] reallocate for payload increase. new message size: %d\n", (int) mlen );	
	if( (nm->tp_buf = (char *) malloc( sizeof( char ) * mlen )) == NULL ) {
		fprintf( stderr, "[CRI] rmr_realloc_payload: cannot get memory for zero copy buffer. bytes requested: %d\n", (int) mlen );
		return NULL;
	}

	nm->header = ((char *) nm->tp_buf) + TP_HDR_LEN;			// point at the new header and copy from old
	SET_HDR_LEN( nm->header );

	if( copy ) {																// if we need to copy the old payload too
		if( DEBUG ) fprintf( stderr, "[DBUG] rmr_realloc_payload: copy payload into new message: %d bytes\n", old_psize );
		memcpy( nm->header, omhdr, sizeof( char ) * (old_psize + RMR_HDR_LEN( omhdr )) );
	} else {																	// just need to copy header
		if( DEBUG ) fprintf( stderr, "[DBUG] rmr_realloc_payload: copy only header into new message: %d bytes\n", RMR_HDR_LEN( nm->header ) );
		memcpy( nm->header, omhdr, sizeof( char ) * RMR_HDR_LEN( omhdr ) );
	}

	ref_tpbuf( nm, mlen );			// set payload and other pointers in the message to the new tp buffer

	if( !copy ) {
		nm->mtype = -1;						// didn't copy payload, so mtype, sub-id, and rts fd are invalid
		nm->sub_id = -1;
		nm->len = 0;						// and len is 0
	} else {
		nm->len = old_len;					// we must force these to avoid losing info if msg wasn't a received message
		nm->mtype = old_mt;
		nm->sub_id = old_sid;
	}

	if( free_tp ) {
		free( old_tp_buf );				// we did not clone, so free b/c no references
	}

	return nm;
}

/*
	For SI95 based transport all receives are driven through the threaded
	ring and thus this function should NOT be called. If it is we will panic
	and abort straight away.
*/
static rmr_mbuf_t* rcv_msg( uta_ctx_t* ctx, rmr_mbuf_t* old_msg ) {

fprintf( stderr, "\n\n>>> rcv_msg: bad things just happened!\n\n>>>>>> abort!  rcv_msg called and it shouldn't be\n" );
exit( 1 );

	return NULL;
}

/*
	Receives a 'raw' message from a non-RMr sender (no header expected). The returned
	message buffer cannot be used to send, and the length information may or may
	not be correct (it is set to the length received which might be more than the
	bytes actually in the payload).

	Mostly this supports the route table collector, but could be extended with an
	API external function.
*/
static void* rcv_payload( uta_ctx_t* ctx, rmr_mbuf_t* old_msg ) {
	return NULL;
/*
FIXME: do we need this in the SI world?  The only user was the route table collector
	int state;
	rmr_mbuf_t*	msg = NULL;		// msg received
	size_t	rsize;				// nng needs to write back the size received... grrr

	if( old_msg ) {
		msg = old_msg;
	} else {
		msg = alloc_zcmsg( ctx, NULL, RMR_MAX_RCV_BYTES, RMR_OK, DEF_TR_LEN );			// will abort on failure, no need to check
	}

	//msg->state = nng_recvmsg( ctx->nn_sock, (nng_msg **) &msg->tp_buf, NO_FLAGS );			// blocks hard until received
	if( (msg->state = xlate_si_state( msg->state, RMR_ERR_RCVFAILED )) != RMR_OK ) {
		return msg;
	}
	rsize = nng_msg_len( msg->tp_buf );

	// do NOT use ref_tpbuf() here! Must fill these in manually.
	msg->header = nng_msg_body( msg->tp_buf );
	msg->len = rsize;							// len is the number of bytes received
	msg->alloc_len = rsize;
	msg->mtype = UNSET_MSGTYPE;					// raw message has no type
	msg->sub_id = UNSET_SUBID;					// nor a subscription id
	msg->state = RMR_OK;
	msg->flags = MFL_RAW;
	msg->payload = msg->header;					// payload is the whole thing; no header
	msg->xaction = NULL;

	if( DEBUG > 1 ) fprintf( stderr, "[DBUG] rcv_payload: got something: type=%d state=%d len=%d\n", msg->mtype, msg->state, msg->len );

	return msg;
*/
}

/*
	This does the hard work of actually sending the message to the given socket. On success,
	a new message struct is returned. On error, the original msg is returned with the state
	set to a reasonable value. If the message being sent as MFL_NOALLOC set, then a new
	buffer will not be allocated and returned (mostly for call() interal processing since
	the return message from call() is a received buffer, not a new one).

	Called by rmr_send_msg() and rmr_rts_msg(), etc. and thus we assume that all pointer
	validation has been done prior.

	When msg->state is not ok, this function must set tp_state in the message as some API 
	fucntions return the message directly and do not propigate errno into the message.
*/
static rmr_mbuf_t* send_msg( uta_ctx_t* ctx, rmr_mbuf_t* msg, int nn_sock, int retries ) {
	int state;
	uta_mhdr_t*	hdr;
	int spin_retries = 1000;				// if eagain/timeout we'll spin, at max, this many times before giving up the CPU
	int	tr_len;								// trace len in sending message so we alloc new message with same trace sizes
	int tot_len;							// total send length (hdr + user data + tp header)

	// future: ensure that application did not overrun the XID buffer; last byte must be 0

	hdr = (uta_mhdr_t *) msg->header;
	hdr->mtype = htonl( msg->mtype );								// stash type/len/sub_id in network byte order for transport
	hdr->sub_id = htonl( msg->sub_id );
	hdr->plen = htonl( msg->len );
	tr_len = RMR_TR_LEN( hdr );										// snarf trace len before sending as hdr is invalid after send

	if( msg->flags & MFL_ADDSRC ) {									// buffer was allocated as a receive buffer; must add our source
		strncpy( (char *) ((uta_mhdr_t *)msg->header)->src, ctx->my_name, RMR_MAX_SRC );					// must overlay the source to be ours
		strncpy( (char *) ((uta_mhdr_t *)msg->header)->srcip, ctx->my_ip, RMR_MAX_SRC );
	}

	if( retries == 0 ) {
		spin_retries = 100;
		retries++;
	}

	errno = 0;
	msg->state = RMR_OK;
	do {
		tot_len = msg->len + PAYLOAD_OFFSET( hdr ) + TP_HDR_LEN;			// we only send what was used + header lengths
		*((int*) msg->tp_buf) = tot_len;

		if( DEBUG > 1 ) fprintf( stderr, "[DEBUG] send_msg: ending %d (%x) bytes  usr_len=%d alloc=%d retries=%d\n", tot_len, tot_len, msg->len, msg->alloc_len, retries );
		if( DEBUG > 2 ) dump_40( msg->tp_buf, "sending" );

		if( (state = SIsendt( ctx->si_ctx, nn_sock, msg->tp_buf, tot_len )) != SI_OK ) {
			if( DEBUG > 1 ) fprintf( stderr, "[DBUG] send_msg:  error!! sent state=%d\n", state );
			msg->state = state;
			if( retries > 0 && state == SI_ERR_BLOCKED ) {
				if( --spin_retries <= 0 ) {				// don't give up the processor if we don't have to
					retries--;
					if( retries > 0 ) {					// only if we'll loop through again
						usleep( 1 );					// sigh, give up the cpu and hope it's just 1 miscrosec
					}
					spin_retries = 1000;
				}
			} else {
				state = 0;			// don't loop
			}
		} else {
			if( DEBUG > 2 ) fprintf( stderr, "[DBUG] sent OK state=%d\n", state );
			state = 0;
			msg->state = RMR_OK;
			hdr = NULL;
		}
	} while( state && retries > 0 );

	if( msg->state == RMR_OK ) {									// successful send
		if( !(msg->flags & MFL_NOALLOC) ) {							// allocate another sendable zc buffer unless told otherwise
			return alloc_zcmsg( ctx, msg, 0, RMR_OK, tr_len );		// preallocate a zero-copy buffer and return msg
		} else {
			rmr_free_msg( msg );						// not wanting a meessage back, trash this one
			return NULL;
		}
	} else {											// send failed -- return original message
		if( msg->state == 98 ) {		// FIX ME: this is just broken, but needs SI changes to work correctly for us
			errno = EAGAIN;
			msg->state = RMR_ERR_RETRY;					// errno will have nano reason
		} else {
			msg->state = RMR_ERR_SENDFAILED;
		}

		if( DEBUG ) fprintf( stderr, "[DBUG] send failed: %d %s\n", (int) msg->state, strerror( msg->state ) );
	}

	return msg;
}

/*
	send message with maximum timeout.
	Accept a message and send it to an endpoint based on message type.
	If NNG reports that the send attempt timed out, or should be retried,
	RMr will retry for approximately max_to microseconds; rounded to the next
	higher value of 10.

	Allocates a new message buffer for the next send. If a message type has
	more than one group of endpoints defined, then the message will be sent
	in round robin fashion to one endpoint in each group.

	An endpoint will be looked up in the route table using the message type and
	the subscription id. If the subscription id is "UNSET_SUBID", then only the
	message type is used.  If the initial lookup, with a subid, fails, then a
	second lookup using just the mtype is tried.

	When msg->state is not OK, this function must set tp_state in the message as 
	some API fucntions return the message directly and do not propigate errno into 
	the message.

	CAUTION: this is a non-blocking send.  If the message cannot be sent, then
		it will return with an error and errno set to eagain. If the send is
		a limited fanout, then the returned status is the status of the last
		send attempt.

*/
static  rmr_mbuf_t* mtosend_msg( void* vctx, rmr_mbuf_t* msg, int max_to ) {
	endpoint_t*	ep;					// end point that we're attempting to send to
	rtable_ent_t*	rte;			// the route table entry which matches the message key
	int	nn_sock;					// endpoint socket (fd in si case) for send
	uta_ctx_t*	ctx;
	int			group;				// selected group to get socket for
	int			send_again;			// true if the message must be sent again
	rmr_mbuf_t*	clone_m;			// cloned message for an nth send
	int		 	sock_ok;			// got a valid socket from round robin select
	char*		d1;
	int			ok_sends = 0;		// track number of ok sends

	if( (ctx = (uta_ctx_t *) vctx) == NULL || msg == NULL ) {		// bad stuff, bail fast
		errno = EINVAL;												// if msg is null, this is their clue
		if( msg != NULL ) {
			msg->state = RMR_ERR_BADARG;
			errno = EINVAL;											// must ensure it's not eagain
			msg->tp_state = errno;
		}
		return msg;
	}

	errno = 0;													// clear; nano might set, but ensure it's not left over if it doesn't
	if( msg->header == NULL ) {
		fprintf( stderr, "rmr_mtosend_msg: ERROR: message had no header\n" );
		msg->state = RMR_ERR_NOHDR;
		errno = EBADMSG;											// must ensure it's not eagain
		msg->tp_state = errno;
		return msg;
	}

	if( max_to < 0 ) {
		max_to = ctx->send_retries;		// convert to retries
	}

	if( (rte = uta_get_rte( ctx->rtable, msg->sub_id, msg->mtype, TRUE )) == NULL ) {		// find the entry which matches subid/type allow fallback to type only key
		if( ctx->flags & CTXFL_WARN ) {
			fprintf( stderr, "[WARN] no route table entry for mtype=%d sub_id=%d\n", msg->mtype, msg->sub_id );
		}
		msg->state = RMR_ERR_NOENDPT;
		errno = ENXIO;										// must ensure it's not eagain
		msg->tp_state = errno;
		return msg;											// caller can resend (maybe) or free
	}

	send_again = 1;											// force loop entry
	group = 0;												// always start with group 0
	while( send_again ) {
		sock_ok = uta_epsock_rr( rte, group, &send_again, &nn_sock, &ep, ctx->si_ctx );		// select endpt from rr group and set again if more groups

		if( DEBUG ) fprintf( stderr, "[DBUG] mtosend_msg: flgs=0x%04x type=%d again=%d group=%d len=%d sock_ok=%d\n",
				msg->flags, msg->mtype, send_again, group, msg->len, sock_ok );

		group++;

		if( sock_ok ) {													// with an rte we _should_ always have a socket, but don't bet on it
			if( send_again ) {
				clone_m = clone_msg( msg );								// must make a copy as once we send this message is not available
				if( clone_m == NULL ) {
					msg->state = RMR_ERR_SENDFAILED;
					errno = ENOMEM;
					msg->tp_state = errno;
					if( ctx->flags & CTXFL_WARN ) {
						fprintf( stderr, "[WARN] unable to clone message for multiple rr-group send\n" );
					}
					return msg;
				}

				if( DEBUG ) fprintf( stderr, "[DBUG] msg cloned: type=%d len=%d\n", msg->mtype, msg->len );
				msg->flags |= MFL_NOALLOC;								// keep send from allocating a new message; we have a clone to use
				msg = send_msg( ctx, msg, nn_sock, max_to );			// do the hard work, msg should be nil on success
	
				if( msg != NULL ) {										// returned message indicates send error of some sort
					rmr_free_msg( msg );								// must ditchone; pick msg so we don't have to unfiddle flags
					msg = clone_m;
				} else {
					ok_sends++;
					msg = clone_m;										// clone will be the next to send
				}
			} else {
				msg = send_msg( ctx, msg, nn_sock, max_to );			// send the last, and allocate a new buffer; drops the clone if it was
				if( DEBUG ) {
					if( msg == NULL ) {
						fprintf( stderr, "[DBUG] mtosend_msg:  send returned nil message!\n" );		
					}
				}
			}

			if( ep != NULL && msg != NULL ) {
				switch( msg->state ) {
					case RMR_OK:
						ep->scounts[EPSC_GOOD]++;
						break;
				
					case RMR_ERR_RETRY:
						ep->scounts[EPSC_TRANS]++;
						break;

					default:
						ep->scounts[EPSC_FAIL]++;
						uta_ep_failed( ep );								// sending to ep failed; set up to reconnect
						break;
				}
			}
		} else {
/*
			if( ctx->flags & CTXFL_WARN ) {
				fprintf( stderr, "[WARN] invalid socket for rte, setting no endpoint err: mtype=%d sub_id=%d\n", msg->mtype, msg->sub_id );
			}
*/
			msg->state = RMR_ERR_NOENDPT;
			errno = ENXIO;
		}
	}

	if( msg ) {							// call functions don't get a buffer back, so a nil check is required
		msg->flags &= ~MFL_NOALLOC;		// must return with this flag off
		if( ok_sends ) {				// multiple rr-groups and one was successful; report ok
			msg->state = RMR_OK;
		}
	
		if( DEBUG ) fprintf( stderr, "[DBUG] final send stats: ok=%d group=%d state=%d\n\n", ok_sends, group, msg->state );
	
		msg->tp_state = errno;
	}

	return msg;									// last message caries the status of last/only send attempt
}


/*
	A generic wrapper to the real send to keep wormhole stuff agnostic.
	We assume the wormhole function vetted the buffer so we don't have to.
*/
static rmr_mbuf_t* send2ep( uta_ctx_t* ctx, endpoint_t* ep, rmr_mbuf_t* msg ) {
	return send_msg( ctx, msg, ep->nn_sock, -1 );
}

#endif
