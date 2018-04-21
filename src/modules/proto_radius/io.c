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
 * @file proto_radius/io.c
 * @brief RADIUS master IO handler
 *
 * @copyright 2018 Alan DeKok (aland@freeradius.org)
 */
#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/radius/radius.h>
#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/unlang.h>
#include <freeradius-devel/io/schedule.h>
#include <freeradius-devel/io/application.h>
#include <freeradius-devel/rad_assert.h>
#include "proto_radius.h"

static fr_event_update_t pause_read[] = {
	FR_EVENT_SUSPEND(fr_event_io_func_t, read),
	{ 0 }
};

static fr_event_update_t resume_read[] = {
	FR_EVENT_RESUME(fr_event_io_func_t, read),
	{ 0 }
};

/*
 *  Return negative numbers to put 'one' at the top of the heap.
 *  Return positive numbers to put 'two' at the top of the heap.
 */
static int pending_packet_cmp(void const *one, void const *two)
{
	proto_radius_pending_packet_t const *a = one;
	proto_radius_pending_packet_t const *b = two;
	int rcode;

	/*
	 *	Larger numbers mean higher priority
	 */
	rcode = (a->priority < b->priority) - (a->priority > b->priority);
	if (rcode != 0) return rcode;

	/*
	 *	Smaller numbers mean packets were received earlier.
	 *	We want to process packets in time order.
	 */
	rcode = (a->recv_time > b->recv_time) - (a->recv_time < b->recv_time);
	if (rcode != 0) return rcode;

	/*
	 *	After that, it doesn't really matter what order the
	 *	packets go in.  Since we'll never have two identical
	 *	"recv_time" values, the code should never get here.
	 */
	return 0;
}

/*
 *	Order clients in the pending_clients heap, based on the
 *	packets that they contain.
 */
static int pending_client_cmp(void const *one, void const *two)
{
	proto_radius_pending_packet_t const *a;
	proto_radius_pending_packet_t const *b;

	proto_radius_client_t const *c1 = one;
	proto_radius_client_t const *c2 = two;

	a = fr_heap_peek(c1->pending);
	b = fr_heap_peek(c2->pending);

	rad_assert(a != NULL);
	rad_assert(b != NULL);

	return pending_packet_cmp(a, b);
}


static int address_cmp(void const *one, void const *two)
{
	int rcode;
	proto_radius_address_t const *a = one;
	proto_radius_address_t const *b = two;

	rcode = (a->src_port - b->src_port);
	if (rcode != 0) return rcode;

	rcode = (a->dst_port - b->dst_port);
	if (rcode != 0) return rcode;

	rcode = (a->if_index - b->if_index);
	if (rcode != 0) return rcode;

	rcode = fr_ipaddr_cmp(&a->src_ipaddr, &b->src_ipaddr);
	if (rcode != 0) return rcode;

	return fr_ipaddr_cmp(&a->dst_ipaddr, &b->dst_ipaddr);
}

static uint32_t connection_hash(void const *ctx)
{
	uint32_t hash;
	proto_radius_connection_t const *c = ctx;

	hash = fr_hash(&c->address->src_ipaddr, sizeof(c->address->src_ipaddr));
	hash = fr_hash_update(&c->address->src_port, sizeof(c->address->src_port), hash);

	hash = fr_hash_update(&c->address->if_index, sizeof(c->address->if_index), hash);

	hash = fr_hash_update(&c->address->dst_ipaddr, sizeof(c->address->dst_ipaddr), hash);
	return fr_hash_update(&c->address->dst_port, sizeof(c->address->dst_port), hash);
}


static int connection_cmp(void const *one, void const *two)
{
	proto_radius_connection_t const *a = one;
	proto_radius_connection_t const *b = two;

	return address_cmp(a->address, b->address);
}


static int track_cmp(void const *one, void const *two)
{
	proto_radius_track_t const *a = one;
	proto_radius_track_t const *b = two;
	int rcode;

	/*
	 *	The tree is ordered by IDs, which are (hopefully)
	 *	pseudo-randomly distributed.
	 */
	rcode = (a->packet[1] < b->packet[1]) - (a->packet[1] > b->packet[1]);
	if (rcode != 0) return rcode;

	/*
	 *	Then ordered by ID, which is usally the same.
	 */
	rcode = (a->packet[0] < b->packet[0]) - (a->packet[0] > b->packet[0]);
	if (rcode != 0) return rcode;

	/*
	 *	Connected sockets MUST have all tracking entries use
	 *	the same client definition.
	 */
	if (a->client->connected) {
		rad_assert(a->client == b->client);
		return 0;
	}

	rad_assert(!b->client->connected);

	/*
	 *	Unconnected sockets must check src/dst ip/port.
	 */
	return address_cmp(a->address, b->address);
}


static proto_radius_pending_packet_t *pending_packet_pop(proto_radius_t *inst)
{
	proto_radius_client_t *client;
	proto_radius_pending_packet_t *pending;

	client = fr_heap_pop(inst->pending_clients);
	if (!client) {
		/*
		 *	99% of the time we don't have pending clients.
		 *	So we might as well free this, so that the
		 *	caller doesn't keep checking us for every packet.
		 */
		talloc_free(inst->pending_clients);
		inst->pending_clients = NULL;
		return NULL;
	}

	pending = fr_heap_pop(client->pending);
	rad_assert(pending != NULL);

	/*
	 *	If the client has more packets pending, add it back to
	 *	the heap.
	 */
	if (fr_heap_num_elements(client->pending) > 0) {
		(void) fr_heap_insert(inst->pending_clients, client);
	}

	rad_assert(inst->num_pending_packets > 0);
	inst->num_pending_packets--;

	return pending;
}


/** Create a new connection.
 *
 *  Called ONLY from the master socket.
 */
static proto_radius_connection_t *proto_radius_connection_alloc(proto_radius_t *inst, proto_radius_client_t *client,
								proto_radius_address_t *address,
								proto_radius_connection_t *nak)
{
	int rcode;
	proto_radius_connection_t *connection;
	dl_instance_t *dl_inst = NULL;
	fr_listen_t *listen;
	RADCLIENT *radclient;

	/*
	 *	Reload the app_io module as a "new" library.  This
	 *	causes the link count for the library to be correct.
	 *	It also allocates a new instance data for it, too.
	 *	Passing CONF_SECTION of NULL ensures that there's no
	 *	config for it, as we'll just clone it's contents from
	 *	the original.  It also means that detach should be
	 *	called when the instance data is freed.
	 */
	if (!nak) {
		if (dl_instance(NULL, &dl_inst, NULL, inst->dl_inst, inst->transport, DL_TYPE_SUBMODULE) < 0) {
			DEBUG("Failed to find proto_radius_%s", inst->transport);
			return NULL;
		}
		rad_assert(dl_inst != NULL);
	} else {
		dl_inst = talloc_init("nak");
	}

	MEM(connection = talloc_zero(dl_inst, proto_radius_connection_t));
	MEM(connection->address = talloc_memdup(connection, address, sizeof(*address)));
	(void) talloc_set_name_const(connection->address, "proto_radius_address_t");

	connection->magic = PR_CONNECTION_MAGIC;
	connection->parent = client;
	connection->dl_inst = dl_inst;

	MEM(connection->client = talloc_zero(connection, proto_radius_client_t));
	MEM(connection->client->radclient = radclient = client_clone(connection->client, client->radclient));
	connection->client->heap_id = -1;
	connection->client->connected = true;

	/*
	 *	Create the packet tracking table for this client.
	 *
	 *	#todo - unify the code with static clients?
	 */
	MEM(connection->client->table = rbtree_talloc_create(client, track_cmp, proto_radius_track_t,
							     NULL, RBTREE_FLAG_NONE));

	/*
	 *	Set this radclient to be dynamic, and active.
	 */
	radclient->dynamic = true;
	radclient->active = true;

	/*
	 *	address->client points to a "static" client.  We want
	 *	to clean up everything associated with the connection
	 *	when it closes.  So we need to point to our own copy
	 *	of the client here.
	 */
	connection->address->radclient = connection->client->radclient;
	connection->client->inst = inst;

	/*
	 *	Create a heap for packets which are pending for this
	 *	client.
	 */
	MEM(connection->client->pending = fr_heap_create(connection->client, pending_packet_cmp,
							 proto_radius_pending_packet_t, heap_id));

	/*
	 *	Clients for connected sockets are always a /32 or /128.
	 */
	connection->client->src_ipaddr = address->src_ipaddr;
	connection->client->network = address->src_ipaddr;

	/*
	 *	Don't initialize mutex or hash table.
	 *	Connections cannot spawn other connections.
	 */

	/*
	 *	If this client state is pending, then the connection
	 *	state is pending, too.  That allows NAT gateways to be
	 *	defined dynamically, AND for them to have multiple
	 *	connections, each with a different client.  This
	 *	allows for different shared secrets to be used for
	 *	different connections.  Once the client gets defined
	 *	for this connection, it will be either "connected" or
	 *	not.  If connected, then the parent client remains
	 *	PENDING.  Otherwise, the parent client is moved to
	 *	DYNAMIC
	 *
	 *	If this client state is static or dynamic,
	 *	then we're just using connected sockets behind
	 *	that client.  The connections here all use the
	 *	same shared secret, but they use different
	 *	sockets, so they allow for sharing of IO
	 *	across CPUs / threads.
	 */
	switch (client->state) {
	case PR_CLIENT_PENDING:
		connection->client->state = PR_CLIENT_PENDING;

		/*
		 *	Needed for rlm_radius, which refuses to proxy packets
		 *	that define a dynamic client.
		 */
		radclient->active = false;
		break;

	case PR_CLIENT_STATIC:
	case PR_CLIENT_DYNAMIC:
		connection->client->state = PR_CLIENT_CONNECTED;
		break;

	case PR_CLIENT_INVALID:
	case PR_CLIENT_NAK:
	case PR_CLIENT_CONNECTED:
		rad_assert(0 == 1);
		talloc_free(dl_inst);
		return NULL;
	}

	if (!nak) {
		/*
		 *	Create the listener, based on our listener.
		 */
		MEM(listen = connection->listen = talloc(connection, fr_listen_t));

		/*
		 *	Note that our instance is effectively 'const'.
		 *
		 *	i.e. we can't add things to it.  Instead, we have to
		 *	put all variable data into the connection.
		 */
		memcpy(listen, inst->listen, sizeof(*listen));

		/*
		 *	Glue in the connection to the listener.
		 */
		listen->app_io = &proto_radius_master_io;
		listen->app_io_instance = connection;

		connection->app_io_instance = dl_inst->data;

		/*
		 *	Bootstrap the configuration.  There shouldn't
		 *	be need to re-parse it.
		 */
		memcpy(connection->app_io_instance, inst->app_io_instance, inst->app_io->inst_size);

		/*
		 *	Instantiate the child, and open the socket.
		 *
		 *	This also sets connection->name.
		 */
		if ((inst->app_io_private->connection_set(connection->app_io_instance, connection) < 0) ||
		    (inst->app_io->instantiate(connection->app_io_instance, inst->app_io_conf) < 0) ||
		    (inst->app_io->open(connection->app_io_instance) < 0)) {
			DEBUG("Failed opening connected socket.");
			talloc_free(dl_inst);
			return NULL;
		}
	}

	/*
	 *	Add the connection to the set of connections for this
	 *	client.
	 */
	pthread_mutex_lock(&client->mutex);
	if (nak) (void) fr_hash_table_delete(client->ht, nak);
	rcode = fr_hash_table_insert(client->ht, connection);
	client->ready_to_delete = false;
	pthread_mutex_unlock(&client->mutex);
	
	if (rcode < 0) {
		ERROR("proto_radius - Failed inserting connection into tracking table.  Closing it, and diuscarding all packets for connection %s.", connection->name);
		goto cleanup;
	}

	/*
	 *	It's a NAK client.  Set the state to NAK, and don't
	 *	add it to the scheduler.
	 */
	if (nak) {
		connection->name = talloc_strdup(connection, nak->name);
		connection->client->state = PR_CLIENT_NAK;
		connection->el = nak->el;
		return connection;
	}

	DEBUG("proto_radius - starting connection %s", connection->name);
	connection->nr = fr_schedule_socket_add(inst->sc, connection->listen);
	if (!connection->nr) {
		ERROR("proto_radius - Failed inserting connection into scheduler.  Closing it, and diuscarding all packets for connection %s.", connection->name);
		pthread_mutex_lock(&client->mutex);
		(void) fr_hash_table_delete(client->ht, connection);
		pthread_mutex_unlock(&client->mutex);

	cleanup:
		talloc_free(dl_inst);
		return NULL;
	}

	return connection;
}


/*
 *	And here we go into the rabbit hole...
 *
 *	@todo future - have a similar structure
 *	proto_radius_connection_io, which will duplicate some code,
 *	but may make things simpler?
 */
static void get_inst(void *instance, proto_radius_t **inst, proto_radius_connection_t **connection,
		     void **app_io_instance)
{
	int magic;

	magic = *(int *) instance;
	if (magic == PR_MAIN_MAGIC) {
		*inst = instance;
		*connection = NULL;
		if (app_io_instance) *app_io_instance = (*inst)->app_io_instance;

	} else {
		rad_assert(magic == PR_CONNECTION_MAGIC);
		*connection = instance;
		*inst = (*connection)->client ->inst;
		if (app_io_instance) *app_io_instance = (*connection)->app_io_instance;

	}
}


static RADCLIENT *proto_radius_radclient_alloc(proto_radius_t *inst, proto_radius_address_t *address)
{
	RADCLIENT *client;
	char src_buf[128];

	MEM(client = talloc_zero(inst, RADCLIENT));

	fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(address->src_ipaddr), 0);

	client->longname = client->shortname = talloc_strdup(client, src_buf);

	client->secret = client->nas_type = talloc_strdup(client, "");

	client->ipaddr = address->src_ipaddr;

	client->src_ipaddr = address->dst_ipaddr;

	client->proto = inst->ipproto;
	client->dynamic = true;

	return client;
}


static proto_radius_track_t *proto_radius_track_add(proto_radius_client_t *client,
						    proto_radius_address_t *address,
						    uint8_t const *packet, fr_time_t recv_time, bool *is_dup)
{
	proto_radius_track_t my_track, *track;

	my_track.address = address;
	my_track.client = client;
	memcpy(my_track.packet, packet, sizeof(my_track.packet));

	track = rbtree_finddata(client->table, &my_track);
	if (!track) {
		*is_dup = false;

		MEM(track = talloc_zero(client, proto_radius_track_t));
		talloc_get_type_abort(track, proto_radius_track_t);

		MEM(track->address = talloc_zero(track, proto_radius_address_t));
		memcpy(track->address, address, sizeof(*address));
		track->address->radclient = client->radclient;

		track->client = client;
		if (client->connected) {
			proto_radius_connection_t *connection = talloc_parent(client);

			track->address = connection->address;
		}

		memcpy(track->packet, packet, sizeof(track->packet));
		track->timestamp = recv_time;
		track->packets = 1;
		return track;
	}

	talloc_get_type_abort(track, proto_radius_track_t);

	/*
	 *	Is it exactly the same packet?
	 */
	if (memcmp(track->packet, my_track.packet, sizeof(my_track.packet)) == 0) {
		/*
		 *	Ignore duplicates while the client is
		 *	still pending.
		 */
		if (client->state == PR_CLIENT_PENDING) {
			DEBUG("Ignoring duplicate packet while client %s is still pending dynamic definition",
			      client->radclient->shortname);
			return NULL;
		}

		*is_dup = true;
		track->packets++;
		return track;
	}

	/*
	 *	The new packet is different from the old one.
	 */
	memcpy(track->packet, my_track.packet, sizeof(my_track.packet));
	track->timestamp = recv_time;
	track->packets++;

	if (track->ev) {
		(void) talloc_const_free(track->ev);
		track->ev = NULL;
	}

	/*
	 *	We haven't yet sent a reply, this is a conflicting
	 *	packet.
	 */
	if (track->reply_len == 0) {
		return track;
	}

	/*
	 *	Free any cached replies.
	 */
	if (track->reply) {
		talloc_const_free(track->reply);
		track->reply = NULL;
		track->reply_len = 0;
	}

	return track;
}

static int pending_free(proto_radius_pending_packet_t *pending)
{
	proto_radius_track_t *track = pending->track;

	/*
	 *	Note that we don't check timestamps, replies, etc.  If
	 *	a packet is pending, then any conflicting packet gets
	 *	the "pending" entry marked as such, and a new entry
	 *	added.  Any duplicate packet gets suppressed.  And
	 *	because the packets are pending, track->reply MUST be
	 *	NULL.
	 */
	rad_assert(track->packets > 0);
	track->packets--;
	
	/*
	 *	No more packets using this tracking entry,
	 *	delete it.
	 */
	if (track->packets == 0) {
		(void) rbtree_deletebydata(track->client->table, track);

		// @todo - put this into a slab allocator
		talloc_free(track);
	}

	return 0;
}

static proto_radius_pending_packet_t *proto_radius_pending_alloc(proto_radius_client_t *client,
								 uint8_t const *buffer, size_t packet_len,
								 proto_radius_track_t *track,
								 int priority)
{
	proto_radius_pending_packet_t *pending;

	MEM(pending = talloc_zero(client->pending, proto_radius_pending_packet_t));

	MEM(pending->buffer = talloc_memdup(pending, buffer, packet_len));
	pending->buffer_len = packet_len;
	pending->priority = priority;
	pending->track = track;
	pending->recv_time = track->timestamp; /* there can only be one */

	talloc_set_destructor(pending, pending_free);

	/*
	 *	Insert the pending packet for this client.  If it
	 *	fails, silently discard the packet.
	 */
	if (fr_heap_insert(client->pending, pending) < 0) {
		talloc_free(pending);
		return NULL;
	}

	/*
	 *	We only track pending packets for the
	 *	main socket.  For connected sockets,
	 *	we pause the FD, so the number of
	 *	pending packets will always be small.
	 */
	if (!client->connected) client->inst->num_pending_packets++;

	return pending;
}


/** Count the number of connections used by active clients.
 *
 *  Unfortunately, we also count NAK'd connections, too, even if they
 *  are closed.  The alternative is to walk through all connections
 *  for each client, which would be a long time.
 */
static int count_connections(void *ctx, UNUSED uint8_t const *key, UNUSED int keylen, void *data)
{
	proto_radius_client_t *client = data;
	int connections;

	/*
	 *	This client has no connections, skip the mutex lock.
	 */
	if (!client->ht) return 0;

	rad_assert(client->use_connected);

	pthread_mutex_lock(&client->mutex);
	connections = fr_hash_table_num_elements(client->ht);
	pthread_mutex_unlock(&client->mutex);

	*((uint32_t *) ctx) += connections;

	return 0;
}


/**  Implement 99% of the RADIUS read routines.
 *
 *  The app_io->read does the transport-specific data read.
 */
static ssize_t mod_read(void *instance, void **packet_ctx, fr_time_t **recv_time_p,
			uint8_t *buffer, size_t buffer_len, size_t *leftover, uint32_t *priority, bool *is_dup)
{
	proto_radius_t *inst;
	ssize_t packet_len;
	fr_time_t recv_time;
	proto_radius_client_t *client;
	proto_radius_address_t address;
	proto_radius_connection_t my_connection, *connection;
	proto_radius_pending_packet_t *pending;
	proto_radius_track_t *track;
	void *app_io_instance;

	get_inst(instance, &inst, &connection, &app_io_instance);

	*is_dup = false;
	track = NULL;

redo:
	/*
	 *	Read one pending packet.  The packet may be pending
	 *	because of dynamic client definitions, or because it's
	 *	for a connected UDP socket, and was sent over by the
	 *	"master" UDP socket.
	 */
	if (connection) {
		/*
		 *	The connection is dead.  Tell the network side
		 *	to close it.
		 */
		if (connection->dead) {
			DEBUG("Dead connection %s", connection->name);
			return -1;
		}

		pending = fr_heap_pop(connection->client->pending);

	} else if (inst->pending_clients) {
		pending = pending_packet_pop(inst);

	} else {
		pending = NULL;
	}

	if (pending) {
		rad_assert(buffer_len >= pending->buffer_len);
		track = pending->track;

		/*
		 *	Clear the destructor as we now own the
		 *	tracking entry.
		 */
		talloc_set_destructor(pending, NULL);

		/*
		 *	We received a conflicting packet while this
		 *	packet was pending.  Discard this entry and
		 *	try to get another one.
		 *
		 *	Note that the pending heap is *simple*.  We
		 *	just track priority and recv_time.  This means
		 *	it's fast, but also that it's hard to look up
		 *	random packets in the pending heap.
		 */
		if (pending->recv_time != track->timestamp) {
			DEBUG3("Discarding old packet");
			talloc_free(pending);
			goto redo;
		}

		/*
		 *	We have a valid packet.  Copy it over to the
		 *	caller, and return.
		 */
		*packet_ctx = track;
		*recv_time_p = &track->timestamp;
		*leftover = 0;
		*priority = pending->priority;
		recv_time = pending->recv_time;
		client = track->client;

		memcpy(buffer, pending->buffer, pending->buffer_len);
		packet_len = pending->buffer_len;

		/*
		 *	Shouldn't be necessary, but what the heck...
		 */
		memcpy(&address, track->address, sizeof(address));
		talloc_free(pending);

		/*
		 *	Skip over all kinds of logic to find /
		 *	allocate the client, when we don't need to do
		 *	it any more.
		 */
		goto have_client;

	} else {
		proto_radius_address_t *local_address = &address;
		fr_time_t *local_recv_time = &recv_time;

		/*
		 *	@todo TCP - handle TCP connected sockets, where we
		 *	don't get a packet here, but instead get told
		 *	there's a new socket.  In that situation, we
		 *	have to get the new sockfd, figure out what
		 *	the source IP is, etc.
		 *
		 *	If we can, we shoe-horn this into the "read"
		 *	routine, which should make the rest of the
		 *	code simpler
		 *
		 *	@todo TCP - for connected TCP sockets which are
		 *	dynamically defined, have the app_io_read()
		 *	function STOP reading the socket once a packet
		 *	has been read.  That puts backpressure on the
		 *	client...
		 *
		 *	@todo TLS - for TLS and dynamic sockets, do the SSL setup here,
		 *		but have a structure which describes the TLS data
		 *		and run THAT through the dynamic client definition,
		 *		instead of using RADIUS packets.
		 */
		packet_len = inst->app_io->read(app_io_instance, (void **) &local_address, &local_recv_time,
					  buffer, buffer_len, leftover, priority, is_dup);
		if (packet_len <= 0) {
			DEBUG("NO DATA %d", (int) packet_len);
			return packet_len;
		}

		rad_assert(packet_len >= 20);
		rad_assert(inst->priorities[buffer[0]] != 0);

		/*
		 *	Not allowed?  Complain and discard it.
		 */
		if (!inst->process_by_code[buffer[0]]) {
			char src_buf[128];

			fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(address.src_ipaddr), 0);

			DEBUG2("proto_radius - ignoring packet %d from IP %s. It is not configured as 'type = ...'",
			       buffer[0], src_buf);
			return 0;
		}

		*priority = inst->priorities[buffer[0]];
		if (connection) DEBUG2("proto_radius - Received %s ID %d length %d from connection %s",
				       fr_packet_codes[buffer[0]], buffer[1], (int) packet_len, connection->name);
	}

	/*
	 *	Look up the client, unless we already have one (for a
	 *	connected socket).
	 */
	if (!connection) {
		client = fr_trie_lookup(inst->trie, &address.src_ipaddr.addr, address.src_ipaddr.prefix);
		rad_assert(!client || !client->connected);

	} else {
		client = connection->client;
	}

	/*
	 *	Negative cache entry.  Drop the packet.
	 */
	if (client && client->state == PR_CLIENT_NAK) {
		return 0;
	}

	/*
	 *	If there's no client, try to pull one from the global
	 *	/ static client list.  Or if dynamic clients are
	 *	allowed, try to define a dynamic client.
	 */
	if (!client) {
		RADCLIENT *radclient = NULL;
		proto_radius_client_state_t state;
		fr_ipaddr_t const *network = NULL;
		char src_buf[128];

		/*
		 *	We MUST be the master socket.
		 */
		rad_assert(!connection);

		radclient = client_find(NULL, &address.src_ipaddr, inst->ipproto);
		if (radclient) {
			state = PR_CLIENT_STATIC;

			/*
			 *	Make our own copy that we can modify it.
			 */
			MEM(radclient = client_clone(inst, radclient));
			radclient->active = true;

		} else if (inst->dynamic_clients) {
			if (inst->max_clients && (inst->num_clients >= inst->max_clients)) {
				fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(address.src_ipaddr), 0);
				DEBUG("proto_radius - ignoring packet code %d from client IP address %s - too many dynamic clients are defined",
				      buffer[0], src_buf);
				return 0;
			}

			network = fr_trie_lookup(inst->networks, &address.src_ipaddr.addr, address.src_ipaddr.prefix);
			if (!network) goto ignore;

			/*
			 *	Allocate our local radclient as a
			 *	placeholder for the dynamic client.
			 */
			radclient = proto_radius_radclient_alloc(inst, &address);
			state = PR_CLIENT_PENDING;

		} else {
		ignore:
			fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(address.src_ipaddr), 0);
			DEBUG("proto_radius - ignoring packet code %d from unknown client IP address %s",
			      buffer[0], src_buf);
			return 0;
		}

		/*
		 *	Create our own local client.  This client
		 *	holds our state which really shouldn't go into
		 *	RADCLIENT.
		 */
		MEM(client = talloc_zero(inst, proto_radius_client_t));
		client->state = state;
		client->src_ipaddr = radclient->ipaddr;
		client->radclient = radclient;
		client->inst = inst;
		client->heap_id = -1;
		client->connected = false;

		if (network) {
			client->network = *network;
		} else {
			client->network = client->src_ipaddr;
		}

		/*
		 *	At this point, this variable can only be true
		 *	for STATIC clients.  PENDING clients may set
		 *	it to true later, after they've been defined.
		 */
		client->use_connected = radclient->use_connected;

		/*
		 *	Create the pending heap for pending clients.
		 */
		if (state == PR_CLIENT_PENDING) {
			MEM(client->pending = fr_heap_create(client, pending_packet_cmp,
							     proto_radius_pending_packet_t, heap_id));
		}

		/*
		 *	Create the packet tracking table for this client.
		 */
		MEM(client->table = rbtree_talloc_create(client, track_cmp, proto_radius_track_t,
							 NULL, RBTREE_FLAG_NONE));

		/*
		 *	Allow connected sockets to be set on a
		 *	per-client basis.
		 */
		if (client->use_connected) {
			rad_assert(client->state == PR_CLIENT_STATIC);

			(void) pthread_mutex_init(&client->mutex, NULL);
			MEM(client->ht = fr_hash_table_create(client, connection_hash, connection_cmp, NULL));
		}

		/*
		 *	Add the newly defined client to the trie of
		 *	allowed clients.
		 */
		if (fr_trie_insert(inst->trie, &client->src_ipaddr.addr, client->src_ipaddr.prefix, client)) {
			ERROR("proto_radius - Failed inserting client %s into tracking table.  Discarding client, and all packts for it.", client->radclient->shortname);
			talloc_free(client);
			return -1;
		}

		client->in_trie = true;
		if (client->state == PR_CLIENT_PENDING) inst->num_clients++;
	}

have_client:
	rad_assert(client->state != PR_CLIENT_INVALID);
	rad_assert(client->state != PR_CLIENT_NAK);

	/*
	 *	@todo TCP - have CLIENT_ACCEPT socket?  for those
	 *	sockets, we never read packets or push packets to the
	 *	child socket.  But we do create connections?
	 *
	 *	For those connections, we just create the
	 *	connection and start it up.  We don't inject
	 *	any packets to it.  Instead, we rely on the
	 *	connection to notice that it's pending, read
	 *	the first packet, and then run the dynamic
	 *	client definition code.
	 */

	/*
	 *	No connected sockets, OR we are the connected socket.
	 *
	 *	Track this packet and return it if necessary.
	 */
	if (connection || !client->use_connected) {
		/*
		 *	Add the packet to the tracking table, if it's
		 *	not already there.  Pending packets will be in
		 *	the tracking table, but won't be counted as
		 *	"live" packets.
		 */
		if (!track) {
			track = proto_radius_track_add(client, &address, buffer, recv_time, is_dup);
			if (!track) {
				DEBUG("Failed tracking packet from client %s - discarding it.", client->radclient->shortname);
				return 0;
			}
		}

		/*
		 *	This is a pending dynamic client.  See if we
		 *	have to either run the dynamic client code to
		 *	define the client, OR to push the packet onto
		 *	the pending queue for this client.
		 */
		if (client->state == PR_CLIENT_PENDING) {
			char src_buf[128];

			/*
			 *	Track pending packets for the master
			 *	socket.  Connected sockets are paused
			 *	as soon as they are defined, so we
			 *	won't be reading any more packets from
			 *	them.
			 *
			 *	Since we don't have pending packets
			 *	for connected sockets, we don't need
			 *	to track pending packets.
			 */
			if (!connection && inst->max_pending_packets && (inst->num_pending_packets >= inst->max_pending_packets)) {
				fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(client->src_ipaddr), 0);

				DEBUG("Too many pending packets for client %s - discarding packet", src_buf);
				return 0;
			}

			/*
			 *	Allocate the pending packet structure.
			 */
			pending = proto_radius_pending_alloc(client, buffer, packet_len,
							     track, *priority);
			if (!pending) {
				fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(client->src_ipaddr), 0);
				DEBUG("Failed tracking packet from client %s - discarding packet", src_buf);
				return 0;
			}

			if (fr_heap_num_elements(client->pending) > 1) {
				fr_value_box_snprint(src_buf, sizeof(src_buf), fr_box_ipaddr(client->src_ipaddr), 0);
				DEBUG("Client %s is still being dynamically defined.  Caching this packet until the client has been defined.", src_buf);
				return 0;
			}

			/*
			 *	Tell this packet that it's defining a
			 *	dynamic client.
			 */
			track->dynamic = recv_time;

		} else {
			/*
			 *	One more packet being used by this client.
			 *
			 *	Note that pending packets don't count against
			 *	the "live packet" count.
			 */
			client->packets++;
		}

		/*
		 *	Remove all cleanup timers for the client /
		 *	connection.  It's still in use, so we don't
		 *	want to clean it up.
		 */
		if (client->ev) {
			talloc_const_free(client->ev);
			client->ready_to_delete = false;
		}

		/*
		 *	Return the packet.
		 */
		*recv_time_p = &track->timestamp;
		*packet_ctx = track;
		return packet_len;
	}

	/*
	 *	We're using connected sockets, but this socket isn't
	 *	connected.  It must be the master socket.  The master
	 *	can either be STATIC, DYNAMIC, or PENDING.  Whatever
	 *	the state, the child socket will take care of handling
	 *	the packet.  e.g. dynamic clients, etc.
	 */
	if (inst->ipproto == IPPROTO_UDP) {
		bool nak = false;

		my_connection.address = &address;

		pthread_mutex_lock(&client->mutex);
		connection = fr_hash_table_finddata(client->ht, &my_connection);
		if (connection) nak = (connection->client->state == PR_CLIENT_NAK);
		pthread_mutex_unlock(&client->mutex);

		/*
		 *	The connection is in NAK state, ignore packets
		 *	for it.
		 */
		if (nak) {
			DEBUG("Discarding packet to NAKed connection %s", connection->name);
			return 0;
		}

	} else {		/* IPPROTO_TCP */
		/*
		 *	@todo TCP - accept() a new connection?
		 *	and set up address properly?  and somehow
		 *	track that we want to start a new connection,
		 *	but we don't have a packet for it...
		 *
		 *	TBH, we probably want read() and
		 *	write() to be in the listener, so that
		 *	proto_radius can set those to itself, and then
		 *	call the underlying app_io mod_read/write.
		 */
		connection = NULL;
		rad_assert(0 == 1);
	}

	/*
	 *	No existing connection, create one.
	 */
	if (!connection) {
		if (inst->max_connections) {
			/*
			 *	We've hit the connection limit.  Walk
			 *	over all clients with connections, and
			 *	count the number of connections used.
			 */
			if (inst->num_connections >= inst->max_connections) {
				inst->num_connections = 0;

				(void) fr_trie_walk(inst->trie, &inst->num_connections, count_connections);

				if ((inst->num_connections + 1) >= inst->max_connections) {
					DEBUG("Too many open connections.  Ignoring dynamic client %s.  Discarding packet.", client->radclient->shortname);
					return 0;
				}
			}
		}

		connection = proto_radius_connection_alloc(inst, client, &address, NULL);
		if (!connection) {
			DEBUG("Failed to allocate connection from client %s.  Discarding packet.", client->radclient->shortname);
			return 0;
		}

		/*
		 *	We have one more connection.  Note that we do
		 *	NOT decrement this counter when a connection
		 *	closes, as the close is done in a child
		 *	thread.  Instead, we just let counter hit the
		 *	limit, and then walk over the clients to reset
		 *	the count.
		 */
		inst->num_connections++;
	}

	DEBUG("Sending packet to connection %s", connection->name);

	/*
	 *	Inject the packet into the connected socket.  It will
	 *	process the packet as if it came in from the network.
	 *
	 *	@todo future - after creating the connection, put the current
	 *	packet into connection->pending, instead of inject?,
	 *	and then call fr_network_listen_read() from the
	 *	child's instantiation routine???

	 *	@todo TCP - for ACCEPT sockets, we don't have a
	 *	packet, so don't do this.  Instead, the connection
	 *	will take care of figuring out what to do.
	 */
	(void) fr_network_listen_inject(connection->nr, connection->listen,
					buffer, packet_len, recv_time);
	return 0;
}

/** Inject a packet to a connection.
 *
 *  Always called in the context of the network.
 */
static int mod_inject(void *instance, uint8_t *buffer, size_t buffer_len, fr_time_t recv_time)
{
	proto_radius_t	*inst;
	size_t		packet_len;
	decode_fail_t	reason;
	bool		is_dup = false;
	proto_radius_connection_t *connection;
	proto_radius_pending_packet_t *pending;
	proto_radius_track_t *track;

	get_inst(instance, &inst, &connection, NULL);

	if (!connection) {
		DEBUG2("Received injected packet for an unconnected socket.");
		return -1;
	}
	
	/*
	 *	We should still sanity check the packet.
	 */
	if (buffer_len < 20) {
		DEBUG2("Failed injecting 'too short' packet size %zd", buffer_len);
		return -1;
	}

	if ((buffer[0] == 0) || (buffer[0] > FR_MAX_PACKET_CODE)) {
		DEBUG("Failed injecting invalid packet code %d", buffer[0]);
		return -1;
	}

	if (!inst->process_by_code[buffer[0]]) {
		DEBUG("Failed injecting unexpected packet code %d", buffer[0]);
		return -1;
	}

	rad_assert(inst->priorities[buffer[0]] != 0);

	/*
	 *	Initialize the packet length.
	 */
	packet_len = buffer_len;

	/*
	 *	If it's not a RADIUS packet, ignore it.  Note that the
	 *	transport reader SHOULD have already checked
	 *	max_attributes.
	 */
	if (!fr_radius_ok(buffer, &packet_len, 0, false, &reason)) {
		DEBUG2("Failed injecting malformed packet");
		return -1;
	}

	/*
	 *	Track this packet, because that's what mod_read expects.
	 */
	track = proto_radius_track_add(connection->client, connection->address, buffer,
				       recv_time, &is_dup);
	if (!track) {
		DEBUG2("Failed injecting packet to tracking table");
		return -1;
	}

	talloc_get_type_abort(track, proto_radius_track_t);

	/*
	 *	@todo future - what to do with duplicates?
	 */
	rad_assert(!is_dup);

	/*
	 *	Remember to restore this packet later.
	 */
	pending = proto_radius_pending_alloc(connection->client, buffer, buffer_len,
					     track, inst->priorities[buffer[0]]);
	if (!pending) {
		DEBUG2("Failed injecting packet due to allocation error");
		return -1;
	}

	return 0;
}

/** Get the file descriptor for this socket.
 *
 * @param[in] const_instance of the RADIUS I/O path.
 * @return the file descriptor
 */
static int mod_fd(void const *const_instance)
{
	proto_radius_t *inst;
	proto_radius_connection_t *connection;
	void *app_io_instance;
	void *instance;

	memcpy(&instance, &const_instance, sizeof(const_instance)); /* const issues */

	get_inst((void *) instance, &inst, &connection, &app_io_instance);

	return inst->app_io->fd(app_io_instance);
}

/** Set the event list for a new socket
 *
 * @param[in] instance of the RADIUS I/O path.
 * @param[in] el the event list
 * @param[in] nr context from the network side
 */
static void mod_event_list_set(void *instance, fr_event_list_t *el, void *nr)
{
	proto_radius_t *inst;
	proto_radius_connection_t *connection;
	void *app_io_instance;

	get_inst(instance, &inst, &connection, &app_io_instance);

	/*
	 *	Dynamic clients require an event list for cleanups.
	 */
	if (!inst->dynamic_clients) {
		/*
		 *	Only Access-Request gets a cleanup delay.
		 */
		if (!inst->code_allowed[FR_CODE_ACCESS_REQUEST]) return;

		/*
		 *	And then, only if cleanup delay is non-zero.
		 */
		if ((inst->cleanup_delay.tv_sec == 0) &&
		    (inst->cleanup_delay.tv_usec == 0)) {
			return;
		}
	}

	if (!connection) {
		inst->el = el;
		inst->nr = nr;

	} else {
		connection->el = el;
		connection->nr = nr;

		/*
		 *	If the connection is pending, pause reading of
		 *	more packets.  If mod_write() accepts the
		 *	connection, it will resume reading.
		 *	Otherwise, it will close the socket without
		 *	resuming it.
		 */
		if (connection->client->state == PR_CLIENT_PENDING) {
			rad_assert(!connection->paused);

			connection->paused = true;
			(void) fr_event_filter_update(connection->el,
						      inst->app_io->fd(connection->app_io_instance),
						      FR_EVENT_FILTER_IO, pause_read);
		}
	}
}


static void client_expiry_timer(fr_event_list_t *el, struct timeval *now, void *uctx)
{
	proto_radius_client_t *client = uctx;
	proto_radius_t *inst;
	proto_radius_connection_t *connection;
	struct timeval when;
	struct timeval *delay;
	int packets, connections;

	DEBUG("TIMER - checking status of client %s", client->radclient->shortname);

	// @todo - print out what we plan on doing next

	get_inst(talloc_parent(client), &inst, &connection, NULL);

	rad_assert(client->state != PR_CLIENT_STATIC);

	/*
	 *	Called from the read or write functions with
	 *	now==NULL, to signal that we have to *set* the timer.
	 */
	if (!now) {
		switch (client->state) {
		case PR_CLIENT_CONNECTED:
			rad_assert(connection != NULL);
			delay = &inst->idle_timeout;
			break;

		case PR_CLIENT_DYNAMIC:
			delay = &inst->idle_timeout;
			break;

		case PR_CLIENT_NAK:
			rad_assert(!connection);
			delay = &inst->nak_lifetime;
			break;

		default:
			rad_assert(0 == 1);
			return;
		}

		goto reset_timer;
	}

	/*
	 *	Count active packets AND pending packets.
	 */
	packets = client->packets;
	if (client->pending) packets += fr_heap_num_elements(client->pending);

	/*
	 *	It's a negative cache entry.  Just delete it.
	 */
	if (client->state == PR_CLIENT_NAK) {
	delete_client:
		rad_assert(packets == 0);

		/*
		 *	It's a connected socket.  Remove it from the
		 *	parents list of connections, and delete it.
		 */
		if (connection) {
			proto_radius_client_t *parent = connection->parent;

			pthread_mutex_lock(&parent->mutex);
			(void) fr_hash_table_delete(parent->ht, connection);
			pthread_mutex_unlock(&parent->mutex);

			/*
			 *	Mark the connection as dead, and tell
			 *	the network side to stop reading from
			 *	it.
			 */
			connection->dead = true;
			fr_network_listen_read(connection->nr, connection->listen);
			return;
		}

		rad_assert(client->in_trie);
		rad_assert(!client->connected);
		(void) fr_trie_remove(inst->trie, &client->src_ipaddr.addr, client->src_ipaddr.prefix);

		rad_assert(inst->num_clients > 0);
		inst->num_clients--;
		talloc_free(client);
		return;
	}
	
	/*
	 *	It's a dynamically defined client.  If no one is using
	 *	it, clean it up after an idle timeout.
	 */
	if ((client->state == PR_CLIENT_DYNAMIC) ||
	    (client->state == PR_CLIENT_CONNECTED)) {
		if (packets > 0) {
			client->ready_to_delete = false;
			return;
		}

		/*
		 *	No packets, check / set idle timeout.
		 */
		goto idle_timeout;
	}

	/*
	 *	The client is pending definition.  It's either a
	 *	dynamic client which has timed out, OR it's a
	 *	"place-holder" client for connected sockets.
	 */
	rad_assert(client->state == PR_CLIENT_PENDING);

	/*
	 *	This is a dynamic client pending definition.
	 *	But it's taken too long to define, so we just
	 *	delete the client, and all packets for it.  A
	 *	new packet will cause the dynamic definition
	 *	to be run again.
	 */
	if (!client->use_connected) {
		if (!packets) {
			goto delete_client;
		}

		/*
		 *	Tell the writer to NOT dynamically define the
		 *	client.  We've run into a problem.  Then,
		 *	return.  The writer will take care of calling
		 *	us again when it notices that a PENDING client
		 *	is ready to delete.
		 *
		 *	TBH... that shouldn't happen?  We should rely
		 *	on the write to do this all of the time...
		 */
		client->ready_to_delete = true;
		return;
	}

	rad_assert(!connection);
	rad_assert(client->ht != NULL);

	/*
	 *	Find out how many connections are using this
	 *	client.
	 */
	pthread_mutex_lock(&client->mutex);
	connections = fr_hash_table_num_elements(client->ht);
	pthread_mutex_unlock(&client->mutex);

	/*
	 *	No connections are using this client.  If
	 *	we've passed the idle timeout, then just
	 *	delete it.  Otherwise, set an idle timeout (as
	 *	above);
	 */
	if (!connections) {
idle_timeout:
		/*
		 *	We didn't receive any packets during the
		 *	idle_timeout, just delete it.
		 */
		if (client->ready_to_delete) {
			if (connection) {
				DEBUG("proto_radius - idle timeout for connection %s", connection->name);
			} else {
				DEBUG("proto_radius - idle timeout for client %s", client->radclient->shortname);
			}
			goto delete_client;
		}

		/*
		 *	No packets and no idle timeout set, go set
		 *	idle timeut.
		 */
		client->ready_to_delete = true;
		delay = &inst->idle_timeout;
		goto reset_timer;
	}

	/*
	 *	There are live sub-connections.  Poll again after a
	 *	long period of time.  Once all of the connections are
	 *	closed, we can then delete this client.
	 *
	 *	@todo - maybe just leave it?  we want to be able to
	 *	clean up this client after a while tho... especially
	 *	if the total number of clients is limited.
	 */
	client->ready_to_delete = false;
	delay = &inst->check_interval;

reset_timer:
	gettimeofday(&when, NULL);
	fr_timeval_add(&when, &when, delay);

	if (fr_event_timer_insert(client, el, &client->ev,
				  &when, client_expiry_timer, client) < 0) {
		ERROR("proto_radius - Failed adding timeout for dynamic client %s.  It will be permanent!",
			client->radclient->shortname);
		return;
	}

	return;
}


static void packet_expiry_timer(fr_event_list_t *el, struct timeval *now, void *uctx)
{
	proto_radius_track_t *track = talloc_get_type_abort(uctx, proto_radius_track_t);
	proto_radius_client_t *client = track->client;
	proto_radius_t *inst = client->inst;

	/*
	 *	We're called from mod_write().  Set a cleanup_delay
	 *	for Access-Request packets.
	 */
	if (!now && (track->packet[0] == FR_CODE_ACCESS_REQUEST) &&
	    ((inst->cleanup_delay.tv_sec | inst->cleanup_delay.tv_usec) != 0)) {

		struct timeval when;

		gettimeofday(&when, NULL);
		fr_timeval_add(&when, &when, &inst->cleanup_delay);
		
		if (fr_event_timer_insert(client, el, &track->ev,
					  &when, packet_expiry_timer, track) == 0) {
			return;
		}

		DEBUG("proto_radius - Failed adding cleanup_delay for packet.  Discarding packet immediately");
	}

	/*
	 *	So that all cleanup paths can come here, not just the
	 *	timeout ones.
	 */
	if (now) {
		DEBUG2("TIMER - proto_radius cleanup delay for ID %d", track->packet[1]);
	} else {
		DEBUG2("proto_radius - cleaning up ID %d", track->packet[1]);
	}

	/*
	 *	Delete the tracking entry.
	 */
	rad_assert(track->packets > 0);
	track->packets--;

	if (track->packets == 0) {
		(void) rbtree_deletebydata(client->table, track);
		talloc_free(track);

	} else {
		if (track->reply) {
			talloc_free(track->reply);
			track->reply = NULL;
		}

		track->reply_len = 0;
	}

	rad_assert(client->packets > 0);
	client->packets--;

	/*
	 *	The client isn't dynamic, stop here.
	 */
	if (client->state == PR_CLIENT_STATIC) return;

	rad_assert(el != NULL);
	rad_assert(client->state != PR_CLIENT_NAK);
	rad_assert(client->state != PR_CLIENT_PENDING);

	/*
	 *	If necessary, call the client expiry timer to clean up
	 *	the client.
	 */
	if (client->packets == 0) {
		client_expiry_timer(el, now, client);
	}
}

static ssize_t mod_write(void *instance, void *packet_ctx,
			 fr_time_t request_time, uint8_t *buffer, size_t buffer_len)
{
	proto_radius_t *inst;
	proto_radius_connection_t *connection;
	proto_radius_track_t *track = packet_ctx;
	proto_radius_client_t *client;
	RADCLIENT *radclient;
	void *app_io_instance;
	int packets;

	get_inst(instance, &inst, &connection, &app_io_instance);

	client = track->client;
	packets = client->packets;
	if (client->pending) packets += fr_heap_num_elements(client->pending);

	/*
	 *	A well-defined client means just send the reply.
	 */
	if (client->state != PR_CLIENT_PENDING) {
		ssize_t packet_len;

		/*
		 *	The request later received a conflicting
		 *	packet, so we discard this one.
		 */
		if (track->timestamp != request_time) {
			rad_assert(track->packets > 0);
			rad_assert(client->packets > 0);
			track->packets--;
			client->packets--;
			packets--;

			DEBUG3("Suppressing reply as we have a newer packet");

			/*
			 *	No packets left for this client, reset
			 *	idle timeouts.
			 */
			if ((packets == 0) && (client->state != PR_CLIENT_STATIC)) {
				client_expiry_timer(connection ? connection->el : inst->el, NULL, client);
			}
			return buffer_len;
		}		

		rad_assert(track->reply == NULL);
		
		/*
		 *	We have a NAK packet, or the request
		 *	has timed out, and we don't respond.
		 */
		if (buffer_len < 20) {
			packet_expiry_timer(connection ? connection->el : inst->el, NULL, track);
			track->reply_len = 1; /* don't respond */
			return buffer_len;
		}

		/*
		 *	We have a real RADIUS packet, write it to the
		 *	network via the underlying transport write.
		 */

		packet_len = inst->app_io->write(app_io_instance, track, request_time,
						 buffer, buffer_len);
		if (packet_len > 0) {
			rad_assert(buffer_len == (size_t) packet_len);
			MEM(track->reply = talloc_memdup(track, buffer, buffer_len));
			track->reply_len = buffer_len;
		} else {
			track->reply_len = 1; /* don't respond */
		}

		/*
		 *	Expire the packet (if necessary).
		 */
		packet_expiry_timer(connection ? connection->el : inst->el, NULL, track);

		return packet_len;
	}

	/*
	 *	The client is pending, so we MUST have dynamic clients.
	 *
	 *	If there's a connected socket and no dynamic clients, then the
	 *	client state is set to CONNECTED when the client is created.
	 */
	rad_assert(inst->dynamic_clients);

	/*
	 *	The request has timed out trying to define the dynamic
	 *	client.  Oops... try again.
	 */
	if ((buffer_len == 1) && (*buffer == true)) {
		DEBUG("Request has timed out trying to define a new client.  Trying again.");
		goto reread;
	}

	/*
	 *	The dynamic client was NOT defined.  Set it's state to
	 *	NAK, delete all pending packets, and close the
	 *	tracking table.
	 */
	if (buffer_len == 1) {
		client->state = PR_CLIENT_NAK;
		talloc_free(client->table);
		talloc_free(client->pending);
		rad_assert(client->packets == 0);

		/*
		 *	If we're a connected socket, allocate a new
		 *	connection which is a place-holder for the
		 *	NAK.  Then, tell the network side to destroy
		 *	this connection.
		 *
		 *	The timer will take care of deleting the NAK
		 *	connection (which doesn't have any FDs
		 *	associated with it).  The network side will
		 *	call mod_close() when the original connection
		 *	is done, which will then free that connection,
		 *	too.
		 */
		if (connection) {
			connection = proto_radius_connection_alloc(inst, client, connection->address, connection);
			client_expiry_timer(connection->el, NULL, connection->client);

			errno = ECONNREFUSED;
			return -1;
		}

		client_expiry_timer(connection ? connection->el : inst->el, NULL, client);
		return buffer_len;
	}

	rad_assert(buffer_len == sizeof(radclient));

	memcpy(&radclient, buffer, sizeof(radclient));

	if (!connection) {
		fr_ipaddr_t ipaddr;

		/*
		 *	Check the encapsulating network against the
		 *	address that the user wants to use, but only
		 *	for unconnected sockets.
		 */
		if (client->network.af != radclient->ipaddr.af) {
			DEBUG("Client IP address %pV IP version does not match the source network %pV of the packet.",
			      fr_box_ipaddr(radclient->ipaddr), fr_box_ipaddr(client->network));
			goto error;
		}

		/*
		 *	Network prefix is more restrictive than the one given
		 *	by the client... that's bad.
		 */
		if (client->network.prefix > radclient->ipaddr.prefix) {
			DEBUG("Client IP address %pV is not within the prefix with the defined network %pV",
			      fr_box_ipaddr(radclient->ipaddr), fr_box_ipaddr(client->network));
			goto error;
		}

		ipaddr = radclient->ipaddr;
		fr_ipaddr_mask(&ipaddr, client->network.prefix);
		if (fr_ipaddr_cmp(&ipaddr, &client->network) != 0) {
			DEBUG("Client IP address %pV is not within the defined network %pV.",
			      fr_box_ipaddr(radclient->ipaddr), fr_box_ipaddr(client->network));
			goto error;
		}

		/*
		 *	We can't define dynamic clients as networks (for now).
		 *
		 *	@todo - If we did allow it, we would have to remove
		 *	this client from the trie, update it's IP address, and
		 *	re-add it.  We can PROBABLY do this if this client
		 *	isn't already connected, AND radclient->use_connected
		 *	is true.  But that's for later...
		 */
		if (((radclient->ipaddr.af == AF_INET) &&
		     (radclient->ipaddr.prefix != 32)) ||
		    ((radclient->ipaddr.af == AF_INET6) &&
		     (radclient->ipaddr.prefix != 128))) {
			ERROR("prot_radius - Cannot define a dynamic client as a network");

		error:
			talloc_free(radclient);

			/*
			 *	Remove the pending client from the trie.
			 */
			if (!connection) {
				rad_assert(client->in_trie);
				rad_assert(!client->connected);
				(void) fr_trie_remove(inst->trie, &client->src_ipaddr.addr, client->src_ipaddr.prefix);
				rad_assert(inst->num_clients > 0);
				inst->num_clients--;
				talloc_free(client);
				return buffer_len;
			}

			/*
			 *	Remove this connection from the parents list of connections.
			 */
			pthread_mutex_lock(&connection->parent->mutex);
			(void) fr_hash_table_delete(connection->parent->ht, connection);
			pthread_mutex_unlock(&connection->parent->mutex);

			talloc_free(connection);
			return buffer_len;
		}
	}

	/*
	 *	The new client is mostly OK.  Copy the various fields
	 *	over.
	 */
#define COPY_FIELD(_x) client->radclient->_x = radclient->_x
#define DUP_FIELD(_x) client->radclient->_x = talloc_strdup(client->radclient, radclient->_x)

	/*
	 *	Only these two fields are set.  Other strings in
	 *	radclient are copies of these ones.
	 */
	talloc_const_free(client->radclient->shortname);
	talloc_const_free(client->radclient->secret);

	DUP_FIELD(longname);
	DUP_FIELD(shortname);
	DUP_FIELD(secret);
	DUP_FIELD(nas_type);

	COPY_FIELD(ipaddr);
	COPY_FIELD(message_authenticator);
	COPY_FIELD(use_connected);

	// @todo - fill in other fields?

	talloc_free(radclient);

	radclient = client->radclient; /* laziness */
	radclient->server_cs = inst->server_cs;
	radclient->server = cf_section_name2(inst->server_cs);
	radclient->cs = NULL;

	/*
	 *	This is a connected socket, and it's just been
	 *	allowed.  Go poke the network side to read from the
	 *	socket.
	 */
	if (connection) {
		rad_assert(connection != NULL);
		rad_assert(connection->client == client);
		rad_assert(client->connected == true);

		client->state = PR_CLIENT_CONNECTED;

		radclient->active = true;

		/*
		 *	Connections can't spawn new connections.
		 */
		client->use_connected = radclient->use_connected = false;

		/*
		 *	If we were paused. resume reading from the
		 *	connection.
		 *
		 *	Note that the event list doesn't like resuming
		 *	a connection that isn't paused.  It just sets
		 *	the read function to NULL.
		 */
		if (connection->paused) {
			(void) fr_event_filter_update(connection->el,
						      inst->app_io->fd(connection->app_io_instance),
						      FR_EVENT_FILTER_IO, resume_read);
		}

		goto finish;
	}

	rad_assert(connection == NULL);
	rad_assert(client->use_connected == false); /* we weren't sure until now */

	/*
	 *	Dynamic clients can spawn new connections.
	 */
	client->use_connected = radclient->use_connected;

	/*
	 *	The admin has defined a client which uses connected
	 *	sockets.  Go spawn it
	 */
	if (client->use_connected) {
		rad_assert(connection == NULL);

		/*
		 *	Leave the state as PENDING.  Each connection
		 *	will then cause a dynamic client to be
		 *	defined.
		 */
		(void) pthread_mutex_init(&client->mutex, NULL);
		MEM(client->ht = fr_hash_table_create(client, connection_hash, connection_cmp, NULL));

	} else {
		/*
		 *	The client has been allowed.
		 */
		client->state = PR_CLIENT_DYNAMIC;
		client->radclient->active = true;
	}

	/*
	 *	Add this client to the master socket, so that
	 *	mod_read() will see the pending client, pop the
	 *	pending packet, and process it.
	 *
	 */
	if (!inst->pending_clients) {		
		MEM(inst->pending_clients = fr_heap_create(client, pending_client_cmp,
							   proto_radius_client_t, heap_id));
	}

	rad_assert(client->heap_id < 0);
	(void) fr_heap_insert(inst->pending_clients, client);

finish:
	/*
	 *	Maybe we defined the client, but the original packet
	 *	timed out, so there's nothing more to do.  In that case, set up the expiry timers.
	 */
	if (packets == 0) {
		client_expiry_timer(connection ? connection->el : inst->el, NULL, client);
	}

reread:
	/*
	 *	If there are pending packets (and there should be at
	 *	least one), tell the network socket to call our read()
	 *	function again.
	 */
	if (fr_heap_num_elements(client->pending) > 0) {
		if (connection) {
			fr_network_listen_read(connection->nr, connection->listen);
		} else {
			fr_network_listen_read(inst->nr, inst->listen);
		}
	}

	return buffer_len;
}

/** Close the socket.
 *
 * @param[in] instance of the RADIUS I/O path.
 * @return
 *	- <0 on error
 *	- 0 on success
 */
static int mod_close(void *instance)
{
	proto_radius_t *inst;
	proto_radius_connection_t *connection;
	void *app_io_instance;
	int rcode;

	get_inst(instance, &inst, &connection, &app_io_instance);

	rcode = inst->app_io->close(app_io_instance);
	if (rcode < 0) return rcode;

	/*
	 *	We allocated this, so we're responsible for closing
	 *	it.
	 */
	if (connection) {
		DEBUG("Closing connection %s", connection->name);
		if (connection->client->pending) {
			TALLOC_FREE(connection->client->pending); /* for any pending packets */
		}
		talloc_free(connection->dl_inst);
	}

	return 0;
}


static int mod_detach(void *instance)
{
	proto_radius_t *inst;
	proto_radius_connection_t *connection;
	void *app_io_instance;
	int rcode;

	get_inst(instance, &inst, &connection, &app_io_instance);

	rcode = inst->app_io->detach(app_io_instance);
	if (rcode < 0) return rcode;

	return 0;
}


fr_app_io_t proto_radius_master_io = {
	.magic			= RLM_MODULE_INIT,
	.name			= "radius_master_io",

	.detach			= mod_detach,
//	.bootstrap		= mod_bootstrap,
//	.instantiate		= mod_instantiate,

	.default_message_size	= 4096,
	.track_duplicates	= true,

	.read			= mod_read,
	.write			= mod_write,
	.inject			= mod_inject,

	.close			= mod_close,
	.fd			= mod_fd,
	.event_list_set		= mod_event_list_set,
};
