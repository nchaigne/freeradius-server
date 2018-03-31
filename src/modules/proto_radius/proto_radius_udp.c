/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file proto_radius_udp.c
 * @brief RADIUS handler for UDP.
 *
 * @copyright 2016 The FreeRADIUS server project.
 * @copyright 2016 Alan DeKok (aland@deployingradius.com)
 */
#include <netdb.h>
#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/protocol.h>
#include <freeradius-devel/udp.h>
#include <freeradius-devel/trie.h>
#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/io/io.h>
#include <freeradius-devel/io/application.h>
#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/io/schedule.h>
#include <freeradius-devel/rad_assert.h>
#include "proto_radius.h"
#include "track.h"

typedef struct {
	int				if_index;

	fr_ipaddr_t			src_ipaddr;
	fr_ipaddr_t			dst_ipaddr;
	uint16_t			src_port;
	uint16_t 			dst_port;

	uint8_t				code;			//!< for duplicate detection
	uint8_t				id;			//!< for duplicate detection

	RADCLIENT			*client;
} proto_radius_udp_address_t;

typedef struct fr_radius_dynamic_client_t {
	dl_instance_t			*submodule;		//!< proto_radius_dynamic_client
	fr_ipaddr_t			*network;		//!< dynamic networks to allow
	fr_trie_t			*trie;			//!< track networks for dynamic clients

	RADCLIENT_LIST			*clients;		//!< local clients
	RADCLIENT_LIST			*pending;		//!< pending local clients
	RADCLIENT_LIST			*negative;		//!< negative cache of rejected clients

	fr_dlist_t			packets;       		//!< list of accepted packets

	uint32_t			max_clients;		//!< maximum number of dynamic clients
	uint32_t			num_clients;		//!< total number of active clients
	uint32_t			num_negative_clients;	//!< how many clients are in the negative cache
	uint32_t			max_pending_clients;	//!< maximum number of pending clients
	uint32_t			num_pending_clients;	//!< number of pending clients
	uint32_t			max_pending_packets;	//!< maximum accepted pending packets
	uint32_t			num_pending_packets;	//!< how many packets are received, but not accepted

	uint32_t			idle_timeout;		//!< of the dynamic client, in seconds.
} fr_radius_dynamic_client_t;

#ifdef HAVE_PTHREAD_H
#define PTHREAD_MUTEX_LOCK   pthread_mutex_lock
#define PTHREAD_MUTEX_UNLOCK pthread_mutex_unlock

#else
#define PTHREAD_MUTEX_LOCK
#define PTHREAD_MUTEX_UNLOCK
#endif

/*
 *	Structure only in the master.
 */
typedef struct proto_radius_udp_master_t {
	dl_instance_t const    		*parent_dl_inst;	//!< for reloading ourselves
	TALLOC_CTX			*ctx;			//!< for the hash table
	fr_hash_table_t			*ht;			//!< for child sockets
#ifdef HAVE_PTHREAD_H
	pthread_mutex_t			mutex;			//!< so the children can remove themselves from the table
#endif
} proto_radius_udp_master_t;

/*
 *	Structure only in the child.
 */
typedef struct proto_radius_udp_child_t {
	fr_ipaddr_t			src_ipaddr;		//!< source IP for connected sockets
	uint16_t			src_port;      		//!< Source port for connected sockets.
	int				if_index;		//!< index of receiving interface

	uint8_t const			*packet;		//!< for injected packets
	size_t				packet_len;		//!< length of the injected packet
	fr_time_t			recv_time;		//!< of the packet
	RADCLIENT			*client;		//!< static client for this connection
	dl_instance_t	  		*dl_inst;		//!< our library instance
	fr_listen_t			*listen;		//!< listener for this socket
	fr_event_timer_t const		*ev;			//!< when we clean up the child socket.
	struct proto_radius_udp_t	*master;		//!< for the master socket.
} proto_radius_udp_child_t;

typedef struct proto_radius_udp_t {
	proto_radius_t	const		*parent;		//!< The module that spawned us!
	char const			*name;			//!< socket name

	int				sockfd;

	fr_event_list_t			*el;			//!< for cleanup timers on Access-Request
	fr_network_t			*nr;			//!< for fr_network_listen_read();

	fr_ipaddr_t			ipaddr;			//!< IP address to listen on.

	char const			*interface;		//!< Interface to bind to.
	char const			*port_name;		//!< Name of the port for getservent().

	uint32_t			recv_buff;		//!< How big the kernel's receive buffer should be.

	fr_tracking_t			*ft;			//!< tracking table
	uint32_t			cleanup_delay;		//!< cleanup delay for Access-Request packets

	CONF_SECTION			*cs;			//!< mainly for connected sockets
	fr_stats_t			stats;			//!< statistics for this socket

	fr_radius_dynamic_client_t	dynamic_clients;	//!< dynamic client infromation

	uint16_t			port;			//!< Port to listen on.

	bool				dynamic_clients_is_set;	//!< set if we have dynamic clients
	bool				recv_buff_is_set;	//!< Whether we were provided with a receive
								//!< buffer value.
	bool				use_connected;		//!< do we use connected sockets
	bool				connected;		//!< is this a connected socket?

	uint32_t	       		max_connections;	//!< maximum number of child connections we allow

	union {
		proto_radius_udp_child_t	child;			//!< information only for the child
		proto_radius_udp_master_t	master;			//!< information only for the master
	};

	uint32_t			priorities[FR_MAX_PACKET_CODE];	//!< priorities for individual packets
} proto_radius_udp_t;


typedef struct dynamic_packet_t {
	uint8_t			*packet;
	fr_time_t		timestamp;
	fr_tracking_entry_t	*track;
	fr_dlist_t		entry;
} dynamic_packet_t;

static const CONF_PARSER dynamic_client_config[] = {
	{ FR_CONF_OFFSET("network", FR_TYPE_COMBO_IP_PREFIX | FR_TYPE_MULTI, fr_radius_dynamic_client_t, network) },

	{ FR_CONF_OFFSET("max_clients", FR_TYPE_UINT32, fr_radius_dynamic_client_t, max_clients), .dflt = "65536" },
	{ FR_CONF_OFFSET("max_pending_clients", FR_TYPE_UINT32, fr_radius_dynamic_client_t, max_pending_clients), .dflt = "256" },
	{ FR_CONF_OFFSET("max_pending_packets", FR_TYPE_UINT32, fr_radius_dynamic_client_t, max_pending_packets), .dflt = "65536" },

	{ FR_CONF_OFFSET("idle_timeout", FR_TYPE_UINT32, fr_radius_dynamic_client_t, idle_timeout), .dflt = "600" },

	CONF_PARSER_TERMINATOR
};


static const CONF_PARSER udp_listen_config[] = {
	{ FR_CONF_OFFSET("ipaddr", FR_TYPE_COMBO_IP_ADDR, proto_radius_udp_t, ipaddr) },
	{ FR_CONF_OFFSET("ipv4addr", FR_TYPE_IPV4_ADDR, proto_radius_udp_t, ipaddr) },
	{ FR_CONF_OFFSET("ipv6addr", FR_TYPE_IPV6_ADDR, proto_radius_udp_t, ipaddr) },

	{ FR_CONF_OFFSET("interface", FR_TYPE_STRING, proto_radius_udp_t, interface) },
	{ FR_CONF_OFFSET("port_name", FR_TYPE_STRING, proto_radius_udp_t, port_name) },

	{ FR_CONF_OFFSET("port", FR_TYPE_UINT16, proto_radius_udp_t, port) },
	{ FR_CONF_IS_SET_OFFSET("recv_buff", FR_TYPE_UINT32, proto_radius_udp_t, recv_buff) },

	{ FR_CONF_OFFSET("cleanup_delay", FR_TYPE_UINT32, proto_radius_udp_t, cleanup_delay), .dflt = "5" },

	{ FR_CONF_OFFSET("connected", FR_TYPE_BOOL, proto_radius_udp_t, use_connected), .dflt = "no" },
	{ FR_CONF_OFFSET("max_connections", FR_TYPE_UINT32, proto_radius_udp_t, max_connections) },

	/*
	 *	Note that we have to pass offset of dynamic_client to get the "IS_SET" functionality.
	 *	But that screws up the entries in the dynamic_client_config, which are now offset
	 *	from THIS offset, instead of offset from the start of proto_radius_udp_t;
	 */
	{ FR_CONF_IS_SET_OFFSET("dynamic_clients", FR_TYPE_SUBSECTION | FR_TYPE_OK_MISSING, proto_radius_udp_t, dynamic_clients),
	  .subcs = (void const *) dynamic_client_config },
	CONF_PARSER_TERMINATOR
};


/*
 *	Allow configurable priorities for each listener.
 */
static uint32_t priorities[FR_MAX_PACKET_CODE] = {
	[FR_CODE_ACCESS_REQUEST] = PRIORITY_HIGH,
	[FR_CODE_ACCOUNTING_REQUEST] = PRIORITY_LOW,
	[FR_CODE_COA_REQUEST] = PRIORITY_NORMAL,
	[FR_CODE_DISCONNECT_REQUEST] = PRIORITY_NORMAL,
	[FR_CODE_STATUS_SERVER] = PRIORITY_NOW,
};


static const CONF_PARSER priority_config[] = {
	{ FR_CONF_OFFSET("Access-Request", FR_TYPE_UINT32, proto_radius_udp_t, priorities[FR_CODE_ACCESS_REQUEST]),
	  .dflt = STRINGIFY(PRIORITY_HIGH) },
	{ FR_CONF_OFFSET("Accounting-Request", FR_TYPE_UINT32, proto_radius_udp_t, priorities[FR_CODE_ACCOUNTING_REQUEST]),
	  .dflt = STRINGIFY(PRIORITY_LOW) },
	{ FR_CONF_OFFSET("CoA-Request", FR_TYPE_UINT32, proto_radius_udp_t, priorities[FR_CODE_COA_REQUEST]),
	  .dflt = STRINGIFY(PRIORITY_NORMAL) },
	{ FR_CONF_OFFSET("Disconnect-Request", FR_TYPE_UINT32, proto_radius_udp_t, priorities[FR_CODE_DISCONNECT_REQUEST]),
	  .dflt = STRINGIFY(PRIORITY_NORMAL) },
	{ FR_CONF_OFFSET("Status-Server", FR_TYPE_UINT32, proto_radius_udp_t, priorities[FR_CODE_STATUS_SERVER]),
	  .dflt = STRINGIFY(PRIORITY_NOW) },

	CONF_PARSER_TERMINATOR
};

static void dynamic_client_timer(proto_radius_udp_t *inst, RADCLIENT *client, uint32_t timer);

/*
 *	@todo - put packets to be cleaned up in a heap or linked list,
 *	and then have one cleanup delay per rlm_radius_udp_t.  That
 *	way we can have a timer which fires periodically, and then
 *	cleans up multiple packets.
 */
static void mod_cleanup_packet(UNUSED fr_event_list_t *el, struct timeval *now, void *uctx)
{
	fr_tracking_entry_t		*track = uctx;
	proto_radius_udp_address_t	*address = (proto_radius_udp_address_t *) &track->src_dst[0];
	RADCLIENT			*client = address->client;
	proto_radius_udp_t		*inst = talloc_parent(track->ft);

	/*
	 *	So that all cleanup paths can come here, not just the
	 *	timeout ones.
	 */
	if (now) {
		DEBUG2("TIMER - proto_radius cleanup delay for ID %d", track->data[1]);
	} else {
		DEBUG2("proto_radius cleaning up ID %d", track->data[1]);
	}

	(void) fr_radius_tracking_entry_delete(track->ft, track, track->timestamp);

	/*
	 *	The client isn't dynamic, stop here.
	 */
	if (!client->dynamic) return;

	/*
	 *	One less packet to deal with.
	 */
	rad_assert(client->outstanding > 0);
	client->outstanding--;

	/*
	 *	There are still outstanding packets, don't clean up
	 *	the client.  And also clean up any old idle timer.
	 */
	if (client->outstanding) {
		if (client->ev) talloc_const_free(client->ev);
		return;
	}

	/*
	 *	There are no outstanding packets, set up a timer to
	 *	delete the socket after idle_timeout.  If someone uses
	 *	it within that time frame, we'll just delete the
	 *	cleanup timer.
	 */
	dynamic_client_timer(inst, client, inst->dynamic_clients.idle_timeout);
}

/** Return the src address associated with the packet_ctx
 *
 */
static int mod_src_address(fr_socket_addr_t *src, UNUSED void const *instance, void const *packet_ctx)
{
	fr_tracking_entry_t const		*track = packet_ctx;
	proto_radius_udp_address_t const	*address = (proto_radius_udp_address_t const *) &track->src_dst[0];

	memset(src, 0, sizeof(*src));

	src->proto = IPPROTO_UDP;
	memcpy(&src->ipaddr, &address->src_ipaddr, sizeof(src->ipaddr));

	return 0;
}

/** Return the dst address associated with the packet_ctx
 *
 */
static int mod_dst_address(fr_socket_addr_t *dst, UNUSED void const *instance, void const *packet_ctx)
{
	fr_tracking_entry_t const		*track = talloc_get_type_abort_const(packet_ctx, fr_tracking_entry_t);
	proto_radius_udp_address_t const	*address = (proto_radius_udp_address_t const *) &track->src_dst[0];

	memset(dst, 0, sizeof(*dst));

	dst->proto = IPPROTO_UDP;
	memcpy(&dst->ipaddr, &address->dst_ipaddr, sizeof(dst->ipaddr));

	return 0;
}

/** Return the client associated with the packet_ctx
 *
 */
static RADCLIENT *mod_client(UNUSED void const *instance, void const *packet_ctx)
{
	fr_tracking_entry_t const		*track = talloc_get_type_abort_const(packet_ctx, fr_tracking_entry_t);
	proto_radius_udp_address_t const	*address = (proto_radius_udp_address_t const *) &track->src_dst[0];

	return address->client;
}


static ssize_t mod_encode(void const *instance, REQUEST *request, uint8_t *buffer, size_t buffer_len)
{
	proto_radius_udp_t const		*inst = instance;
	fr_tracking_entry_t const		*track = talloc_get_type_abort(request->async->packet_ctx, fr_tracking_entry_t);
	proto_radius_udp_address_t const	*address = (proto_radius_udp_address_t const *) &track->src_dst[0];
	RADCLIENT				*client;

	/*
	 *	Not a dynamic client, or it's an active one.  Let
	 *	proto_radius do all of the work.
	 */
	if (!inst->dynamic_clients_is_set || !address->client->dynamic || address->client->active) return 0;

	/*
	 *	This will never happen...
	 */
	if (buffer_len < sizeof(client)) {
		buffer[0] = 1;
		return 1;
	}

	/*
	 *	Allocate the client.  If that fails, send back a NAK.
	 *
	 *	@todo - deal with NUMA zones?  Or just deal with this
	 *	client being in different memory.
	 *
	 *	Maybe we should create a CONF_SECTION from the client,
	 *	and pass *that* back to mod_write(), which can then
	 *	parse it to create the actual client....
	 */
	client = client_afrom_request(NULL, request);
	if (!client) {
		PERROR("Failed creating new client");
		buffer[0] = 1;
		return 1;
	}

	memcpy(buffer, &client, sizeof(client));
	return sizeof(client);
}


static int mod_decode(void const *instance, REQUEST *request, UNUSED uint8_t *const data, UNUSED size_t data_len)
{
	proto_radius_udp_t const		*inst = instance;
	fr_tracking_entry_t const		*track = request->async->packet_ctx;
	proto_radius_udp_address_t const	*address = (proto_radius_udp_address_t const *) &track->src_dst[0];

	if ((track->timestamp == request->async->recv_time) &&
	    (track->reply_len > 0)) {
		DEBUG("Suppressing dup.");
		return -1;
	}

	request->client = address->client;
	request->packet->if_index = address->if_index;
	request->packet->src_ipaddr = address->src_ipaddr;
	request->packet->src_port = address->src_port;
	request->packet->dst_ipaddr = address->dst_ipaddr;
	request->packet->dst_port = address->dst_port;

	request->reply->if_index = address->if_index;
	request->reply->src_ipaddr = address->dst_ipaddr;
	request->reply->src_port = address->dst_port;
	request->reply->dst_ipaddr = address->src_ipaddr;
	request->reply->dst_port = address->src_port;

	request->root = &main_config;
	REQUEST_VERIFY(request);

	if (request->client->dynamic && !request->client->active) {
		fr_app_process_t const	*app_process;
		vp_cursor_t cursor;
		VALUE_PAIR *vp;

		app_process = (fr_app_process_t const *) inst->dynamic_clients.submodule->module->common;

		request->async->process = app_process->process;

		/*
		 *	Mash all encrypted attributes to sane
		 *	(i.e. non-hurtful) values.
		 */
		for (vp = fr_pair_cursor_init(&cursor, &request->packet->vps);
		     vp != NULL;
		     vp = fr_pair_cursor_next(&cursor)) {
			if (vp->da->flags.encrypt != FLAG_ENCRYPT_NONE) {
				switch (vp->da->type) {
				default:
					break;

				case FR_TYPE_UINT32:
					vp->vp_uint32 = 0;
					break;

				case FR_TYPE_IPV4_ADDR:
					vp->vp_ipv4addr = INADDR_ANY;
					break;

				case FR_TYPE_OCTETS:
					fr_pair_value_memcpy(vp, (uint8_t const *) "", 1);
					break;

				case FR_TYPE_STRING:
					fr_pair_value_strcpy(vp, "");
					break;
				}
			}
		}
	}

	return 0;
}

static size_t dynamic_client_packet_restore(proto_radius_udp_t *inst, uint8_t *buffer, size_t buffer_len,
					     fr_time_t *packet_time,
					     proto_radius_udp_address_t *address, fr_tracking_entry_t **track)
{
	fr_dlist_t		*entry;
	dynamic_packet_t	*saved;
	size_t			packet_len;

redo:
	entry = FR_DLIST_FIRST(inst->dynamic_clients.packets);
	if (!entry) return 0;
	fr_dlist_remove(entry);

	saved = fr_ptr_to_type(dynamic_packet_t, entry, entry);
	rad_assert(saved);
	rad_assert(saved->packet != NULL);
	rad_assert(saved->track != NULL);

	/*
	 *	The saved packet subsequently got a conflicting
	 *	packet.  We therefore ignore the older one.
	 */
	if (saved->timestamp != saved->track->timestamp) {
	drop_packet:
		(void) fr_radius_tracking_entry_delete(saved->track->ft, saved->track, saved->timestamp);
		((proto_radius_udp_address_t *)saved->track->src_dst)->client->received--;
		talloc_free(saved);
		goto redo;
	}

	/*
	 *	Can't copy the packet over, there's nothing more we
	 *	can do.
	 */
	packet_len = talloc_array_length(saved->packet);
	if (packet_len > buffer_len) {
		goto drop_packet;
	}

	/*
	 *	Copy the saved packet back to the output buffer.
	 */
	memcpy(buffer, saved->packet, packet_len);
	*track = saved->track;

	memcpy(address, saved->track->src_dst, sizeof(*address));
	address->client->received--;
	*packet_time = saved->timestamp;
	talloc_free(saved);

	return packet_len;
}


static int dynamic_client_packet_save(proto_radius_udp_t *inst, uint8_t *packet, size_t packet_len, fr_time_t packet_time,
				      proto_radius_udp_address_t *address, fr_tracking_entry_t **track)
{
	dynamic_packet_t	*saved;
	fr_tracking_status_t	tracking_status;

	if (inst->dynamic_clients.num_pending_packets >= inst->dynamic_clients.max_pending_packets) {
		DEBUG("Too many pending packets - ignoring packet.");
		return -1;
	}

	tracking_status = fr_radius_tracking_entry_insert(track, inst->ft, packet, packet_time, address);
	switch (tracking_status) {
	case FR_TRACKING_ERROR:
	case FR_TRACKING_UNUSED:
		rad_assert(0 == 1);
		return -1;	/* shouldn't happen */

		/*
		 *	Retransmit of the same packet.  There's
		 *	nothing we can do.
		 */
	case FR_TRACKING_SAME:
		return 0;

		/*
		 *	We're done the old packet, and have received a
		 *	new packet.  This shouldn't happen here.  If
		 *	we're done the old packet, we shouldn't be calling this function.
		 */
	case FR_TRACKING_UPDATED:
		DEBUG3("UPDATED packet");
		rad_assert(0 == 1);
		return -1;

		/*
		 *	We're NOT done the old packet, and have
		 *	received a new packet.  This can happen if the
		 *	old packet is taking too long.  Oh well... we
		 *	will just discard the old one in mod_write()
		 */
	case FR_TRACKING_CONFLICTING:
		DEBUG3("CONFLICTING packet ID %d", packet[1]);
		break;

		/*
		 *	We have a brand new packet.  Remember it!
		 */
	case FR_TRACKING_NEW:
		DEBUG3("NEW packet");
		break;
	}

	MEM(saved = talloc_zero(inst, dynamic_packet_t));
	MEM(saved->packet = talloc_memdup(saved, packet, packet_len));
	saved->track = *track;
	saved->timestamp = packet_time;
	fr_dlist_insert_tail(&address->client->packets, &saved->entry);
	address->client->received++;
	inst->dynamic_clients.num_pending_packets++;

	return 0;
}


static ssize_t dynamic_client_alloc(proto_radius_udp_t *inst, uint8_t *packet, size_t packet_len, fr_time_t packet_time,
				    proto_radius_udp_address_t *address, fr_tracking_entry_t **track, fr_ipaddr_t *network)
{
	RADCLIENT *client;

	/*
	 *	Limit the total number of clients.
	 */
	if (inst->dynamic_clients.num_clients >= inst->dynamic_clients.max_clients) {
		DEBUG("Too many dynamic clients - ignoring packet.");
		return 0;
	}

	/*
	 *	Limit the total number of pending clients.
	 */
	if (inst->dynamic_clients.num_pending_clients >= inst->dynamic_clients.max_pending_clients) {
		DEBUG("Too many pending dynamic clients");
		return 0;
	}

	/*
	 *	Allocate the bare client, and fill in some basic fields.
	 */
	client = talloc_zero(inst, RADCLIENT);
	if (!client) {
		return 0;
	}

	FR_DLIST_INIT(client->packets);
	client->active = false;
	client->dynamic = true;
	client->secret = client->longname = client->shortname = client->nas_type = talloc_strdup(client, "");

	client->ipaddr = address->src_ipaddr;
	client->src_ipaddr = address->dst_ipaddr;

	client->network = *network;

	address->client = client;

	/*
	 *	Save a copy of this packet in the client, so that we
	 *	can re-play it once we accept the client.
	 */
	if (dynamic_client_packet_save(inst, packet, packet_len, packet_time, address, track) < 0) {
		talloc_free(client);
		return 0;
	}

	/*
	 *	It's now one of our clients (pending).
	 *
	 *	We can rely on the worker enforcing max_request_time,
	 *	so we don't need to do something similar here.
	 *
	 *	i.e. if the client takes 30s to define, well, too
	 *	bad...
	 */
	if (!client_add(inst->dynamic_clients.pending, client)) {
		talloc_free(client);
		return -1;
	}

	inst->dynamic_clients.num_pending_clients++;

	return packet_len;
}

static void dynamic_client_expire(UNUSED fr_event_list_t *el, UNUSED struct timeval *now, void *uctx)
{
	RADCLIENT *client = uctx;
	proto_radius_udp_t *inst = client->ctx;

	DEBUG("TIMER - checking dynamic client %s for expiration.", client->shortname);

	rad_assert(client->dynamic);

	/*
	 *	It's a negative cache entry.  Just delete it.
	 */
	if (client->negative) {
		DEBUG("%s - deleting negative client %s.", inst->name, client->shortname);
		rad_assert(client->outstanding == 0);
		client_delete(inst->dynamic_clients.negative, client);
		inst->dynamic_clients.num_negative_clients--;
		client_free(client);
		return;
	}

	/*
	 *	There are still packets using this socket, wait for
	 *	them to all finish.
	 */
	if (client->outstanding > 0) {
		DEBUG("%s - waiting for packets to finish processing for client %s", inst->name, client->shortname);
		return;
	}

	/*
	 *	The client has expired, and no one is using it.
	 */
	DEBUG("%s - deleting client %s.", inst->name, client->shortname);
	client_delete(inst->dynamic_clients.clients, client);
	client_free(client);
}

static void dynamic_client_timer(proto_radius_udp_t *inst, RADCLIENT *client, uint32_t timer)
{
	struct timeval when;

	rad_assert(timer > 0);

	gettimeofday(&when, NULL);
	when.tv_sec += timer;

	client->ctx = inst;	/* nowhere else to put this... */

	if (fr_event_timer_insert(client, inst->el, &client->ev,
				  &when, dynamic_client_expire, client) < 0) {
		ERROR("Failed adding timeout for dynamic client.  It will be permanent!");
		return;
	}
}

static proto_radius_udp_t *mod_clone(proto_radius_udp_t *inst, proto_radius_udp_address_t *address)
{
	int			rcode;
	proto_radius_udp_t	*child;
	dl_instance_t		*dl_inst = NULL;
	fr_listen_t		*listen;

	/*
	 *	Reload ourselves as a "new" library.  This causes the
	 *	link count for the library to be correct.  It also
	 *	allocates a new instance data for the library.
	 *	Passing CONF_SECTION of NULL ensures that there's no
	 *	config for it, as we'll just clone it's contents from
	 *	the parent.  It also means that detach should be
	 *	called when the instance data is freed.
	 */
	if (dl_instance(NULL, &dl_inst, NULL, inst->master.parent_dl_inst, "udp", DL_TYPE_SUBMODULE) < 0) {
		return NULL;
	}

	child = talloc_get_type_abort(dl_inst->data, proto_radius_udp_t);

	/*
	 *	Copy the basic configuration, and then modify it.
	 */
	memcpy(child, inst, sizeof(*child));

	child->connected = true;
	child->sockfd = -1;
	child->name = NULL;
	child->el = NULL;
	child->nr = NULL;
	child->ft = NULL;
	child->dynamic_clients_is_set = false;
	memset(&child->master, 0, sizeof(child->master));

	child->child.master = inst;
	child->child.dl_inst = dl_inst;

	child->ipaddr = address->dst_ipaddr;
	child->port = address->dst_port;
	child->child.if_index = address->if_index;
	child->child.src_ipaddr = address->src_ipaddr;
	child->child.src_port = address->src_port;

	child->dynamic_clients.clients = NULL;
	child->dynamic_clients.pending = NULL;
	child->dynamic_clients.negative = NULL;
	child->dynamic_clients.trie = NULL;

	child->child.client = client_clone(child, address->client);
	if (!child->child.client) {
		ERROR("Failed cloning client");
		talloc_free(dl_inst);
		return NULL;
	}

	/*
	 *	These fields may be different from the ones in the
	 *	template client.  Further "ipaddr" is the IP address
	 *	of the client.  And "src_ipaddr" is the address that
	 *	packets are sent from.
	 */
	child->child.client->src_ipaddr = address->dst_ipaddr;
	child->child.client->ipaddr = address->src_ipaddr;

	/*
	 *	Create the new listener, and populate it's children.
	 */
	listen = child->child.listen = talloc(child, fr_listen_t);
	if (!listen) {
		talloc_free(dl_inst);
		return NULL;
	}

	memcpy(listen, inst->parent->listen, sizeof(*listen));
	listen->app_io_instance = child;

	/*
	 *	Instantiate the child, and open the socket.
	 */
	if ((listen->app_io->instantiate(child, inst->cs) < 0) ||
	    (listen->app_io->open(child) < 0)) {
		talloc_free(dl_inst);
		return NULL;
	}

	/*
	 *	Attach it to the parent hash table, so that the child
	 *	can find itself there when it starts running.
	 *
	 *	@todo - remove the mutex lock, and have the master
	 *	handle the tree.
	 */
	PTHREAD_MUTEX_LOCK(&inst->master.mutex);
	rcode = fr_hash_table_insert(inst->master.ht, child);
	PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);

	if (rcode < 0) {
		ERROR("Failed inserting child socket into hash table.");
		talloc_free(dl_inst);
		return NULL;
	}

	/*
	 *	Add the child to the network side.  If that doesn't
	 *	work, remove it from the hash table.
	 */
	child->nr = fr_schedule_socket_add(inst->parent->sc, listen);
	if (!child->nr) {
		ERROR("Failed adding child socket to scheduler.");
		PTHREAD_MUTEX_LOCK(&inst->master.mutex);
		(void) fr_hash_table_delete(inst->master.ht, child);
		PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);
		talloc_free(dl_inst);
		return NULL;
	}

	return child;
}

/** Get a packet from one of several possible places.
 *
 */
static ssize_t mod_read_packet(proto_radius_udp_t *inst, uint8_t *buffer, size_t buffer_len,
			       fr_time_t *packet_time,
			       proto_radius_udp_address_t *address, fr_tracking_entry_t **track)
{
	int				flags;
	fr_dlist_t			*entry;
	ssize_t				data_size;
	size_t				packet_len;
	struct timeval			timestamp;
	decode_fail_t			reason;

	/*
	 *	Check for injected packets first.  This only works for
	 *	connected sockets.
	 */
	if (inst->connected && inst->child.packet) {
		/*
		 *	Packet is too large, ignore it.
		 */
		if (buffer_len < inst->child.packet_len) {
			inst->child.packet = NULL;
			goto check_dynamic;
		}

		memcpy(buffer, inst->child.packet, inst->child.packet_len);

		address->code = buffer[0];
		address->id = buffer[1];
		address->if_index = inst->child.if_index;
		address->src_ipaddr = inst->child.src_ipaddr;
		address->src_port = inst->child.src_port;
		address->dst_ipaddr = inst->ipaddr;
		address->dst_port = inst->port;
		address->client = inst->child.client;
		*packet_time = inst->child.recv_time;
		inst->child.packet = NULL;
		return inst->child.packet_len;
	}

	/*
	 *	There are saved packets.  Go read them.
	 */
check_dynamic:
	entry = FR_DLIST_FIRST(inst->dynamic_clients.packets);
	if (entry) {
		packet_len = dynamic_client_packet_restore(inst, buffer, buffer_len, packet_time, address, track);
		if (packet_len > 0) {
			rad_assert(track != NULL);
			return packet_len;
		}
	}

	/*
	 *	Tell udp_recv if we're connected or not.
	 */
	flags = UDP_FLAGS_CONNECTED * inst->connected;

	data_size = udp_recv(inst->sockfd, buffer, buffer_len, flags,
			     &address->src_ipaddr, &address->src_port,
			     &address->dst_ipaddr, &address->dst_port,
			     &address->if_index, &timestamp);
	if (data_size < 0) {
		DEBUG2("proto_radius_udp got read error %zd: %s", data_size, fr_strerror());
		return data_size;
	}

	if (!data_size) {
		DEBUG2("proto_radius_udp got no data: ignoring");
		return 0;
	}

	packet_len = data_size;

	if (data_size < 20) {
		DEBUG2("proto_radius_udp got 'too short' packet size %zd", data_size);
		inst->stats.total_malformed_requests++;
		return 0;
	}

	if ((buffer[0] == 0) || (buffer[0] > FR_MAX_PACKET_CODE)) {
		DEBUG("proto_radius_udp got invalid packet code %d", buffer[0]);
		inst->stats.total_unknown_types++;
		return 0;
	}

	if (!inst->parent->process_by_code[buffer[0]]) {
		DEBUG("proto_radius_udp got unexpected packet code %d", buffer[0]);
		inst->stats.total_unknown_types++;
		return 0;
	}

	/*
	 *	If it's not a RADIUS packet, ignore it.
	 */
	if (!fr_radius_ok(buffer, &packet_len, inst->parent->max_attributes, false, &reason)) {
		/*
		 *	@todo - check for F5 load balancer packets.  <sigh>
		 */
		DEBUG2("proto_radius_udp got a packet which isn't RADIUS");
		inst->stats.total_malformed_requests++;
		return 0;
	}

	/*
	 *	Track the packet ID.
	 */
	address->code = buffer[0];
	address->id = buffer[1];
	address->client = NULL;
	*packet_time = fr_time();

	return packet_len;
}


static ssize_t mod_read(void *instance, void **packet_ctx, fr_time_t **recv_time, uint8_t *buffer, size_t buffer_len, size_t *leftover, uint32_t *priority, bool *is_dup)
{
	proto_radius_udp_t		*inst = talloc_get_type_abort(instance, proto_radius_udp_t);

	int				flags;
	ssize_t				data_size;
	size_t				packet_len;

	fr_time_t			packet_time;
	fr_tracking_status_t		tracking_status;
	fr_tracking_entry_t		*track;
	proto_radius_udp_address_t	address;

	*leftover = 0;		/* always for UDP */
	*is_dup = false;
	track = NULL;

	/*
	 *	Get a packet, using various magic.
	 */
	data_size = mod_read_packet(inst, buffer, buffer_len, &packet_time, &address, &track);
	if (data_size <= 0) return data_size;

	packet_len = data_size;

	/*
	 *	See if this packet is for a connected socket.  If so,
	 *	send it to the connected socket.
	 */
	if (inst->use_connected && !inst->connected) {
		proto_radius_udp_t *child, my_child;

		/*
		 *	There is a connection which matches this
		 *	packet.  Inject the packet there, and remove
		 *	any local tracking entry if it exists.
		 */
		my_child.ipaddr = address.dst_ipaddr;
		my_child.port = address.dst_port;

		my_child.child.src_ipaddr = address.src_ipaddr;
		my_child.child.src_port = address.src_port;

		/*
		 *	@todo - find a way to remove this mutex.
		 *
		 *	The best way is probably to create a "linking"
		 *	structure..  the parent puts the packet into
		 *	the linking structure (which contains src/dst
		 *	IP/port, and a pointer to the child).  But the
		 *	parent NEVER dereferences the child?
		 *
		 *	The child can then free itself as needed, BUT
		 *	first marks up the linking structure as "no
		 *	longer necessary".  At which point it's
		 *	cleaned up...
		 *
		 *	The parent will have to periodically walk
		 *	through all children, to see if they are
		 *	alive.  That work is likely less of an issue
		 *	than locking a mutex for every packet.
		 *	Especially if the work is done periodically,
		 *	and is amortized over many packets...
		 */
		PTHREAD_MUTEX_LOCK(&inst->master.mutex);
		child = fr_hash_table_finddata(inst->master.ht, &my_child);
		if (child) {
			(void) fr_network_listen_inject(child->nr, child->child.listen,
							buffer, packet_len, packet_time);
			PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);

			if (track) {
				(void) fr_radius_tracking_entry_delete(track->ft, track, packet_time);
			}
			return 0;
		}

		/*
		 *	There's no existing connection, fall back to
		 *	looking up the client.
		 */
		PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);

		/*
		 *	We have a dynamic client, AND we're using
		 *	connected sockets.  BUT this packet isn't for
		 *	any known connection.
		 */
		if (address.client && address.client->dynamic) {
			/*
			 *	@todo - do the whole dynamic client
			 *	alloc thing again... set the client to
			 *	inactive, and run the packet through
			 *	the same logic.
			 */
		}
	}

	/*
	 *	Look up the client.  The client may already exist if
	 *	the packet was received from a dynamic client in the
	 *	process of being created.
	 */
	if (!address.client) address.client = client_find(NULL, &address.src_ipaddr, IPPROTO_UDP);

	/*
	 *	No client and no dynamic clients.  Discard the packet.
	 */
	if (!address.client && !inst->dynamic_clients_is_set) {
	unknown:
		ERROR("Packet from unknown client at address %pV:%u - ignoring.",
		      fr_box_ipaddr(address.src_ipaddr), address.src_port);
		inst->stats.total_invalid_requests++;
		return 0;
	}

	/*
	 *	Still no client (and we have dynamic clients), look up
	 *	the client in the dynamic client list.
	 */
	if (!address.client) address.client = client_find(inst->dynamic_clients.clients, &address.src_ipaddr, IPPROTO_UDP);

	/*
	 *	Still no client, maybe it's pending?
	 *
	 *	If it's pending, save the packet for later processing and return.
	 */
	if (!address.client) {
		address.client = client_find(inst->dynamic_clients.pending, &address.src_ipaddr, IPPROTO_UDP);
		if (address.client) {
			if (dynamic_client_packet_save(inst, buffer, packet_len, packet_time, &address, &track) < 0) {
				goto unknown;
			}

			return 0;
		}
	}

	/*
	 *	Still no client (and we have dynamic clients), try to
	 *	define the client.
	 */
	if (!address.client) {
		fr_ipaddr_t *network;
		/*
		 *	No static client.  No dynamic client.  Maybe
		 *	it's from a known network?  Look up the packet
		 *	source address, returning the longest prefix
		 *	match with a known network.
		 */
		network = fr_trie_lookup(inst->dynamic_clients.trie, &address.src_ipaddr.addr, address.src_ipaddr.prefix);
		if (!network) {
			DEBUG("%s - Source IP address %pV was not within a known network",
			      inst->name, fr_box_ipaddr(address.src_ipaddr));
			goto unknown;
		}

		DEBUG("Found matching network.  Checking for dynamic client definition.");

		/*
		 *	Allocate the dynamic client, and add the
		 *	packet to the tracking table.
		 */
		if (dynamic_client_alloc(inst, buffer, packet_len, packet_time, &address, &track, network) < 0) {
			DEBUG("Failed allocating dynamic client");
			goto unknown;
		}

		/*
		 *	Return the packet, as it's ALREADY been
		 *	inserted into the tracking table via
		 *	dynamic_client_alloc().
		 */
		goto return_packet;
	}

	/*
	 *	Check for a socket that SHOULD be connected.  If so,
	 *	either create the socket, OR find it in the list of
	 *	sockets, and send the packet there.
	 *
	 *	We can then REMOVE the tracking table entry for this
	 *	packet, as it is no longer used.  We ALSO need to mark
	 *	up the client as "connected", so that packets to it go
	 *	to the child socket.  And, somehow... clean up the
	 *	client when there are no more packets for it?
	 *
	 *	i.e. if there's a client but no child socket, go back
	 *	and create a child socket...
	 */
	if (inst->use_connected && !inst->connected) {
		proto_radius_udp_t *child;

		/*
		 *	Try to clone us into a child.  If that
		 *	succeeds, send the packet to the child.
		 */
		child = mod_clone(inst, &address);
		if (child) {
			PTHREAD_MUTEX_LOCK(&inst->master.mutex);
			(void) fr_network_listen_inject(child->nr, child->child.listen,
							buffer, packet_len, packet_time);
			PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);
		}

		/*
		 *	We're no longer tracking this packet.
		 *	Instead, the child socket is.  So we just
		 *	discard the packet.
		 */
		if (track) (void) fr_radius_tracking_entry_delete(track->ft, track, packet_time);
		return 0;
	}

	/*
	 *	If the packet signature fails validation, ignore it.
	 */
	if (fr_radius_verify(buffer, NULL,
			     (uint8_t const *)address.client->secret,
			     talloc_array_length(address.client->secret) - 1) < 0) {
		DEBUG2("proto_radius_udp packet failed verification: %s", fr_strerror());
		inst->stats.total_bad_authenticators++;
		return 0;
	}

	/*
	 *	If the packet is not already in the tracking table
	 *	(e.g. dynamic clients have packets in the tracking
	 *	table), then go check it now.
	 */
	if (!track) {
		tracking_status = fr_radius_tracking_entry_insert(&track, inst->ft, buffer, packet_time, &address);
		switch (tracking_status) {
		case FR_TRACKING_ERROR:
		case FR_TRACKING_UNUSED:
			inst->stats.total_packets_dropped++;
			return -1;	/* Fatal */

			/*
			 *	If the entry already has a cleanup delay, we
			 *	extend the cleanup delay.  i.e. the cleanup
			 *	delay is from the last reply we sent, not from
			 *	the first one.
			 */
		case FR_TRACKING_SAME:
			DEBUG3("SAME packet");
			if (track->ev) {
				struct timeval tv;


				gettimeofday(&tv, NULL);
				tv.tv_sec += inst->cleanup_delay;

				DEBUG3("SAME packet - cleanup");
				(void) fr_event_timer_insert(NULL, inst->el, &track->ev,
							     &tv, mod_cleanup_packet, track);
			}

			inst->stats.total_dup_requests++;

			/*
			 *	We are intentionally not responding.
			 */
			if (track->reply_len && !track->reply) {
				return 0;
			}

			/*
			 *	If there is a reply, just resend that.
			 */
			if (track->reply) {
				void *packet;

				flags = UDP_FLAGS_CONNECTED * inst->connected;
				memcpy(&packet, &track->reply, sizeof(packet)); /* const issues */
				rad_assert(track->reply_len >= 20);

				(void) udp_send(inst->sockfd, packet, track->reply_len, flags,
						&address.dst_ipaddr, address.dst_port,
						address.if_index,
						&address.src_ipaddr, address.src_port);
				return 0;
			}

			/*
			 *	Otherwise it's a duplicate packet.  Send the
			 *	whole thing over to the network stack, while
			 *	updating the "packet recv time" to be when the
			 *	original packet was received.
			 *
			 *	We still have ordering issue.  The
			 *	original packet MAY be done before
			 *	this packet gets to the worker.  So
			 *	the this packet is ALSO marked up as
			 *	"dup".  The worker will then ignore
			 *	the duplicate packet if it's already
			 *	sent a reply.
			 */
			packet_time = track->timestamp;
			*is_dup = true;
			break;

			/*
			 *	Delete any pre-existing cleanup_delay timers.
			 */
		case FR_TRACKING_UPDATED:
			DEBUG3("UPDATED packet");
			if (track->ev) (void) fr_event_timer_delete(inst->el, &track->ev);
			break;

		case FR_TRACKING_CONFLICTING:
			if (track->ev) (void) fr_event_timer_delete(inst->el, &track->ev);
			DEBUG3("CONFLICTING packet ID %d", buffer[1]);
			break;

		case FR_TRACKING_NEW:
			rad_assert(track->ev == NULL);
			DEBUG3("NEW packet");
			break;
		}
	}

	/*
	 *	Remove the cleanup timer if we receive a new packet
	 *	for this connection.
	 */
	if (inst->connected && inst->child.ev) {
		fr_event_timer_delete(inst->el, &inst->child.ev);
	}

	inst->stats.total_requests++;
	rad_assert(address.client != NULL);
	if (address.client->dynamic) address.client->outstanding++;

return_packet:
	*packet_ctx = track;
	*recv_time = &track->timestamp;
	*priority = priorities[buffer[0]];

	return packet_len;
}


static int mod_inject(void *instance, uint8_t *buffer, size_t buffer_len, fr_time_t recv_time)
{
	proto_radius_udp_t		*inst = talloc_get_type_abort(instance, proto_radius_udp_t);
	size_t				packet_len;
	decode_fail_t			reason;

	if (!inst->connected) {
		DEBUG2("proto_radius_udp received injected packet for an unconnected socket.");
		inst->stats.total_packets_dropped++;
		return -1;
	}

	if (inst->child.packet != NULL) {
		DEBUG2("proto_radius_udp received two injected packets in a row.");
		inst->stats.total_packets_dropped++;
		return -1;
	}

	/*
	 *	We should still sanity check the packet.
	 */
	if (buffer_len < 20) {
		DEBUG2("proto_radius_udp got 'too short' packet size %zd", buffer_len);
		inst->stats.total_malformed_requests++;
		return -1;
	}

	if ((buffer[0] == 0) || (buffer[0] > FR_MAX_PACKET_CODE)) {
		DEBUG("proto_radius_udp got invalid packet code %d", buffer[0]);
		inst->stats.total_unknown_types++;
		return -1;
	}

	if (!inst->parent->process_by_code[buffer[0]]) {
		DEBUG("proto_radius_udp got unexpected packet code %d", buffer[0]);
		inst->stats.total_unknown_types++;
		return -1;
	}

	/*
	 *	Initialize the packet length.
	 */
	packet_len = buffer_len;

	/*
	 *	If it's not a RADIUS packet, ignore it.
	 */
	if (!fr_radius_ok(buffer, &packet_len, inst->parent->max_attributes, false, &reason)) {
		/*
		 *	@todo - check for F5 load balancer packets.  <sigh>
		 */
		DEBUG2("proto_radius_udp got a packet which isn't RADIUS");
		inst->stats.total_malformed_requests++;
		return -1;
	}

	inst->child.packet = buffer;
	inst->child.packet_len = packet_len;
	inst->child.recv_time = recv_time;

	return 0;
}

static ssize_t mod_write(void *instance, void *packet_ctx,
			 fr_time_t request_time, uint8_t *buffer, size_t buffer_len)
{
	proto_radius_udp_t		*inst = talloc_get_type_abort(instance, proto_radius_udp_t);
	fr_tracking_entry_t		*track = packet_ctx;
	proto_radius_udp_address_t	*address = (proto_radius_udp_address_t *) &track->src_dst[0];

	int				flags;
	ssize_t				data_size;
	fr_time_t			reply_time;
	struct timeval			tv;

	/*
	 *	Check for the first packet back from a dynamic client
	 *	definition.  If we find it, add the client (or not),
	 *	as required.
	 */
	if (inst->dynamic_clients_is_set && address->client->dynamic && !address->client->active) {
		RADCLIENT *client = address->client;
		RADCLIENT *newclient;
		fr_dlist_t *entry;
		dynamic_packet_t *saved;

		/*
		 *	@todo - maybe just duplicate the new client
		 *	fields, and talloc_free(newclient).  That
		 *	means we don't have to muck with pending
		 *	packets.
		 */
		inst->dynamic_clients.num_pending_clients--;

		/*
		 *	Delete the "pending" client from the pending
		 *	client list.  Whatever we do next, this client
		 *	is no longer "pending".
		 */
		client_delete(inst->dynamic_clients.pending, address->client);

		/*
		 *	NAK: drop all packets.
		 *
		 *	If it's an explicit NAK, then add the source
		 *	IP to a negative cache as a DoS prevention.
		 */
		if (buffer_len == 1) {
			if ((inst->dynamic_clients.num_negative_clients <= 1024) &&
			    client_add(inst->dynamic_clients.negative, client)) {
				client->negative = true;
				inst->dynamic_clients.num_negative_clients++;
			}

		nak:
			while ((entry = FR_DLIST_FIRST(client->packets)) != NULL) {
				saved = fr_ptr_to_type(dynamic_packet_t, entry, entry);
				(void) talloc_get_type_abort(saved, dynamic_packet_t);
				(void) fr_radius_tracking_entry_delete(saved->track->ft, saved->track, saved->timestamp);
				fr_dlist_remove(&saved->entry);
				talloc_free(saved);
				inst->dynamic_clients.num_pending_packets--;
			}

			/*
			 *	Do NOT delete the tracking table
			 *	entry.  The packet has to be
			 *	re-injected!
			 *
			 *	But, add a timer to clean up the
			 *	negative cache entry in 30s.
			 *
			 *	@todo - make this timer configurable
			 */
			dynamic_client_timer(inst, client, 30);
			return buffer_len;
		}

		rad_assert(buffer_len == sizeof(newclient));
		memcpy(&newclient, buffer, sizeof(newclient));
		FR_DLIST_INIT(newclient->packets);

		/*
		 *	@todo - if we have connected sockets, then
		 *	DON'T delete the old client.  Instead, move
		 *	packets for this connection to the new client.
		 *	Then, check if there are pending packets for
		 *	the old client.  If not, delete it.  If so, do
		 *	something intelligent...
		 *
		 *	For connected sockets, we don't set a cleanup
		 *	timer on the client.  Instead, we just delete
		 *	the client when the socket goes away...
		 */
		rad_assert(!inst->use_connected);

		DEBUG("%s - Defining new client %s", inst->name, client->shortname);
		newclient->dynamic = true;

		/*
		 *	If we can't add it, then clean it up.  BUT
		 *	allow other packets to come from the same IP.
		 */
		if (!client_add(inst->dynamic_clients.clients, newclient)) {
			talloc_free(newclient);
			client->negative = false;
			goto nak;
		}

		newclient->active = true;
		inst->dynamic_clients.num_clients++;

		/*
		 *	Move the packets over to the pending list, and
		 *	re-write their client pointers to be the newly
		 *	allocated one.
		 */
		while ((entry = FR_DLIST_FIRST(client->packets)) != NULL) {
			saved = fr_ptr_to_type(dynamic_packet_t, entry, entry);
			(void) talloc_get_type_abort(saved, dynamic_packet_t);

			fr_dlist_remove(&saved->entry);
			fr_dlist_insert_tail(&inst->dynamic_clients.packets, &saved->entry);

			address = (proto_radius_udp_address_t *) &saved->track->src_dst[0];
			address->client = newclient;

			rad_assert(inst->dynamic_clients.num_pending_packets > 0);
			inst->dynamic_clients.num_pending_packets--;
		}

		talloc_free(client);

		/*
		 *	Tell the network side to call mod_read(), if necessary.
		 */
		entry = FR_DLIST_FIRST(inst->dynamic_clients.packets);
		if (entry) {
			DEBUG3("Emptying pending queue");
			fr_network_listen_read(inst->nr, inst->parent->listen);
		}

		/*
		 *	Do NOT delete the tracking table
		 *	entry.  The packet has to be
		 *	re-injected!.
		 */
		return buffer_len;
	}

	/*
	 *	The original packet has changed.  Suppress the write,
	 *	as the client will never accept the response.
	 */
	if ((track->timestamp != request_time) || !address->client) {
		inst->stats.total_packets_dropped++;
		DEBUG3("Suppressing reply as we have a newer packet");
		(void) fr_radius_tracking_entry_delete(track->ft, track, request_time);
		return buffer_len;
	}

	inst->stats.total_responses++;

	/*
	 *	Figure out when we've sent the reply.
	 */
	reply_time = fr_time();

	flags = UDP_FLAGS_CONNECTED * inst->connected;

	/*
	 *	This handles the race condition where we get a DUP,
	 *	but the original packet replies before we're run.
	 *	i.e. this packet isn't marked DUP, so we have to
	 *	discover it's a dup later...
	 *
	 *	As such, if there's already a reply, then we ignore
	 *	the encoded reply (which is probably going to be a
	 *	NAK), and instead reply with the cached reply.
	 */
	if (track->reply_len) {
		if (track->reply_len >= 20) {
			char *packet;

			memcpy(&packet, &track->reply, sizeof(packet)); /* const issues */

			(void) udp_send(inst->sockfd, packet, track->reply_len, flags,
					&address->dst_ipaddr, address->dst_port,
					address->if_index,
					&address->src_ipaddr, address->src_port);
		}

		mod_cleanup_packet(NULL, NULL, track);
		return buffer_len;
	}

	/*
	 *	Only write replies if they're RADIUS packets.
	 *	sometimes we want to NOT send a reply...
	 */
	if (buffer_len >= 20) {
		data_size = udp_send(inst->sockfd, buffer, buffer_len, flags,
				     &address->dst_ipaddr, address->dst_port,
				     address->if_index,
				     &address->src_ipaddr, address->src_port);
			/*
			 *	This whole socket is dead.  Stop processing all packets
			 */
		if (data_size < 0) {
			mod_cleanup_packet(NULL, NULL, track);
			rad_assert(0 == 1);
			fr_exit(EXIT_FAILURE);
		}

	} else {
		/*
		 *	Otherwise lie, and say we've written it all...
		 */
		data_size = buffer_len;
		DEBUG3("Got NAK, not writing reply");
	}

	/*
	 *	Root through the reply to determine any
	 *	connection-level negotiation data.
	 */
	if (track->data[0] == FR_CODE_STATUS_SERVER) {
//		status_check_reply(inst, buffer, buffer_len);
	}

	/*
	 *	Most packets are cleaned up immediately.  Also, if
	 *	cleanup_delay = 0, then we even clean up
	 *	Access-Request packets immediately.
	 */
	if (!inst->cleanup_delay) {
		DEBUG3("Deleting tracking table entry");

	done:
		if (track->ev) (void) fr_event_timer_delete(inst->el, &track->ev);
		mod_cleanup_packet(NULL, NULL, track);
		return data_size;
	}

	/*
	 *	Add the reply to the tracking entry.
	 */
	if (fr_radius_tracking_entry_reply(track->ft, track, reply_time,
					   buffer, buffer_len) < 0) {
		DEBUG3("Failed adding reply to tracking table");
		goto done;
	}

	/*
	 *	@todo - Move event timers to fr_time_t
	 */
	gettimeofday(&tv, NULL);

	tv.tv_sec += inst->cleanup_delay;

	/*
	 *	Set cleanup timer.
	 */
	if (fr_event_timer_insert(NULL, inst->el, &track->ev,
				  &tv, mod_cleanup_packet, track) < 0) {
		DEBUG3("Failed adding cleanup timer");
		goto done;
	}

	/*
	 *	Don't delete the tracking entry.  The cleanup timer
	 *	will do that.
	 */
	return data_size;
}


/** Open a UDP listener for RADIUS
 *
 * @param[in] instance of the RADIUS UDP I/O path.
 * @return
 *	- <0 on error
 *	- 0 on success
 */
static int mod_close(void *instance)
{
	proto_radius_udp_t *inst = talloc_get_type_abort(instance, proto_radius_udp_t);

	close(inst->sockfd);
	inst->sockfd = -1;

	/*
	 *	If we're the child, then free the module instance, on
	 *	close.  And, remove the link to the dl library.
	 */
	if (inst->connected) {
		talloc_free(inst->child.dl_inst);
	}

	return 0;
}

/** Open a UDP listener for RADIUS
 *
 * @param[in] instance of the RADIUS UDP I/O path.
 * @return
 *	- <0 on error
 *	- 0 on success
 */
static int mod_open(void *instance)
{
	proto_radius_udp_t *inst = talloc_get_type_abort(instance, proto_radius_udp_t);

	int				sockfd = 0;
	uint16_t			port = inst->port;

	sockfd = fr_socket_server_udp(&inst->ipaddr, &port, inst->port_name, true);
	if (sockfd < 0) {
		PERROR("Failed opening UDP socket");
	error:
		return -1;
	}

	/*
	 *	Set SO_REUSEPORT before bind, so that all packets can
	 *	listen on the same destination IP address.
	 */
	if (inst->use_connected) {
		int on = 1;

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
			WARN("Failed to set socket 'reuseport': disabling connected sockets.");
			inst->use_connected = false;
		}
	}

	if (fr_socket_bind(sockfd, &inst->ipaddr, &port, inst->interface) < 0) {
		close(sockfd);
		PERROR("Failed binding socket");
		goto error;
	}

	/*
	 *	Connect to the client for child sockets.
	 */
	if (inst->use_connected && inst->connected) {
		socklen_t salen;
		struct sockaddr_storage src;

		if (fr_ipaddr_to_sockaddr(&inst->child.src_ipaddr, inst->child.src_port,
					  &src, &salen) < 0) {
			close(sockfd);
			ERROR("Failed getting IP address");
			goto error;
		}

		if (connect(sockfd, (struct sockaddr *) &src, salen) < 0) {
			close(sockfd);
			ERROR("Failed in connect: %s", fr_syserror(errno));
			goto error;
		}
	}

	inst->sockfd = sockfd;

	// @todo - also print out auth / acct / coa, etc.
	DEBUG("Listening on radius address %s bound to virtual server %s",
	      inst->name, cf_section_name2(inst->parent->server_cs));

	return 0;
}

/** Get the file descriptor for this socket.
 *
 * @param[in] instance of the RADIUS UDP I/O path.
 * @return the file descriptor
 */
static int mod_fd(void const *instance)
{
	proto_radius_udp_t const *inst = talloc_get_type_abort_const(instance, proto_radius_udp_t);

	return inst->sockfd;
}


/** Set the event list for a new socket
 *
 * @param[in] instance of the RADIUS UDP I/O path.
 * @param[in] el the event list
 * @param[in] nr context from the network side
 */
static void mod_event_list_set(void *instance, fr_event_list_t *el, void *nr)
{
	proto_radius_udp_t *inst;

	memcpy(&inst, &instance, sizeof(inst)); /* const issues */

	inst = talloc_get_type_abort(instance, proto_radius_udp_t);

	/*
	 *	Dynamic clients require an event list for cleanups.
	 */
	if (!inst->dynamic_clients_is_set) {
		/*
		 *	Only Access-Request gets a cleanup delay.
		 */
		if (!inst->parent->code_allowed[FR_CODE_ACCESS_REQUEST]) return;

		/*
		 *	And then, only if it is non-zero.
		 */
		if (!inst->cleanup_delay) return;
	}

	inst->el = el;
	inst->nr = nr;
}


static int mod_instantiate(void *instance, CONF_SECTION *cs)
{
	proto_radius_udp_t *inst = talloc_get_type_abort(instance, proto_radius_udp_t);
	char		    dst_buf[128];

	inst->ft = fr_radius_tracking_create(inst, sizeof(proto_radius_udp_address_t), inst->parent->code_allowed);
	if (!inst->ft) {
		cf_log_perr(cs, "Failed to create tracking table");
		return -1;
	}

	/*
	 *	Instantiate proto_radius_dynamic_client
	 */
	if (inst->dynamic_clients_is_set) {
		fr_app_process_t const	*app_process;

		app_process = (fr_app_process_t const *)inst->dynamic_clients.submodule->module->common;
		if (app_process->instantiate && (app_process->instantiate(inst->dynamic_clients.submodule->data,
									  cf_item_to_section(cf_parent(cs))) < 0)) {
			cf_log_err(cs, "Instantiation failed for \"%s\"", app_process->name);
			return -1;
		}
	}

	/*
	 *	Get our name.
	 */
	rad_assert(inst->name == NULL);

	if (fr_ipaddr_is_inaddr_any(&inst->ipaddr)) {
		if (inst->ipaddr.af == AF_INET) {
			strlcpy(dst_buf, "*", sizeof(dst_buf));
		} else {
			rad_assert(inst->ipaddr.af == AF_INET6);
			strlcpy(dst_buf, "::", sizeof(dst_buf));
		}
	} else {
		fr_value_box_snprint(dst_buf, sizeof(dst_buf), fr_box_ipaddr(inst->ipaddr), 0);
	}

	if (!inst->connected) {
		inst->name = talloc_typed_asprintf(inst, "proto udp address %s port %u",
						   dst_buf, inst->port);
	} else {
		char src_buf[128];

		fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(inst->child.src_ipaddr), 0);

		inst->name = talloc_typed_asprintf(inst, "proto udp connected socket from client %s port %u to address %s port %u",
						   src_buf, inst->child.src_port, dst_buf, inst->port);
	}

	return 0;
}

static uint32_t udp_hash_inst(void const *instance)
{
	uint32_t hash;
	proto_radius_udp_t const *inst = instance;

	hash = fr_hash(&inst->ipaddr, sizeof(inst->ipaddr));
	hash = fr_hash_update(&inst->port, sizeof(inst->port), hash);

	hash = fr_hash_update(&inst->child.src_ipaddr, sizeof(inst->child.src_ipaddr), hash);

	return fr_hash_update(&inst->child.src_port, sizeof(inst->child.src_port), hash);
}

static int udp_cmp_inst(void const *one, void const *two)
{
	int rcode;
	proto_radius_udp_t const *a = one;
	proto_radius_udp_t const *b = two;

	rcode = (a->child.src_port - b->child.src_port);
	if (rcode != 0) return rcode;

	rcode = (a->port - b->port);
	if (rcode != 0) return rcode;

	rcode = fr_ipaddr_cmp(&a->ipaddr, &b->ipaddr);
	if (rcode != 0) return rcode;

	return fr_ipaddr_cmp(&a->child.src_ipaddr, &b->child.src_ipaddr);
}


static int mod_bootstrap(void *instance, CONF_SECTION *cs)
{
	proto_radius_udp_t	*inst = talloc_get_type_abort(instance, proto_radius_udp_t);
	dl_instance_t const	*dl_inst;
	CONF_SECTION		*subcs;

	/*
	 *	Find the dl_instance_t holding our instance data
	 *	so we can find out what the parent of our instance
	 *	was.
	 */
	dl_inst = dl_instance_find(instance);
	rad_assert(dl_inst);

	inst->parent = talloc_get_type_abort(dl_inst->parent->data, proto_radius_t);
	inst->master.parent_dl_inst = dl_inst->parent;
	inst->cs = cs;

	/*
	 *	Hide this for now.  It's only for people who know what
	 *	they're doing.
	 */
	subcs = cf_section_find(cs, "priority", NULL);
	if (subcs) {
		if (cf_section_rules_push(subcs, priority_config) < 0) return -1;
		if (cf_section_parse(NULL, NULL, subcs) < 0) return -1;

	} else {
		rad_assert(sizeof(inst->priorities) == sizeof(priorities));
		memcpy(&inst->priorities, &priorities, sizeof(priorities));
	}

	/*
	 *	Complain if no "ipaddr" is set.
	 */
	if (inst->ipaddr.af == AF_UNSPEC) {
		cf_log_err(cs, "No 'ipaddr' was specified in the 'udp' section");
		return -1;
	}

	if (inst->recv_buff_is_set) {
		FR_INTEGER_BOUND_CHECK("recv_buff", inst->recv_buff, >=, 32);
		FR_INTEGER_BOUND_CHECK("recv_buff", inst->recv_buff, <=, INT_MAX);
	}

	if (!inst->port) {
		struct servent *s;

		if (!inst->port_name) {
			cf_log_err(cs, "No 'port' was specified in the 'udp' section");
			return -1;
		}

		s = getservbyname(inst->port_name, "udp");
		if (!s) {
			cf_log_err(cs, "Unknown value for 'port_name = %s", inst->port_name);
			return -1;
		}

		inst->port = ntohl(s->s_port);
	}

	FR_INTEGER_BOUND_CHECK("cleanup_delay", inst->cleanup_delay, <=, 30);

	if (inst->dynamic_clients_is_set) {
		size_t i, num;
		dl_instance_t *parent_inst;

		if (inst->use_connected) {
			cf_log_err(cs, "Cannot (yet) use dynamic clients and connected sockets.");
			return -1;
		}

		if (!inst->dynamic_clients.network) {
			cf_log_err(cs, "One or more 'network' entries MUST be specified for dynamic clients.");
			return -1;
		}

		inst->dynamic_clients.trie = fr_trie_alloc(inst);
		if (!inst->dynamic_clients.trie) {
			cf_log_err(cs, "Failed creating network trie.");
			return -1;
		}

		num = talloc_array_length(inst->dynamic_clients.network);
		for (i = 0; i < num; i++) {
			fr_ipaddr_t *network;
			char buffer[256];

			/*
			 *	Can't add v4 networks to a v6 socket, or vice versa.
			 */
			if (inst->dynamic_clients.network[i].af != inst->ipaddr.af) {
				fr_value_box_snprint(buffer, sizeof(buffer), fr_box_ipaddr(inst->dynamic_clients.network[i]), 0);
				cf_log_err(cs, "Address family in entry %zd - 'network = %s' does not match 'ipaddr'", i + 1, buffer);
				return -1;
			}

			/*
			 *	Duplicates are bad.
			 */
			network = fr_trie_match(inst->dynamic_clients.trie,
					   &inst->dynamic_clients.network[i].addr, inst->dynamic_clients.network[i].prefix);
			if (network) {
				fr_value_box_snprint(buffer, sizeof(buffer), fr_box_ipaddr(inst->dynamic_clients.network[i]), 0);
				cf_log_err(cs, "Cannot add duplicate entry 'network = %s'", buffer);
				return -1;
			}

			/*
			 *	Look for overlapping entries.
			 *	i.e. the networks MUST be disjoint.
			 *
			 *	Note that this catches 192.168.1/24
			 *	followed by 192.168/16, but NOT the
			 *	other way around.  The best fix is
			 *	likely to add a flag to
			 *	fr_trie_alloc() saying "we can only
			 *	have terminal fr_trie_user_t nodes"
			 */
			network = fr_trie_lookup(inst->dynamic_clients.trie,
					   &inst->dynamic_clients.network[i].addr, inst->dynamic_clients.network[i].prefix);
			if (network && (network->prefix <= inst->dynamic_clients.network[i].prefix)) {
				fr_value_box_snprint(buffer, sizeof(buffer), fr_box_ipaddr(inst->dynamic_clients.network[i]), 0);
				cf_log_err(cs, "Cannot add overlapping entry 'network = %s'", buffer);
				cf_log_err(cs, "Entry is completely enclosed inside of a previously defined network.");
				return -1;
			}

			/*
			 *	Insert the network into the trie.
			 *	Lookups will return the fr_ipaddr_t of
			 *	the network.
			 */
			if (fr_trie_insert(inst->dynamic_clients.trie,
					   &inst->dynamic_clients.network[i].addr, inst->dynamic_clients.network[i].prefix,
					   &inst->dynamic_clients.network[i]) < 0) {
				fr_value_box_snprint(buffer, sizeof(buffer), fr_box_ipaddr(inst->dynamic_clients.network[i]), 0);
				cf_log_err(cs, "Failed adding 'network = %s' to tracking table.", buffer);
				return -1;
			}
		}

		parent_inst = cf_data_value(cf_data_find(cf_parent(cs), dl_instance_t, "proto_radius"));
		rad_assert(parent_inst != NULL);

		if (dl_instance(inst, &inst->dynamic_clients.submodule,
				cs, parent_inst, "dynamic_client", DL_TYPE_SUBMODULE) < 0) {
			cf_log_perr(cs, "Failed finding proto_radius_dynamic_client");
			return -1;
		}

		FR_DLIST_INIT(inst->dynamic_clients.packets);

		/*
		 *	Allow static clients for this virtual server.
		 */
		inst->dynamic_clients.clients = client_list_init(NULL); // client_list_parse_section(inst->parent->server_cs, false);
		inst->dynamic_clients.pending = client_list_init(NULL);
		inst->dynamic_clients.negative = client_list_init(NULL);

		FR_INTEGER_BOUND_CHECK("max_clients", inst->dynamic_clients.max_clients, >=, 1);
		FR_INTEGER_BOUND_CHECK("max_clients", inst->dynamic_clients.max_clients, <=, (1 << 20));

		FR_INTEGER_BOUND_CHECK("max_pending_clients", inst->dynamic_clients.max_pending_clients, >=, 4);
		FR_INTEGER_BOUND_CHECK("max_pending_clients", inst->dynamic_clients.max_pending_clients, <=, 2048);

		FR_INTEGER_BOUND_CHECK("max_pending_packets", inst->dynamic_clients.max_pending_clients, >=, 256);
		FR_INTEGER_BOUND_CHECK("max_pending_packets", inst->dynamic_clients.max_pending_clients, <=, 65536);

		if (inst->dynamic_clients.idle_timeout) {
			FR_INTEGER_BOUND_CHECK("idle_timeout", inst->dynamic_clients.idle_timeout, >=, 30);
			FR_INTEGER_BOUND_CHECK("idle_timeout", inst->dynamic_clients.idle_timeout, <=, 86400);
		}
	}

	/*
	 *	Using connected sockets?  Initialize more information.
	 */
	if (inst->use_connected) {
		if (!inst->connected) {
#ifdef HAVE_PTHREAD_H
			(void) pthread_mutex_init(&inst->master.mutex, NULL);
#endif
			inst->master.ctx = talloc_init("proto_radius_udp_master_t");
			if (!inst->master.ctx) {
			nomem:
				cf_log_err(cs, "Failed initializing data structures.");
				return -1;
			}
			inst->master.ht = fr_hash_table_create(inst->master.ctx, udp_hash_inst, udp_cmp_inst, NULL);
			if (!inst->master.ht) goto nomem;

			if (!inst->max_connections) inst->max_connections = 65536;

			FR_INTEGER_BOUND_CHECK("max_connections", inst->max_connections, >=, 4);
			FR_INTEGER_BOUND_CHECK("max_connections", inst->max_connections, <=, 65536);

		} else {
			/*
			 *	We MUST have been initialized to point
			 *	to the master.
			 */
			rad_assert(inst->child.master != NULL);

			/*
			 *	We MUST already have an open socket.
			 */
			rad_assert(inst->sockfd >= 0);
		}
	}

	return 0;
}

static int divorce_children(UNUSED void *ctx, void *data)
{
	proto_radius_udp_t *child = data;

	child->child.master = NULL;
	return 0;
}


static int mod_detach(void *instance)
{
	proto_radius_udp_t	*inst = talloc_get_type_abort(instance, proto_radius_udp_t);

	close(inst->sockfd);

	/*
	 *	Clean up extra tracking information when using
	 *	connected sockets.
	 */
	if (inst->use_connected) {
		/*
		 *	If we're the master, tell the children to
		 *	forget about us, and then clean up the hash
		 *	table and mutexes.
		 */
		if (!inst->connected) {
			PTHREAD_MUTEX_LOCK(&inst->master.mutex);
			(void) fr_hash_table_walk(inst->master.ht, divorce_children, inst);
			TALLOC_FREE(inst->master.ctx);
			PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);
#ifdef HAVE_PTHREAD_H
			(void) pthread_mutex_destroy(&inst->master.mutex);
#endif

		} else {
			/*
			 *	We're the child, tell the master to
			 *	forget about us.
			 *
			 *	@todo - mark ourselves dead, and let
			 *	the master clean us up.
			 */
			PTHREAD_MUTEX_LOCK(&inst->master.mutex);
			(void) fr_hash_table_delete(inst->child.master->master.ht, inst);
			PTHREAD_MUTEX_UNLOCK(&inst->master.mutex);

			/*
			 *	If it's a dynamic client, then we're
			 *	the only one who knows about it.  So
			 *	we need to free the client.
			 */
			if (inst->child.client->dynamic) talloc_free(inst->child.client);
		}
	}

	if (inst->dynamic_clients_is_set) {
		TALLOC_FREE(inst->dynamic_clients.clients);
		TALLOC_FREE(inst->dynamic_clients.trie);
	}

	return 0;
}


/** Private interface for use by proto_radius
 *
 */
extern proto_radius_app_io_t proto_radius_app_io_private;
proto_radius_app_io_t proto_radius_app_io_private = {
	.client			= mod_client,
	.src			= mod_src_address,
	.dst			= mod_dst_address
};

extern fr_app_io_t proto_radius_udp;
fr_app_io_t proto_radius_udp = {
	.magic			= RLM_MODULE_INIT,
	.name			= "radius_udp",
	.config			= udp_listen_config,
	.inst_size		= sizeof(proto_radius_udp_t),
	.detach			= mod_detach,
	.bootstrap		= mod_bootstrap,
	.instantiate		= mod_instantiate,

	.default_message_size	= 4096,
	.track_duplicates	= true,

	.open			= mod_open,
	.read			= mod_read,
	.write			= mod_write,
	.inject			= mod_inject,
	.decode			= mod_decode,
	.encode			= mod_encode, /* only for dynamic client creation */
	.close			= mod_close,
	.fd			= mod_fd,
	.event_list_set		= mod_event_list_set,
};
