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
	Mnemonic:	rtable_si_static.c
	Abstract:	Route table management functions which depend on the underlying
				transport mechanism and thus cannot be included with the generic
				route table functions.

				This module is designed to be included by any module (main) needing
				the static/private stuff.

	Author:		E. Scott Daniels
	Date:		29 November 2018
*/

#ifndef rtable_static_c
#define	rtable_static_c

#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


// -----------------------------------------------------------------------------------------------------

/*
	Mark an endpoint closed because it's in a failing state.
*/
static void uta_ep_failed( endpoint_t* ep ) {
	if( ep != NULL ) {
		if( DEBUG ) fprintf( stderr, "[DBUG] connection to %s was closed\n", ep->name );
		ep->open = 0;
	}
}

/*
	Establish a TCP connection to the indicated target (IP address).
	Target assumed to be address:port.  The new socket is returned via the
	user supplied pointer so that a success/fail code is returned directly.
	Return value is 0 (false) on failure, 1 (true)  on success.

	In order to support multi-threaded user applications we must hold a lock before
	we attempt to create a dialer and connect. NNG is thread safe, but we can
	get things into a bad state if we allow a collision here.  The lock grab
	only happens on the intial session setup.
*/
static int uta_link2( si_ctx_t* si_ctx, endpoint_t* ep ) {
	static int	flags = 0;

	char* 		target;
	char		conn_info[SI_MAX_ADDR_LEN];	// string to give to nano to make the connection
	char*		addr;
	int			state = FALSE;
	char*		tok;

	if( ep == NULL ) {
		if( DEBUG ) fprintf( stderr, "[DBUG] link2 ep was nil!\n" );
		return FALSE;
	}

	target = ep->name;				// always give name to transport so changing dest IP does not break reconnect
	if( target == NULL  ||  (addr = strchr( target, ':' )) == NULL ) {		// bad address:port
		if( ep->notify ) {
			fprintf( stderr, "[WARN] rmr: link2: unable to create link: bad target: %s\n", target == NULL ? "<nil>" : target );
			ep->notify = 0;
		}
		return FALSE;
	}

	pthread_mutex_lock( &ep->gate );			// grab the lock
	if( ep->open ) {
		pthread_mutex_unlock( &ep->gate );
		return TRUE;
	}

	snprintf( conn_info, sizeof( conn_info ), "%s", target );
	errno = 0;
	if( DEBUG > 1 ) fprintf( stderr, "[DBUG] link2 attempting connection with: %s\n", conn_info );
	if( (ep->nn_sock = SIconnect( si_ctx, conn_info )) < 0 ) {
		pthread_mutex_unlock( &ep->gate );

		if( ep->notify ) {							// need to notify if set
			fprintf( stderr, "[WRN] rmr: link2: unable to connect  to target: %s: %d %s\n", target, errno, strerror( errno ) );
			ep->notify = 0;
		}
		//nng_close( *nn_sock );
		return FALSE;
	}

	if( DEBUG ) fprintf( stderr, "[INFO] rmr_si_link2: connection was successful to: %s\n", target );

	ep->open = TRUE;						// set open/notify before giving up lock

	if( ! ep->notify ) {						// if we yammered about a failure, indicate finally good
		fprintf( stderr, "[INFO] rmr: link2: connection finally establisehd with target: %s\n", target );
		ep->notify = 1;
	}

	pthread_mutex_unlock( &ep->gate );
	return TRUE;
}

/*
	This provides a protocol independent mechanism for establishing the connection to an endpoint.
	Return is true (1) if the link was opened; false on error.
*/
static int rt_link2_ep( void* vctx, endpoint_t* ep ) {
	uta_ctx_t* ctx;

	if( ep == NULL ) {
		return FALSE;
	}

	if( ep->open )  {			// already open, do nothing
		return TRUE;
	}

	if( (ctx = (uta_ctx_t *) vctx) == NULL ) {
		return FALSE;
	}

	uta_link2( ctx->si_ctx, ep );
	return ep->open;
}


/*
	Add an endpoint to a route table entry for the group given. If the endpoint isn't in the
	hash we add it and create the endpoint struct.

	The caller must supply the specific route table (we assume it will be pending, but they
	could live on the edge and update the active one, though that's not at all a good idea).
*/
extern endpoint_t*  uta_add_ep( route_table_t* rt, rtable_ent_t* rte, char* ep_name, int group  ) {
	endpoint_t*	ep;
	rrgroup_t* rrg;				// pointer at group to update

	if( ! rte || ! rt ) {
		fprintf( stderr, "[WRN] uda_add_ep didn't get a valid rt and/or rte pointer\n" );
		return NULL;
	}

	if( rte->nrrgroups <= group || group < 0 ) {
		fprintf( stderr, "[WRN] uda_add_ep group out of range: %d (max == %d)\n", group, rte->nrrgroups );
		return NULL;
	}

	//fprintf( stderr, ">>>> add ep grp=%d to rte @ 0x%p  rrg=%p\n", group, rte, rte->rrgroups[group] );
	if( (rrg = rte->rrgroups[group]) == NULL ) {
		if( (rrg = (rrgroup_t *) malloc( sizeof( *rrg ) )) == NULL ) {
			fprintf( stderr, "[WRN] rmr_add_ep: malloc failed for round robin group: group=%d\n", group );
			return NULL;
		}
		memset( rrg, 0, sizeof( *rrg ) );

		if( (rrg->epts = (endpoint_t **) malloc( sizeof( endpoint_t ) * MAX_EP_GROUP )) == NULL ) {
			fprintf( stderr, "[WRN] rmr_add_ep: malloc failed for group endpoint array: group=%d\n", group );
			return NULL;
		}
		memset( rrg->epts, 0, sizeof( endpoint_t ) * MAX_EP_GROUP );

		rte->rrgroups[group] = rrg;
		//fprintf( stderr, ">>>> added new rrg grp=%d to rte @ 0x%p  rrg=%p\n", group, rte, rte->rrgroups[group] );

		rrg->ep_idx = 0;						// next endpoint to send to
		rrg->nused = 0;							// number populated
		rrg->nendpts = MAX_EP_GROUP;			// number allocated

		if( DEBUG > 1 ) fprintf( stderr, "[DBUG] rrg added to rte: mtype=%d group=%d\n", rte->mtype, group );
	}

	ep = rt_ensure_ep( rt, ep_name ); 			// get the ep and create one if not known

	if( rrg != NULL ) {
		if( rrg->nused >= rrg->nendpts ) {
			// future: reallocate
			fprintf( stderr, "[WRN] endpoint array for mtype/group %d/%d is full!\n", rte->mtype, group );
			return NULL;
		}

		rrg->epts[rrg->nused] = ep;
		rrg->nused++;
	}

	if( DEBUG > 1 ) fprintf( stderr, "[DBUG] endpoint added to mtype/group: %d/%d %s nused=%d\n", rte->mtype, group, ep_name, rrg->nused );
	return ep;
}


/*
	Given a name, find the nano socket needed to send to it. Returns the socket via
	the user pointer passed in and sets the return value to true (1). If the
	endpoint cannot be found false (0) is returned.
*/
static int uta_epsock_byname( route_table_t* rt, char* ep_name, int* nn_sock, endpoint_t** uepp, si_ctx_t* si_ctx ) {
	endpoint_t* ep;
	int state = FALSE;

	if( rt == NULL ) {
		return FALSE;
	}

	ep =  rmr_sym_get( rt->hash, ep_name, 1 );
	if( uepp != NULL ) {							// caller needs endpoint too, give it back
		*uepp = ep;
	}
	if( ep == NULL ) {
		if( DEBUG ) fprintf( stderr, "[DBUG] get ep by name for %s not in hash!\n", ep_name );
		if( ! ep_name || (ep = rt_ensure_ep( rt, ep_name)) == NULL ) {				// create one if not in rt (support rts without entry in our table)
			return FALSE;
		}
	}

	if( ! ep->open )  {										// not open -- connect now
		if( DEBUG ) fprintf( stderr, "[DBUG] get ep by name for %s session not started... starting\n", ep_name );
		if( ep->addr == NULL ) {					// name didn't resolve before, try again
			ep->addr = strdup( ep->name );			// use the name directly; if not IP then transport will do dns lookup
		}
		if( uta_link2( si_ctx, ep ) ) {											// find entry in table and create link
			state = TRUE;
			ep->open = TRUE;
			*nn_sock = ep->nn_sock;							// pass socket back to caller
		}
		if( DEBUG ) fprintf( stderr, "[DBUG] epsock_bn: connection state: %s %s\n", state ? "[OK]" : "[FAIL]", ep->name );
	} else {
		*nn_sock = ep->nn_sock;
		state = TRUE;
	}

	return state;
}

/*
	Make a round robin selection within a round robin group for a route table
	entry. Returns the nanomsg socket if there is a rte for the message
	key, and group is defined. Socket is returned via pointer in the parm
	list (nn_sock).

	The group is the group number to select from.

	The user supplied (via pointer to) integer 'more' will be set if there are
	additional groups beyond the one selected. This allows the caller to
	to easily iterate over the group list -- more is set when the group should
	be incremented and the function invoked again. Groups start at 0.

	The return value is true (>0) if the socket was found and *nn_sock was updated
	and false (0) if there is no associated socket for the msg type, group combination.
	We return the index+1 from the round robin table on success so that we can verify
	during test that different entries are being seleted; we cannot depend on the nng
	socket being different as we could with nano.

	NOTE:	The round robin selection index increment might collide with other
		threads if multiple threads are attempting to send to the same round
		robin group; the consequences are small and avoid locking. The only side
		effect is either sending two messages in a row to, or skipping, an endpoint.
		Both of these, in the grand scheme of things, is minor compared to the
		overhead of grabbing a lock on each call.
*/
static int uta_epsock_rr( rtable_ent_t *rte, int group, int* more, int* nn_sock, endpoint_t** uepp, si_ctx_t* si_ctx ) {
	endpoint_t*	ep;				// seected end point
	int  state = FALSE;			// processing state
	int dummy;
	rrgroup_t* rrg;
	int	idx;

	//fprintf( stderr, ">>>> epsock_rr selecting: grp=%d mtype=%d ngrps=%d\n", group, rte->mtype, rte->nrrgroups );

	if( ! more ) {				// eliminate cheks each time we need to use
		more = &dummy;
	}

	if( ! nn_sock ) {			// user didn't supply a pointer
		if( DEBUG ) fprintf( stderr, "[DBUG] epsock_rr invalid nnsock pointer\n" );
		errno = EINVAL;
		*more = 0;
		return FALSE;
	}

	if( rte == NULL ) {
		if( DEBUG ) fprintf( stderr, "[DBUG] epsock_rr rte was nil; nothing selected\n" );
		*more = 0;
		return FALSE;
	}

	if( group < 0 || group >= rte->nrrgroups ) {
		if( DEBUG > 1 ) fprintf( stderr, "[DBUG] group out of range: group=%d max=%d\n", group, rte->nrrgroups );
		*more = 0;
		return FALSE;
	}

	if( (rrg = rte->rrgroups[group]) == NULL ) {
		if( DEBUG > 1 ) fprintf( stderr, "[DBUG] rrg not found for group %d (ptr rrgroups[g] == nil)\n", group );
		*more = 0; 					// groups are inserted contig, so nothing should be after a nil pointer
		return FALSE;
	}

	*more = group < rte->nrrgroups-1 ? (rte->rrgroups[group+1] != NULL): 0;	// more if something in next group slot

	switch( rrg->nused ) {
		case 0:				// nothing allocated, just punt
			if( DEBUG > 1 ) fprintf( stderr, "[DBUG] nothing allocated for the rrg\n" );
			return FALSE;

		case 1:				// exactly one, no rr to deal with
			ep = rrg->epts[0];
			if( DEBUG > 1 ) fprintf( stderr, "[DBUG] _rr returning socket with one choice in group \n" );
			state = TRUE;
			break;

		default:										// need to pick one and adjust rr counts
			idx = rrg->ep_idx++ % rrg->nused;			// see note above
			ep = rrg->epts[idx];						// select next endpoint
			if( DEBUG > 1 ) fprintf( stderr, "[DBUG] _rr returning socket with multiple choices in group idx=%d \n", rrg->ep_idx );
			state = idx + 1;							// unit test checks to see that we're cycling through, so must not just be TRUE
			break;
	}

	if( uepp != NULL ) {								// caller may need refernce to endpoint too; give it if pointer supplied
		*uepp = ep;
	}
	if( state ) {										// end point selected, open if not, get socket either way
		if( ! ep->open ) {								// not connected
			if( DEBUG ) fprintf( stderr, "[DBUG] epsock_rr selected endpoint not yet open; opening %s\n", ep->name );
			if( ep->addr == NULL ) {					// name didn't resolve before, try again
				ep->addr = strdup( ep->name );			// use the name directly; if not IP then transport will do dns lookup
			}

			if( uta_link2( si_ctx, ep ) ) {											// find entry in table and create link
				ep->open = TRUE;
				*nn_sock = ep->nn_sock;							// pass socket back to caller
			} else {
				state = FALSE;
			}
			if( DEBUG ) fprintf( stderr, "[DBUG] epsock_rr: connection attempted with %s: %s\n", ep->name, state ? "[OK]" : "[FAIL]" );
		} else {
			*nn_sock = ep->nn_sock;
		}
	}

	if( DEBUG > 1 ) fprintf( stderr, "[DBUG] epsock_rr returns state=%d\n", state );
	return state;
}

/*
	Finds the rtable entry which matches the key. Returns a nil pointer if
	no entry is found. If try_alternate is set, then we will attempt 
	to find the entry with a key based only on the message type.
*/
static inline rtable_ent_t*  uta_get_rte( route_table_t *rt, int sid, int mtype, int try_alt ) {
	uint64_t key;			// key is sub id and mtype banged together
	rtable_ent_t* rte;		// the entry we found

	if( rt == NULL || rt->hash == NULL ) {
		return NULL;
	}

	key = build_rt_key( sid, mtype );											// first try with a 'full' key
	if( ((rte = rmr_sym_pull( rt->hash, key )) != NULL)  ||  ! try_alt ) {		// found or not allowed to try the alternate, return what we have
		return rte;
	}

	if( sid != UNSET_SUBID ) {								// not found, and allowed to try alternate; and the sub_id was set
		key = build_rt_key( UNSET_SUBID, mtype );			// rebuild key
		rte = rmr_sym_pull( rt->hash, key );				// see what we get with this
	}

	return rte;
}

/*
	Return a string of count information. E.g.:
		<ep-name>:<port> <good> <hard-fail> <soft-fail>

	Caller must free the string allocated if a buffer was not provided.

	Pointer returned is to a freshly allocated string, or the user buffer
	for convenience.

	If the endpoint passed is a nil pointer, then we return a nil -- caller
	must check!
*/
static inline char* get_ep_counts( endpoint_t* ep, char* ubuf, int ubuf_len ) {
	char*	rs;			// result string

	if( ep == NULL ) {
		return NULL;
	}

	if( ubuf != NULL ) {
		rs = ubuf;
	} else {
		ubuf_len = 256;
		rs = malloc( sizeof( char ) * ubuf_len );
	}

	snprintf( rs, ubuf_len, "%s %lld %lld %lld", ep->name, ep->scounts[EPSC_GOOD], ep->scounts[EPSC_FAIL], ep->scounts[EPSC_TRANS] );

	return rs;
}

#endif
