/*
 * dhcperfcli.c
 */

#include "dhcperfcli.h"
#include "dpc_util.h"


static char const *prog_version = "(FreeRADIUS version " RADIUSD_VERSION_STRING ")"
#ifdef RADIUSD_VERSION_COMMIT
" (git #" STRINGIFY(RADIUSD_VERSION_COMMIT) ")"
#endif
", built on " __DATE__ " at " __TIME__;


/*
 *	Global variables.
 */
char const *radius_dir = RADDBDIR;
char const *dict_dir = DICTDIR;
fr_dict_t *dict = NULL;
int	dpc_debug_lvl = 0;

TALLOC_CTX *autofree = NULL;
char const *progname = NULL;
fr_event_list_t *event_list = NULL;

static char const *file_vps_in = NULL;
static dpc_input_list_t vps_list_in = { 0 };

static fr_ipaddr_t server_ipaddr = { 0 };
static fr_ipaddr_t client_ipaddr = { 0 };
static uint16_t server_port = DHCP_PORT_SERVER;
static int force_af = AF_INET; // we only do DHCPv4.
static int packet_code = FR_CODE_UNDEFINED;

static float timeout = 3.0;
static struct timeval tv_timeout;

static const FR_NAME_NUMBER request_types[] = {
	{ "discover", FR_DHCPV4_DISCOVER },
	{ "request",  FR_DHCPV4_REQUEST },
	{ "decline",  FR_DHCPV4_DECLINE },
	{ "release",  FR_DHCPV4_RELEASE },
	{ "inform",   FR_DHCPV4_INFORM },
	{ "lease_query",  FR_DHCPV4_LEASE_QUERY },
	{ "auto",     FR_CODE_UNDEFINED },
	{ NULL, 0}
};


/*
 *	Static functions declaration.
 */
static void usage(int);
static dpc_input_t *dpc_get_input_list_head(dpc_input_list_t *list);
static VALUE_PAIR *dpc_pair_list_append(TALLOC_CTX *ctx, VALUE_PAIR **to, VALUE_PAIR *from);
static void dpc_packet_print(FILE *fp, RADIUS_PACKET *packet, bool received);


/*
 *	Basic send / receive, for now.
 */
static int my_sockfd = -1;
static int send_with_socket(RADIUS_PACKET **reply, RADIUS_PACKET *request)
{
	int on = 1;

	if (my_sockfd != -1) { // for now: just reopen a socket each time we have a packet to send.
		close(my_sockfd);
		my_sockfd = -1;
	}

	if (my_sockfd == -1) {
		/* Open a connectionless UDP socket for sending and receiving. */
		my_sockfd = fr_socket_server_udp(&request->src_ipaddr, &request->src_port, NULL, false);
		if (my_sockfd < 0) {
			ERROR("Error opening socket: %s", fr_strerror());
			return -1;
		}

		if (fr_socket_bind(my_sockfd, &request->src_ipaddr, &request->src_port, NULL) < 0) {
			ERROR("Error binding socket: %s", fr_strerror());
			return -1;
		}
	}

	/*
	 *	Set option 'receive timeout' on socket.
	 *	Note: in case of a timeout, the error will be "Resource temporarily unavailable".
	 */
	if (setsockopt(my_sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv_timeout, sizeof(struct timeval)) == -1) {
		ERROR("Failed setting socket timeout: %s", fr_syserror(errno));
		return -1;
	}

	if (setsockopt(my_sockfd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
		ERROR("Can't set broadcast option: %s", fr_syserror(errno));
		return -1;
	}
	request->sockfd = my_sockfd;

	dpc_socket_inspect(fr_log_fp, my_sockfd, NULL, NULL, NULL, NULL); // debug socket

	DPC_DEBUG_TRACE("sending one packet, id: %u", request->id);
	DPC_DEBUG_HEX_DUMP("data", request->data, request->data_len);

	if (fr_dhcpv4_udp_packet_send(request) < 0) { /* Send using a connectionless UDP socket (sendfromto). */
		ERROR("Failed sending: %s", fr_syserror(errno));
		return -1;
	}

	*reply = fr_dhcpv4_udp_packet_recv(my_sockfd); /* Receive using a connectionless UDP socket (recvfromto). */
	if (!*reply) {
		if (errno == EAGAIN) {
			fr_strerror(); /* Clear the error buffer */
			ERROR("Timed out waiting for reply");
		} else {
			ERROR("Error receiving reply");
		}
		return -1;
	}

	return 0;
}

static RADIUS_PACKET *request_init(dpc_input_t *input)
{
	vp_cursor_t cursor;
	VALUE_PAIR *vp;
	RADIUS_PACKET *request;

	MEM(request = fr_radius_alloc(input, true));

	// fill in the packet value pairs
	dpc_pair_list_append(request, &request->vps, input->vps);

	/*
	 *	Fix / set various options
	 */
	for (vp = fr_pair_cursor_init(&cursor, &input->vps);
	     vp;
	     vp = fr_pair_cursor_next(&cursor)) {
		/*
		 *	Allow to set packet type using DHCP-Message-Type
		 */
		if (vp->da->vendor == DHCP_MAGIC_VENDOR && vp->da->attr == FR_DHCPV4_MESSAGE_TYPE) {
			request->code = vp->vp_uint32 + FR_DHCPV4_OFFSET;
		} else if (!vp->da->vendor) switch (vp->da->attr) {
		/*
		 *	Also allow to set packet type using Packet-Type
		 *	(this takes precedence over the command argument.)
		 */
		case FR_PACKET_TYPE:
			request->code = vp->vp_uint32;
			break;

		case FR_PACKET_DST_PORT:
			request->dst_port = vp->vp_uint16;
			break;

		case FR_PACKET_DST_IP_ADDRESS:
		case FR_PACKET_DST_IPV6_ADDRESS:
			memcpy(&request->dst_ipaddr, &vp->vp_ip, sizeof(request->src_ipaddr));
			break;

		case FR_PACKET_SRC_PORT:
			request->src_port = vp->vp_uint16;
			break;

		case FR_PACKET_SRC_IP_ADDRESS:
		case FR_PACKET_SRC_IPV6_ADDRESS:
			memcpy(&request->src_ipaddr, &vp->vp_ip, sizeof(request->src_ipaddr));
			break;

		default:
			break;
		} /* switch over the attribute */

	} /* loop over the input vps */

	/*
	 *	Set defaults if they weren't specified via pairs
	 */
	if (request->src_port == 0) request->src_port = server_port + 1;
	if (request->dst_port == 0) request->dst_port = server_port;
	if (request->src_ipaddr.af == AF_UNSPEC) request->src_ipaddr = client_ipaddr;
	if (request->dst_ipaddr.af == AF_UNSPEC) request->dst_ipaddr = server_ipaddr;
	if (!request->code) request->code = packet_code;

	if (!request->code) {
		ERROR("No packet type specified in command line or input vps");
		return NULL;
	}

	return request;
}

static void dpc_do_request(void)
{
	RADIUS_PACKET *request = NULL;
	RADIUS_PACKET *reply = NULL;
	UNUSED int ret;

	// grab one input entry
	dpc_input_t *input = dpc_get_input_list_head(&vps_list_in);

	request = request_init(input);
	if (request) {
		if (fr_debug_lvl > 1) {
			DEBUG2("Request input vps:");
			fr_pair_list_fprint(fr_log_fp, request->vps);
		}

		/*
		 *	Encode the packet
		 */
		if (fr_dhcpv4_packet_encode(request) < 0) {
			ERROR("Failed encoding request packet");
			exit(EXIT_FAILURE);
		}
		fr_strerror(); /* Clear the error buffer */

		//if (fr_debug_lvl) {
		//	fr_dhcpv4_packet_decode(request);
		//	dhcp_packet_debug(request, false);
		//}
		dpc_packet_print(fr_log_fp, request, false); /* print request packet. */

		ret = send_with_socket(&reply, request);

		if (reply) {
			if (fr_dhcpv4_packet_decode(reply) < 0) {
				ERROR("Failed decoding reply packet");
				ret = -1;
			}

			dpc_packet_print(fr_log_fp, reply, true); /* print reply packet. */
		}
	}

	talloc_free(input);
}

/*
 *	Print an ethernet address in a buffer.
 */
static char *ether_addr_print(const uint8_t *addr, char *buf)
{
	sprintf (buf, "%02x:%02x:%02x:%02x:%02x:%02x",
		addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	return buf;
}

/*
 *	Print the packet header.
 */
static void dpc_packet_header_print(FILE *fp, RADIUS_PACKET *packet, bool received)
{
	char src_ipaddr[128] = "";
	char dst_ipaddr[128] = "";

	uint32_t yiaddr;
	char lease_ipaddr[128] = "";
	uint8_t hwaddr[6] = "";
	char buf_hwaddr[128] = "";

	if (!fp) return;
	if (!packet) return;

	/* Internally, DHCP packet code starts with an offset of 1024 (hack), so... */
	int code = packet->code - FR_DHCPV4_OFFSET;

	fprintf(fp, "%s", received ? "Received" : "Sent");

	if (is_dhcp_code(code)) {
		fprintf(fp, " %s", dhcp_message_types[code]);
	} else {
		fprintf(fp, " DHCP packet");
		if (code <= 0) fprintf(fp, " (BOOTP)"); /* No DHCP Message Type: BOOTP (or malformed DHCP packet). */
		else fprintf(fp, " (code %u)", code);
	}

	/* DHCP specific information */
	memcpy(hwaddr, packet->data + 28, sizeof(hwaddr));
	fprintf(fp, " (hwaddr: %s", ether_addr_print(hwaddr, buf_hwaddr) );

	if (packet->code == FR_DHCPV4_ACK || packet->code == FR_DHCPV4_OFFER) {
		memcpy(&yiaddr, packet->data + 16, 4);
		fprintf(fp, ", yiaddr: %s", inet_ntop(AF_INET, &yiaddr, lease_ipaddr, sizeof(lease_ipaddr)) );
	}
	fprintf(fp, ")");

	/* Generic protocol information. */
	fprintf(fp, " Id %u (0x%08x) from %s:%i to %s:%i length %zu\n",
	        packet->id, packet->id,
	        inet_ntop(packet->src_ipaddr.af, &packet->src_ipaddr.addr, src_ipaddr, sizeof(src_ipaddr)),
	        packet->src_port,
	        inet_ntop(packet->dst_ipaddr.af, &packet->dst_ipaddr.addr, dst_ipaddr, sizeof(dst_ipaddr)),
	        packet->dst_port,
	        packet->data_len);
}

/*
 *	Print the "fields" (options excluded) of a DHCP packet (from the VPs list).
 */
static void dpc_packet_fields_print(FILE *fp, VALUE_PAIR *vp)
{
	fr_cursor_t cursor;

	for (vp = fr_cursor_init(&cursor, &vp); vp; vp = fr_cursor_next(&cursor)) {
		if ((vp->da->vendor == DHCP_MAGIC_VENDOR) && (vp->da->attr >= 256 && vp->da->attr <= 269)) {
			fr_pair_fprint(fp, vp);
		}
	}
}

/*
 *	Print the "options" of a DHCP packet (from the VPs list).
 */
static int dpc_packet_options_print(FILE *fp, VALUE_PAIR *vp)
{
	char buf[1024];
	char *p = buf;
	int num = 0; /* Keep track of how many options we have. */

	fr_cursor_t cursor;
	for (vp = fr_cursor_init(&cursor, &vp); vp; vp = fr_cursor_next(&cursor)) {
		if ((vp->da->vendor == DHCP_MAGIC_VENDOR) && !(vp->da->attr >= 256 && vp->da->attr <= 269)) {
			num ++;

			p = buf;
			*p++ = '\t';

			if (vp->da->parent && vp->da->parent->type == FR_TYPE_TLV) {
				/* If attribute has a parent which is of type "tlv", print <option.sub-attr> (eg. "82.1"). */
				p += sprintf(p, "(%d.%d) ", vp->da->parent->attr, vp->da->attr);
			} else {
				/* Otherwise this is a simple option. */
				p += sprintf(p, "(%d) ", vp->da->attr);
			}

			p += fr_pair_snprint(p, sizeof(buf) - 1, vp);
			*p++ = '\n';
			*p = '\0';

			fputs(buf, fp);
		}
	}
	return num;
}

/*
 * Print a DHCP packet.
 */
static void dpc_packet_print(FILE *fp, RADIUS_PACKET *packet, bool received)
{
	dpc_packet_header_print(fp, packet, received);

	fprintf(fp, "DHCP vps fields:\n");
	dpc_packet_fields_print(fp, packet->vps);

	fprintf(fp, "DHCP vps options:\n");
	if (dpc_packet_options_print(fp, packet->vps) == 0) {
		fprintf(fp, "\t(empty list)\n");
	}
}

/*
 * Convert a float to struct timeval.
 */
static void dpc_float_to_timeval(struct timeval *tv, float f_val)
{
	tv->tv_sec = (time_t)f_val;
	tv->tv_usec = (uint64_t)(f_val * USEC) - (tv->tv_sec * USEC);
}

/*
 *	Append a list of VP. (inspired from FreeRADIUS's fr_pair_list_copy.)
 */
static VALUE_PAIR *dpc_pair_list_append(TALLOC_CTX *ctx, VALUE_PAIR **to, VALUE_PAIR *from)
{
	vp_cursor_t src, dst;

	if (NULL == *to) { // fall back to fr_pair_list_copy for a new list.
		*to = fr_pair_list_copy(ctx, from);
		return (*to);
	}

	VALUE_PAIR *out = *to, *vp;

	fr_pair_cursor_init(&dst, &out);
	for (vp = fr_pair_cursor_init(&src, &from);
	     vp;
	     vp = fr_pair_cursor_next(&src)) {
		VP_VERIFY(vp);
		vp = fr_pair_copy(ctx, vp);
		if (!vp) {
			fr_pair_list_free(&out);
			return NULL;
		}
		fr_pair_cursor_append(&dst, vp); /* fr_pair_list_copy sets next pointer to NULL */
	}

	return *to;
}

/*
 *	Add an allocated input entry to the tail of the list.
 */
static void dpc_input_item_add(dpc_input_list_t *list, dpc_input_t *entry)
{
	if (!list || !entry) return;

	if (!list->head) {
		assert(list->tail == NULL);
		list->head = entry;
		entry->prev = NULL;
	} else {
		assert(list->tail != NULL);
		assert(list->tail->next == NULL);
		list->tail->next = entry;
		entry->prev = list->tail;
	}
	list->tail = entry;
	entry->next = NULL;
	entry->list = list;
	list->size ++;
}

/*
 *	Remove an input entry from its list.
 */
static dpc_input_t *dpc_input_item_draw(dpc_input_t *entry)
{
	if (!entry) return NULL; // should not happen.
	if (!entry->list) return entry; // not in a list: just return the entry.

	dpc_input_t *prev, *next;

	prev = entry->prev;
	next = entry->next;

	dpc_input_list_t *list = entry->list;

	assert(list->head != NULL); // entry belongs to a list, so the list can't be empty.
	assert(list->tail != NULL); // same.

	if (prev) {
		assert(list->head != entry); // if entry has a prev, then entry can't be head.
		prev->next = next;
	}
	else {
		assert(list->head == entry); // if entry has no prev, then entry must be head.
		list->head = next;
	}

	if (next) {
		assert(list->tail != entry); // if entry has a next, then entry can't be tail.
		next->prev = prev;
	}
	else {
		assert(list->tail == entry); // if entry has no next, then entry must be tail.
		list->tail = prev;
	}

	entry->list = NULL;
	entry->prev = NULL;
	entry->next = NULL;
	list->size --;
	return entry;
}

/*
 *	Get the head input entry from a list.
 */
static dpc_input_t *dpc_get_input_list_head(dpc_input_list_t *list)
{
	if (!list) return NULL;
	if (!list->head || list->size == 0) { // list is empty.
		return NULL;
	}
	// list is valid and has at least one element.
	return dpc_input_item_draw(list->head);
}

/*
 *	Handle a list of input vps we've just read.
 */
static void dpc_handle_input(dpc_input_t *input)
{
	// for now, just trace what we've read.
	if (fr_debug_lvl > 1) {
		DEBUG2("Input vps read:");
		fr_pair_list_fprint(fr_log_fp, input->vps);
	}

	dpc_input_item_add(&vps_list_in, input);
}

/*
 *	Load input vps.
 */
static void dpc_input_load_from_fd(TALLOC_CTX *ctx, FILE *file_in)
{
	bool file_done = false;
	dpc_input_t *input;

	/*
	 *	Loop until the file is done.
	 */
	do {
		MEM(input = talloc_zero(ctx, dpc_input_t));

		if (fr_pair_list_afrom_file(input, &input->vps, file_in, &file_done) < 0) {
			ERROR("Error parsing input vps");
			talloc_free(input);
			break;
		}
		if (NULL == input->vps) {
			/* Last line might be empty, in this case we will obtain a NULL vps pointer. Silently ignore this. */
			talloc_free(input);
			break;
		}

		dpc_handle_input(input);

	} while (!file_done);
	fr_strerror(); /* Clear the error buffer */

	DEBUG("Done reading input, list size: %d", vps_list_in.size);
}

/*
 *	Load input vps, either from a file if specified, or stdin otherwise.
 */
static int dpc_input_load(TALLOC_CTX *ctx)
{
	FILE *file_in = NULL;

	/*
	 *	Determine where to read the vps from.
	 */
	if (file_vps_in && strcmp(file_vps_in, "-") != 0) {
		DEBUG("Opening input file: %s", file_vps_in);

		file_in = fopen(file_vps_in, "r");
		if (!file_in) {
			ERROR("Error opening %s: %s", file_vps_in, strerror(errno));
			return -1;
		}
	} else {
		DEBUG("Reading input vps from stdin");
		file_in = stdin;
	}

	dpc_input_load_from_fd(ctx, file_in);

	if (file_in != stdin) fclose(file_in);
	return 0;
}

/*
 *	Load dictionaries.
 */
static void dpc_dict_init(void)
{
	fr_dict_attr_t const *da;

	DEBUG("Including dictionary file \"%s/%s\"", dict_dir, FR_DICTIONARY_FILE);
	if (fr_dict_from_file(NULL, &dict, dict_dir, FR_DICTIONARY_FILE, "dhcperfcli") < 0) {
		fr_perror("dhcperfcli");
		exit(EXIT_FAILURE);
	}

	DEBUG("Including dictionary file \"%s/%s\"", radius_dir, FR_DICTIONARY_FILE);
	if (fr_dict_read(dict, radius_dir, FR_DICTIONARY_FILE) == -1) {
		fr_log_perror(&default_log, L_ERR, "Failed to initialize the dictionaries");
		exit(EXIT_FAILURE);
	}
	fr_strerror(); /* Clear the error buffer */

	/*
	 *	Ensure that dictionary.dhcp is loaded.
	 */
	da = fr_dict_attr_by_name(NULL, "DHCP-Message-Type");
	if (!da) {
		if (fr_dict_read(dict, dict_dir, "dictionary.dhcp") < 0) {
			ERROR("Failed reading dictionary.dhcp");
			exit(EXIT_FAILURE);
		}
	}
}

/*
 *	Initialize event list.
 */
static void dpc_event_init(TALLOC_CTX *ctx)
{
	event_list = fr_event_list_alloc(ctx, NULL, NULL);
	if (!event_list) {
		ERROR("Failed to create event list");
		exit(EXIT_FAILURE);
	}
}

/*
 *	Resolve host address and port.
 */
static void dpc_host_addr_resolve(char *host_arg, fr_ipaddr_t *host_ipaddr, uint16_t *host_port)
{
	if (!host_arg || !host_ipaddr || !host_port) return;

	uint16_t port;

	if (fr_inet_pton_port(host_ipaddr, &port, host_arg, -1, force_af, true, true) < 0) {
		ERROR("Failed to parse host address");
		exit(EXIT_FAILURE);
	}

	if (port != 0) { /* If a port is specified, use it. Otherwise, keep default. */
		*host_port = port;
	}
}

/*
 *	See what kind of request we want to send.
 */
static void dpc_command_parse(char const *command)
{
	// request types (or "auto")
	if (!isdigit((int) command[0])) {
		packet_code = fr_str2int(request_types, command, -2);
		if (packet_code == -2) {
			ERROR("Unrecognised packet type \"%s\"", command);
			usage(1);
		}
	} else {
		packet_code = atoi(command);
	}
}

/*
 *	Process command line options and arguments.
 */
static void dpc_options_parse(int argc, char **argv)
{
	int argval;

	while ((argval = getopt(argc, argv, "f:ht:vx")) != EOF) {
		switch (argval) {
		case 'f':
			file_vps_in = optarg;
			break;

		case 'h':
			usage(0);
			break;

		case 't':
			if (!isdigit((int) *optarg)) usage(1);
			timeout = atof(optarg);
			break;

		case 'v':
			printf("%s %s\n", progname, prog_version);
			exit(0);

		case 'x':
			dpc_debug_lvl ++;
			fr_debug_lvl ++; // TODO: see how we handle this
			break;

		default:
			usage(1);
			break;
		}
	}
	argc -= (optind - 1);
	argv += (optind - 1);

	/*
	 *	Resolve server host address and port.
	 */
	if (argc - 1 >= 1 && strcmp(argv[1], "-") != 0) {
		dpc_host_addr_resolve(argv[1], &server_ipaddr, &server_port);
		client_ipaddr.af = server_ipaddr.af;
	}

	/*
	 *	See what kind of request we want to send.
	 */
	if (argc - 1 >= 2) {
		dpc_command_parse(argv[2]);
	}

	dpc_float_to_timeval(&tv_timeout, timeout);
}



/*
 *	The main guy.
 */
int main(int argc, char **argv)
{
	char *p;

	fr_debug_lvl = 0; /* FreeRADIUS libraries debug. */
	dpc_debug_lvl = 0; /* Our own debug. */
	fr_log_fp = stdout; /* Both will go there. */

	/* Get program name from argv. */
	p = strrchr(argv[0], FR_DIR_SEP);
	if (!p) {
		progname = argv[0];
	} else {
		progname = p + 1;
	}

	dpc_options_parse(argc, argv);

	dpc_dict_init();

	dpc_event_init(autofree);

	dpc_input_load(autofree);

	// for now
	while (vps_list_in.size > 0) {
		dpc_do_request();
	}

	exit(0);
}

/*
 *	Display the syntax for starting this program.
 */
static void NEVER_RETURNS usage(int status)
{
	FILE *output = status?stderr:stdout;

	fprintf(output, "Usage: %s [options] [<server>[:<port>] [<command>]]\n", progname);
	fprintf(output, "  <server>:<port>  The DHCP server. If omitted, it must be specified in inputs vps.\n");
	fprintf(output, "  <command>        One of (packet type): discover, request, decline, release, inform.\n");
	fprintf(output, "                   If omitted, packet type must be specified in input vps.\n");
	fprintf(output, " Options:\n");
	fprintf(output, "  -f <file>        Read input vps from <file>, not stdin.\n");
	fprintf(output, "  -h               Print this help message.\n");
	fprintf(output, "  -t <timeout>     Wait at most <timeout> seconds for a reply (may be a floating point number).\n");
	fprintf(output, "  -v               Print version information.\n");
	fprintf(output, "  -x               Turn on additional debugging. (-xx gives more debugging).\n");

	exit(status);
}
