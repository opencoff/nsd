/*
 * server.c -- nsd(8) network input/output
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <netinet/in.h>
#ifdef USE_TCP_FASTOPEN
  #include <netinet/tcp.h>
#endif
#include <arpa/inet.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <poll.h>
#ifndef SHUT_WR
#define SHUT_WR 1
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif /* HAVE_MMAP */
#ifdef HAVE_OPENSSL_RAND_H
#include <openssl/rand.h>
#endif
#ifdef HAVE_OPENSSL_SSL_H
#include <openssl/ssl.h>
#endif
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#ifdef HAVE_OPENSSL_OCSP_H
#include <openssl/ocsp.h>
#endif
#ifndef USE_MINI_EVENT
#  ifdef HAVE_EVENT_H
#    include <event.h>
#  else
#    include <event2/event.h>
#    include "event2/event_struct.h"
#    include "event2/event_compat.h"
#  endif
#else
#  include "mini_event.h"
#endif

#include "axfr.h"
#include "namedb.h"
#include "netio.h"
#include "xfrd.h"
#include "xfrd-tcp.h"
#include "xfrd-disk.h"
#include "difffile.h"
#include "nsec3.h"
#include "ipc.h"
#include "udb.h"
#include "remote.h"
#include "lookup3.h"
#include "rrl.h"
#ifdef USE_DNSTAP
#include "dnstap/dnstap_collector.h"
#endif

#define RELOAD_SYNC_TIMEOUT 25 /* seconds */

#ifdef USE_TCP_FASTOPEN
  #define TCP_FASTOPEN_FILE "/proc/sys/net/ipv4/tcp_fastopen"
  #define TCP_FASTOPEN_SERVER_BIT_MASK 0x2
#endif

/*
 * Data for the UDP handlers.
 */
struct udp_handler_data
{
	struct nsd        *nsd;
	struct nsd_socket *socket;
	query_type        *query;
};

struct tcp_accept_handler_data {
	struct nsd         *nsd;
	struct nsd_socket  *socket;
	int event_added;
	struct event       event;
#ifdef HAVE_SSL
	/* handler accepts TLS connections on the dedicated port */
	int tls_accept;
#endif
};

/*
 * These globals are used to enable the TCP accept handlers
 * when the number of TCP connection drops below the maximum
 * number of TCP connections.
 */
static size_t		tcp_accept_handler_count;
static struct tcp_accept_handler_data*	tcp_accept_handlers;

static struct event slowaccept_event;
static int slowaccept;

#ifdef HAVE_SSL
static unsigned char *ocspdata = NULL;
static long ocspdata_len = 0;
#endif

#ifndef NONBLOCKING_IS_BROKEN
#  define NUM_RECV_PER_SELECT 100
#endif

#if (!defined(NONBLOCKING_IS_BROKEN) && defined(HAVE_RECVMMSG))
struct mmsghdr msgs[NUM_RECV_PER_SELECT];
struct iovec iovecs[NUM_RECV_PER_SELECT];
struct query *queries[NUM_RECV_PER_SELECT];
#endif

/*
 * Data for the TCP connection handlers.
 *
 * The TCP handlers use non-blocking I/O.  This is necessary to avoid
 * blocking the entire server on a slow TCP connection, but does make
 * reading from and writing to the socket more complicated.
 *
 * Basically, whenever a read/write would block (indicated by the
 * EAGAIN errno variable) we remember the position we were reading
 * from/writing to and return from the TCP reading/writing event
 * handler.  When the socket becomes readable/writable again we
 * continue from the same position.
 */
struct tcp_handler_data
{
	/*
	 * The region used to allocate all TCP connection related
	 * data, including this structure.  This region is destroyed
	 * when the connection is closed.
	 */
	region_type*		region;

	/*
	 * The global nsd structure.
	 */
	struct nsd*			nsd;

	/*
	 * The current query data for this TCP connection.
	 */
	query_type*			query;

	/*
	 * The query_state is used to remember if we are performing an
	 * AXFR, if we're done processing, or if we should discard the
	 * query and connection.
	 */
	query_state_type	query_state;

	/*
	 * The event for the file descriptor and tcp timeout
	 */
	struct event event;

	/*
	 * The bytes_transmitted field is used to remember the number
	 * of bytes transmitted when receiving or sending a DNS
	 * packet.  The count includes the two additional bytes used
	 * to specify the packet length on a TCP connection.
	 */
	size_t				bytes_transmitted;

	/*
	 * The number of queries handled by this specific TCP connection.
	 */
	int					query_count;
	
	/*
	 * The timeout in msec for this tcp connection
	 */
	int	tcp_timeout;
#ifdef HAVE_SSL
	/*
	 * TLS object.
	 */
	SSL* tls;

	/*
	 * TLS handshake state.
	 */
	enum { tls_hs_none, tls_hs_read, tls_hs_write,
		tls_hs_read_event, tls_hs_write_event } shake_state;
#endif
	/* list of connections, for service of remaining tcp channels */
	struct tcp_handler_data *prev, *next;
};
/* global that is the list of active tcp channels */
static struct tcp_handler_data *tcp_active_list = NULL;

/*
 * Handle incoming queries on the UDP server sockets.
 */
static void handle_udp(int fd, short event, void* arg);

/*
 * Handle incoming connections on the TCP sockets.  These handlers
 * usually wait for the NETIO_EVENT_READ event (indicating an incoming
 * connection) but are disabled when the number of current TCP
 * connections is equal to the maximum number of TCP connections.
 * Disabling is done by changing the handler to wait for the
 * NETIO_EVENT_NONE type.  This is done using the function
 * configure_tcp_accept_handlers.
 */
static void handle_tcp_accept(int fd, short event, void* arg);

/*
 * Handle incoming queries on a TCP connection.  The TCP connections
 * are configured to be non-blocking and the handler may be called
 * multiple times before a complete query is received.
 */
static void handle_tcp_reading(int fd, short event, void* arg);

/*
 * Handle outgoing responses on a TCP connection.  The TCP connections
 * are configured to be non-blocking and the handler may be called
 * multiple times before a complete response is sent.
 */
static void handle_tcp_writing(int fd, short event, void* arg);

#ifdef HAVE_SSL
/* Create SSL object and associate fd */
static SSL* incoming_ssl_fd(SSL_CTX* ctx, int fd);
/*
 * Handle TLS handshake. May be called multiple times if incomplete.
 */
static int tls_handshake(struct tcp_handler_data* data, int fd, int writing);

/*
 * Handle incoming queries on a TLS over TCP connection.  The TLS
 * connections are configured to be non-blocking and the handler may
 * be called multiple times before a complete query is received.
 */
static void handle_tls_reading(int fd, short event, void* arg);

/*
 * Handle outgoing responses on a TLS over TCP connection.  The TLS
 * connections are configured to be non-blocking and the handler may
 * be called multiple times before a complete response is sent.
 */
static void handle_tls_writing(int fd, short event, void* arg);
#endif

/*
 * Send all children the quit nonblocking, then close pipe.
 */
static void send_children_quit(struct nsd* nsd);
/* same, for shutdown time, waits for child to exit to avoid restart issues */
static void send_children_quit_and_wait(struct nsd* nsd);

/* set childrens flags to send NSD_STATS to them */
#ifdef BIND8_STATS
static void set_children_stats(struct nsd* nsd);
#endif /* BIND8_STATS */

/*
 * Change the event types the HANDLERS are interested in to EVENT_TYPES.
 */
static void configure_handler_event_types(short event_types);

static uint16_t *compressed_dname_offsets = 0;
static uint32_t compression_table_capacity = 0;
static uint32_t compression_table_size = 0;
static domain_type* compressed_dnames[MAXRRSPP];

#ifdef USE_TCP_FASTOPEN
/* Checks to see if the kernel value must be manually changed in order for
   TCP Fast Open to support server mode */
static void report_tcp_fastopen_config() {

	int tcp_fastopen_fp;
	uint8_t tcp_fastopen_value;

	if ( (tcp_fastopen_fp = open(TCP_FASTOPEN_FILE, O_RDONLY)) == -1 ) {
		log_msg(LOG_INFO,"Error opening " TCP_FASTOPEN_FILE ": %s\n", strerror(errno));
	}
	if (read(tcp_fastopen_fp, &tcp_fastopen_value, 1) == -1 ) {
		log_msg(LOG_INFO,"Error reading " TCP_FASTOPEN_FILE ": %s\n", strerror(errno));
		close(tcp_fastopen_fp);
	}
	if (!(tcp_fastopen_value & TCP_FASTOPEN_SERVER_BIT_MASK)) {
		log_msg(LOG_WARNING, "Error: TCP Fast Open support is available and configured in NSD by default.\n");
		log_msg(LOG_WARNING, "However the kernel paramenters are not configured to support TCP_FASTOPEN in server mode.\n");
		log_msg(LOG_WARNING, "To enable TFO use the command:");
		log_msg(LOG_WARNING, "  'sudo sysctl -w net.ipv4.tcp_fastopen=2' for pure server mode or\n");
		log_msg(LOG_WARNING, "  'sudo sysctl -w net.ipv4.tcp_fastopen=3' for both client and server mode\n");
		log_msg(LOG_WARNING, "NSD will not have TCP Fast Open available until this change is made.\n");
		close(tcp_fastopen_fp);
	}
	close(tcp_fastopen_fp);
}
#endif

/*
 * Remove the specified pid from the list of child pids.  Returns -1 if
 * the pid is not in the list, child_num otherwise.  The field is set to 0.
 */
static int
delete_child_pid(struct nsd *nsd, pid_t pid)
{
	size_t i;
	for (i = 0; i < nsd->child_count; ++i) {
		if (nsd->children[i].pid == pid) {
			nsd->children[i].pid = 0;
			if(!nsd->children[i].need_to_exit) {
				if(nsd->children[i].child_fd != -1)
					close(nsd->children[i].child_fd);
				nsd->children[i].child_fd = -1;
				if(nsd->children[i].handler)
					nsd->children[i].handler->fd = -1;
			}
			return i;
		}
	}
	return -1;
}

/*
 * Restart child servers if necessary.
 */
static int
restart_child_servers(struct nsd *nsd, region_type* region, netio_type* netio,
	int* xfrd_sock_p)
{
	struct main_ipc_handler_data *ipc_data;
	size_t i;
	int sv[2];

	/* Fork the child processes... */
	for (i = 0; i < nsd->child_count; ++i) {
		if (nsd->children[i].pid <= 0) {
			if (nsd->children[i].child_fd != -1)
				close(nsd->children[i].child_fd);
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
				log_msg(LOG_ERR, "socketpair: %s",
					strerror(errno));
				return -1;
			}
			nsd->children[i].child_fd = sv[0];
			nsd->children[i].parent_fd = sv[1];
			nsd->children[i].pid = fork();
			switch (nsd->children[i].pid) {
			default: /* SERVER MAIN */
				close(nsd->children[i].parent_fd);
				nsd->children[i].parent_fd = -1;
				if (fcntl(nsd->children[i].child_fd, F_SETFL, O_NONBLOCK) == -1) {
					log_msg(LOG_ERR, "cannot fcntl pipe: %s", strerror(errno));
				}
				if(!nsd->children[i].handler)
				{
					ipc_data = (struct main_ipc_handler_data*) region_alloc(
						region, sizeof(struct main_ipc_handler_data));
					ipc_data->nsd = nsd;
					ipc_data->child = &nsd->children[i];
					ipc_data->child_num = i;
					ipc_data->xfrd_sock = xfrd_sock_p;
					ipc_data->packet = buffer_create(region, QIOBUFSZ);
					ipc_data->forward_mode = 0;
					ipc_data->got_bytes = 0;
					ipc_data->total_bytes = 0;
					ipc_data->acl_num = 0;
					nsd->children[i].handler = (struct netio_handler*) region_alloc(
						region, sizeof(struct netio_handler));
					nsd->children[i].handler->fd = nsd->children[i].child_fd;
					nsd->children[i].handler->timeout = NULL;
					nsd->children[i].handler->user_data = ipc_data;
					nsd->children[i].handler->event_types = NETIO_EVENT_READ;
					nsd->children[i].handler->event_handler = parent_handle_child_command;
					netio_add_handler(netio, nsd->children[i].handler);
				}
				/* clear any ongoing ipc */
				ipc_data = (struct main_ipc_handler_data*)
					nsd->children[i].handler->user_data;
				ipc_data->forward_mode = 0;
				/* restart - update fd */
				nsd->children[i].handler->fd = nsd->children[i].child_fd;
				break;
			case 0: /* CHILD */
				/* the child need not be able to access the
				 * nsd.db file */
				namedb_close_udb(nsd->db);
#ifdef MEMCLEAN /* OS collects memory pages */
				region_destroy(region);
#endif
				nsd->pid = 0;
				nsd->child_count = 0;
				nsd->server_kind = nsd->children[i].kind;
				nsd->this_child = &nsd->children[i];
				nsd->this_child->child_num = i;
				/* remove signal flags inherited from parent
				   the parent will handle them. */
				nsd->signal_hint_reload_hup = 0;
				nsd->signal_hint_reload = 0;
				nsd->signal_hint_child = 0;
				nsd->signal_hint_quit = 0;
				nsd->signal_hint_shutdown = 0;
				nsd->signal_hint_stats = 0;
				nsd->signal_hint_statsusr = 0;
				close(*xfrd_sock_p);
				close(nsd->this_child->child_fd);
				nsd->this_child->child_fd = -1;
				if (fcntl(nsd->this_child->parent_fd, F_SETFL, O_NONBLOCK) == -1) {
					log_msg(LOG_ERR, "cannot fcntl pipe: %s", strerror(errno));
				}
				server_child(nsd);
				/* NOTREACH */
				exit(0);
			case -1:
				log_msg(LOG_ERR, "fork failed: %s",
					strerror(errno));
				return -1;
			}
		}
	}
	return 0;
}

#ifdef BIND8_STATS
static void set_bind8_alarm(struct nsd* nsd)
{
	/* resync so that the next alarm is on the next whole minute */
	if(nsd->st.period > 0) /* % by 0 gives divbyzero error */
		alarm(nsd->st.period - (time(NULL) % nsd->st.period));
}
#endif

/* set zone stat ids for zones initially read in */
static void
zonestatid_tree_set(struct nsd* nsd)
{
	struct radnode* n;
	for(n=radix_first(nsd->db->zonetree); n; n=radix_next(n)) {
		zone_type* zone = (zone_type*)n->elem;
		zone->zonestatid = getzonestatid(nsd->options, zone->opts);
	}
}

#ifdef USE_ZONE_STATS
void
server_zonestat_alloc(struct nsd* nsd)
{
	size_t num = (nsd->options->zonestatnames->count==0?1:
			nsd->options->zonestatnames->count);
	size_t sz = sizeof(struct nsdst)*num;
	char tmpfile[256];
	uint8_t z = 0;

	/* file names */
	nsd->zonestatfname[0] = 0;
	nsd->zonestatfname[1] = 0;
	snprintf(tmpfile, sizeof(tmpfile), "%snsd-xfr-%d/nsd.%u.zstat.0",
		nsd->options->xfrdir, (int)getpid(), (unsigned)getpid());
	nsd->zonestatfname[0] = region_strdup(nsd->region, tmpfile);
	snprintf(tmpfile, sizeof(tmpfile), "%snsd-xfr-%d/nsd.%u.zstat.1",
		nsd->options->xfrdir, (int)getpid(), (unsigned)getpid());
	nsd->zonestatfname[1] = region_strdup(nsd->region, tmpfile);

	/* file descriptors */
	nsd->zonestatfd[0] = open(nsd->zonestatfname[0], O_CREAT|O_RDWR, 0600);
	if(nsd->zonestatfd[0] == -1) {
		log_msg(LOG_ERR, "cannot create %s: %s", nsd->zonestatfname[0],
			strerror(errno));
		exit(1);
	}
	nsd->zonestatfd[1] = open(nsd->zonestatfname[1], O_CREAT|O_RDWR, 0600);
	if(nsd->zonestatfd[0] == -1) {
		log_msg(LOG_ERR, "cannot create %s: %s", nsd->zonestatfname[1],
			strerror(errno));
		close(nsd->zonestatfd[0]);
		unlink(nsd->zonestatfname[0]);
		exit(1);
	}

#ifdef HAVE_MMAP
	if(lseek(nsd->zonestatfd[0], (off_t)sz-1, SEEK_SET) == -1) {
		log_msg(LOG_ERR, "lseek %s: %s", nsd->zonestatfname[0],
			strerror(errno));
		exit(1);
	}
	if(write(nsd->zonestatfd[0], &z, 1) == -1) {
		log_msg(LOG_ERR, "cannot extend stat file %s (%s)",
			nsd->zonestatfname[0], strerror(errno));
		exit(1);
	}
	if(lseek(nsd->zonestatfd[1], (off_t)sz-1, SEEK_SET) == -1) {
		log_msg(LOG_ERR, "lseek %s: %s", nsd->zonestatfname[1],
			strerror(errno));
		exit(1);
	}
	if(write(nsd->zonestatfd[1], &z, 1) == -1) {
		log_msg(LOG_ERR, "cannot extend stat file %s (%s)",
			nsd->zonestatfname[1], strerror(errno));
		exit(1);
	}
	nsd->zonestat[0] = (struct nsdst*)mmap(NULL, sz, PROT_READ|PROT_WRITE,
		MAP_SHARED, nsd->zonestatfd[0], 0);
	if(nsd->zonestat[0] == MAP_FAILED) {
		log_msg(LOG_ERR, "mmap failed: %s", strerror(errno));
		unlink(nsd->zonestatfname[0]);
		unlink(nsd->zonestatfname[1]);
		exit(1);
	}
	nsd->zonestat[1] = (struct nsdst*)mmap(NULL, sz, PROT_READ|PROT_WRITE,
		MAP_SHARED, nsd->zonestatfd[1], 0);
	if(nsd->zonestat[1] == MAP_FAILED) {
		log_msg(LOG_ERR, "mmap failed: %s", strerror(errno));
		unlink(nsd->zonestatfname[0]);
		unlink(nsd->zonestatfname[1]);
		exit(1);
	}
	memset(nsd->zonestat[0], 0, sz);
	memset(nsd->zonestat[1], 0, sz);
	nsd->zonestatsize[0] = num;
	nsd->zonestatsize[1] = num;
	nsd->zonestatdesired = num;
	nsd->zonestatsizenow = num;
	nsd->zonestatnow = nsd->zonestat[0];
#endif /* HAVE_MMAP */
}

void
zonestat_remap(struct nsd* nsd, int idx, size_t sz)
{
#ifdef HAVE_MMAP
#ifdef MREMAP_MAYMOVE
	nsd->zonestat[idx] = (struct nsdst*)mremap(nsd->zonestat[idx],
		sizeof(struct nsdst)*nsd->zonestatsize[idx], sz,
		MREMAP_MAYMOVE);
	if(nsd->zonestat[idx] == MAP_FAILED) {
		log_msg(LOG_ERR, "mremap failed: %s", strerror(errno));
		exit(1);
	}
#else /* !HAVE MREMAP */
	if(msync(nsd->zonestat[idx],
		sizeof(struct nsdst)*nsd->zonestatsize[idx], MS_ASYNC) != 0)
		log_msg(LOG_ERR, "msync failed: %s", strerror(errno));
	if(munmap(nsd->zonestat[idx],
		sizeof(struct nsdst)*nsd->zonestatsize[idx]) != 0)
		log_msg(LOG_ERR, "munmap failed: %s", strerror(errno));
	nsd->zonestat[idx] = (struct nsdst*)mmap(NULL, sz,
		PROT_READ|PROT_WRITE, MAP_SHARED, nsd->zonestatfd[idx], 0);
	if(nsd->zonestat[idx] == MAP_FAILED) {
		log_msg(LOG_ERR, "mmap failed: %s", strerror(errno));
		exit(1);
	}
#endif /* MREMAP */
#endif /* HAVE_MMAP */
}

/* realloc the zonestat array for the one that is not currently in use,
 * to match the desired new size of the array (if applicable) */
void
server_zonestat_realloc(struct nsd* nsd)
{
#ifdef HAVE_MMAP
	uint8_t z = 0;
	size_t sz;
	int idx = 0; /* index of the zonestat array that is not in use */
	if(nsd->zonestatnow == nsd->zonestat[0])
		idx = 1;
	if(nsd->zonestatsize[idx] == nsd->zonestatdesired)
		return;
	sz = sizeof(struct nsdst)*nsd->zonestatdesired;
	if(lseek(nsd->zonestatfd[idx], (off_t)sz-1, SEEK_SET) == -1) {
		log_msg(LOG_ERR, "lseek %s: %s", nsd->zonestatfname[idx],
			strerror(errno));
		exit(1);
	}
	if(write(nsd->zonestatfd[idx], &z, 1) == -1) {
		log_msg(LOG_ERR, "cannot extend stat file %s (%s)",
			nsd->zonestatfname[idx], strerror(errno));
		exit(1);
	}
	zonestat_remap(nsd, idx, sz);
	/* zero the newly allocated region */
	if(nsd->zonestatdesired > nsd->zonestatsize[idx]) {
		memset(((char*)nsd->zonestat[idx])+sizeof(struct nsdst) *
			nsd->zonestatsize[idx], 0, sizeof(struct nsdst) *
			(nsd->zonestatdesired - nsd->zonestatsize[idx]));
	}
	nsd->zonestatsize[idx] = nsd->zonestatdesired;
#endif /* HAVE_MMAP */
}

/* switchover to use the other array for the new children, that
 * briefly coexist with the old children.  And we want to avoid them
 * both writing to the same statistics arrays. */
void
server_zonestat_switch(struct nsd* nsd)
{
	if(nsd->zonestatnow == nsd->zonestat[0]) {
		nsd->zonestatnow = nsd->zonestat[1];
		nsd->zonestatsizenow = nsd->zonestatsize[1];
	} else {
		nsd->zonestatnow = nsd->zonestat[0];
		nsd->zonestatsizenow = nsd->zonestatsize[0];
	}
}
#endif /* USE_ZONE_STATS */

static void
cleanup_dname_compression_tables(void *ptr)
{
	free(ptr);
	compressed_dname_offsets = NULL;
	compression_table_capacity = 0;
}

static void
initialize_dname_compression_tables(struct nsd *nsd)
{
	size_t needed = domain_table_count(nsd->db->domains) + 1;
	needed += EXTRA_DOMAIN_NUMBERS;
	if(compression_table_capacity < needed) {
		if(compressed_dname_offsets) {
			region_remove_cleanup(nsd->db->region,
				cleanup_dname_compression_tables,
				compressed_dname_offsets);
			free(compressed_dname_offsets);
		}
		compressed_dname_offsets = (uint16_t *) xmallocarray(
			needed, sizeof(uint16_t));
		region_add_cleanup(nsd->db->region, cleanup_dname_compression_tables,
			compressed_dname_offsets);
		compression_table_capacity = needed;
		compression_table_size=domain_table_count(nsd->db->domains)+1;
	}
	memset(compressed_dname_offsets, 0, needed * sizeof(uint16_t));
	compressed_dname_offsets[0] = QHEADERSZ; /* The original query name */
}

/* create and bind sockets.  */
static int
server_init_ifs(struct nsd *nsd, size_t from, size_t to, int* reuseport_works)
{
	struct addrinfo* addr;
	size_t i;
#if defined(SO_REUSEPORT) || defined(SO_REUSEADDR) || (defined(INET6) && (defined(IPV6_V6ONLY) || defined(IPV6_USE_MIN_MTU) || defined(IPV6_MTU) || defined(IP_TRANSPARENT)) || defined(IP_FREEBIND) || defined(SO_BINDANY))
	int on = 1;
#endif
#ifdef USE_TCP_FASTOPEN
	int qlen;
#endif

	/* UDP */

	/* Make a socket... */
	for (i = from; i < to; i++) {
		/* for reuseports copy socket specs of first entries */
		addr = nsd->udp[i%nsd->ifs].addr;
		if (!addr) {
			nsd->udp[i].s = -1;
			continue;
		}
		nsd->udp[i].fam = (int)addr->ai_family;
		if ((nsd->udp[i].s = socket(addr->ai_family, addr->ai_socktype, 0)) == -1) {
#if defined(INET6)
			if (addr->ai_family == AF_INET6 &&
				errno == EAFNOSUPPORT && nsd->grab_ip6_optional) {
				log_msg(LOG_WARNING, "fallback to UDP4, no IPv6: not supported");
				continue;
			}
#endif /* INET6 */
			log_msg(LOG_ERR, "can't create a socket: %s", strerror(errno));
			return -1;
		}

#ifdef SO_REUSEPORT
#  ifdef SO_REUSEPORT_LB
		/* on FreeBSD 12 we have SO_REUSEPORT_LB that does loadbalance
		 * like SO_REUSEPORT on Linux.  This is what the users want
		 * with the config option in nsd.conf; if we actually
		 * need local address and port reuse they'll also need to
		 * have SO_REUSEPORT set for them, assume it was _LB they want.
		 */
		if(nsd->reuseport && *reuseport_works &&
			setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_REUSEPORT_LB,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			if(verbosity >= 3
#ifdef ENOPROTOOPT
				|| errno != ENOPROTOOPT
#endif
				)
			    log_msg(LOG_ERR, "setsockopt(..., SO_REUSEPORT_LB, "
				"...) failed: %s", strerror(errno));
			*reuseport_works = 0;
		}
#  else /* SO_REUSEPORT_LB */
		if(nsd->reuseport && *reuseport_works &&
			setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_REUSEPORT,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			if(verbosity >= 3
#ifdef ENOPROTOOPT
				|| errno != ENOPROTOOPT
#endif
				)
			    log_msg(LOG_ERR, "setsockopt(..., SO_REUSEPORT, "
				"...) failed: %s", strerror(errno));
			*reuseport_works = 0;
		}
#  endif /* SO_REUSEPORT_LB */
#else
		(void)reuseport_works;
#endif /* SO_REUSEPORT */
#if defined(SO_RCVBUF)
	{
		int rcv = 1*1024*1024;
		if (nsd->options->receive_buffer_size > 0) {
			rcv = nsd->options->receive_buffer_size;
		}
#  ifdef SO_RCVBUFFORCE
		if(setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_RCVBUFFORCE, (void*)&rcv,
			(socklen_t)sizeof(rcv)) < 0) {
			if(errno != EPERM && errno != ENOBUFS) {
				log_msg(LOG_ERR, "setsockopt(..., SO_RCVBUFFORCE, "
                                        "...) failed: %s", strerror(errno));
				return -1;
			}
		}
#  else
		if(setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_RCVBUF, (void*)&rcv,
			(socklen_t)sizeof(rcv)) < 0) {
			if(errno != ENOBUFS && errno != ENOSYS) {
				log_msg(LOG_ERR, "setsockopt(..., SO_RCVBUF, "
                                        "...) failed: %s", strerror(errno));
				return -1;
			}
		}
#  endif /* SO_RCVBUFFORCE */
	}
#endif /* SO_RCVBUF */

#ifdef SO_SNDBUF
	{
		int snd = 1*1024*1024;
		if (nsd->options->send_buffer_size > 0) {
			snd = nsd->options->send_buffer_size;
		}
#  ifdef SO_SNDBUFFORCE
		if(setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_SNDBUFFORCE, (void*)&snd,
			(socklen_t)sizeof(snd)) < 0) {
			if(errno != EPERM && errno != ENOBUFS) {
				log_msg(LOG_ERR, "setsockopt(..., SO_SNDBUFFORCE, "
                                        "...) failed: %s", strerror(errno));
				return -1;
			}
		}
#  else
		if(setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_SNDBUF, (void*)&snd,
			(socklen_t)sizeof(snd)) < 0) {
			if(errno != ENOBUFS && errno != ENOSYS) {
				log_msg(LOG_ERR, "setsockopt(..., SO_SNDBUF, "
                                        "...) failed: %s", strerror(errno));
				return -1;
			}
		}
#  endif /* SO_SNDBUFFFORCE */
	}
#endif /* SO_SNDBUF */

#if defined(INET6)
		if (addr->ai_family == AF_INET6) {
# if defined(IPV6_V6ONLY)
			if (setsockopt(nsd->udp[i].s,
				       IPPROTO_IPV6, IPV6_V6ONLY,
				       &on, sizeof(on)) < 0)
			{
				log_msg(LOG_ERR, "setsockopt(..., IPV6_V6ONLY, ...) failed: %s",
					strerror(errno));
				return -1;
			}
# endif
# if defined(IPV6_USE_MIN_MTU)
			/*
			 * There is no fragmentation of IPv6 datagrams
			 * during forwarding in the network. Therefore
			 * we do not send UDP datagrams larger than
			 * the minimum IPv6 MTU of 1280 octets. The
			 * EDNS0 message length can be larger if the
			 * network stack supports IPV6_USE_MIN_MTU.
			 */
			if (setsockopt(nsd->udp[i].s,
				       IPPROTO_IPV6, IPV6_USE_MIN_MTU,
				       &on, sizeof(on)) < 0)
			{
				log_msg(LOG_ERR, "setsockopt(..., IPV6_USE_MIN_MTU, ...) failed: %s",
					strerror(errno));
				return -1;
			}
# elif defined(IPV6_MTU)
			/*
			 * On Linux, PMTUD is disabled by default for datagrams
			 * so set the MTU equal to the MIN MTU to get the same.
			 */
			on = IPV6_MIN_MTU;
			if (setsockopt(nsd->udp[i].s, IPPROTO_IPV6, IPV6_MTU, 
				&on, sizeof(on)) < 0)
			{
				log_msg(LOG_ERR, "setsockopt(..., IPV6_MTU, ...) failed: %s",
					strerror(errno));
				return -1;
			}
			on = 1;
# endif
		}
#endif
#if defined(AF_INET)
		if (addr->ai_family == AF_INET) {
#  if defined(IP_MTU_DISCOVER)
			int mtudisc_disabled = 0;
#   if defined(IP_PMTUDISC_OMIT)
		/* Try IP_PMTUDISC_OMIT first */

		/*
		 * Linux 3.15 has IP_PMTUDISC_OMIT which makes sockets
		 * ignore PMTU information and send packets with DF=0.
		 * Fragmentation is allowed if and only if the packet
		 * size exceeds the outgoing interface MTU or the packet
		 * encounters smaller MTU link in network.
		 * This mitigates DNS fragmentation attacks by preventing
		 * forged PMTU information.
		 * FreeBSD already has same semantics without setting
		 * the option.
		 */
			int action_omit = IP_PMTUDISC_OMIT;
			if (!mtudisc_disabled) {
				if(setsockopt(nsd->udp[i].s, IPPROTO_IP,
					IP_MTU_DISCOVER, &action_omit,
					sizeof(action_omit)) < 0)
				{
					log_msg(LOG_ERR, "setsockopt(..., IP_MTU_DISCOVER, IP_PMTUDISC_OMIT...) failed: %s",
						strerror(errno));
				} else {
					mtudisc_disabled = 1;
				}
			}	
#   endif /* IP_PMTUDISC_OMIT */
#   if defined(IP_PMTUDISC_DONT)
			/* 
			 * Use IP_PMTUDISC_DONT
			 * if IP_PMTUDISC_OMIT failed / undefined
			 */
			if (!mtudisc_disabled) {
				int action_dont = IP_PMTUDISC_DONT;
				if (setsockopt(nsd->udp[i].s, IPPROTO_IP, 
					IP_MTU_DISCOVER, &action_dont,
					sizeof(action_dont)) < 0)
				{
					log_msg(LOG_ERR, "setsockopt(..., IP_MTU_DISCOVER, IP_PMTUDISC_DONT...) failed: %s",
						strerror(errno));
				} else {
					mtudisc_disabled = 1;
				}
			}
#   endif /* IP_PMTUDISC_DONT */
			/* exit if all methods to disable PMTUD failed */
			if(!mtudisc_disabled) {
				return -1;
			}
#  elif defined(IP_DONTFRAG)
			int off = 0;
			if (setsockopt(nsd->udp[i].s, IPPROTO_IP, IP_DONTFRAG,
				&off, sizeof(off)) < 0)
			{
				log_msg(LOG_ERR, "setsockopt(..., IP_DONTFRAG, ...) failed: %s",
					strerror(errno));
				return -1;
			}
#  endif
		}
#endif
		/* set it nonblocking */
		/* otherwise, on OSes with thundering herd problems, the
		   UDP recv could block NSD after select returns readable. */
		if (fcntl(nsd->udp[i].s, F_SETFL, O_NONBLOCK) == -1) {
			log_msg(LOG_ERR, "cannot fcntl udp: %s", strerror(errno));
		}

		/* Bind it... */
		if (nsd->options->ip_freebind) {
#ifdef IP_FREEBIND
			if (setsockopt(nsd->udp[i].s, IPPROTO_IP, IP_FREEBIND, &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(...,IP_FREEBIND, ...) failed for udp: %s",
					strerror(errno));
			}
#endif /* IP_FREEBIND */
		}

		if (nsd->options->ip_transparent) {
#ifdef IP_TRANSPARENT
			if (setsockopt(nsd->udp[i].s, IPPROTO_IP, IP_TRANSPARENT, &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(...,IP_TRANSPARENT, ...) failed for udp: %s",
					strerror(errno));
			}
#endif /* IP_TRANSPARENT */
#ifdef SO_BINDANY
			if (setsockopt(nsd->udp[i].s, SOL_SOCKET, SO_BINDANY, &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(...,SO_BINDANY, ...) failed for udp: %s",
					strerror(errno));
			}
#endif /* SO_BINDANY */
		}

		if (
			bind(nsd->udp[i].s, (struct sockaddr *) addr->ai_addr, addr->ai_addrlen) != 0) {
			char buf[256];
			addrport2str((void*)addr->ai_addr, buf, sizeof(buf));
			log_msg(LOG_ERR, "can't bind udp socket %s: %s", buf, strerror(errno));
			return -1;
		}
	}

	/* TCP */

#ifdef USE_TCP_FASTOPEN
	report_tcp_fastopen_config();
#endif

	/* Make a socket... */
	for (i = from; i < to; i++) {
		/* for reuseports copy socket specs of first entries */
		addr = nsd->tcp[i%nsd->ifs].addr;
		if (!addr) {
			nsd->tcp[i].s = -1;
			continue;
		}
		nsd->tcp[i].fam = (int)addr->ai_family;
		/* turn off REUSEPORT for TCP by copying the socket fd */
		if(i >= nsd->ifs) {
			nsd->tcp[i].s = nsd->tcp[i%nsd->ifs].s;
			continue;
		}
		if ((nsd->tcp[i].s = socket(addr->ai_family, addr->ai_socktype, 0)) == -1) {
#if defined(INET6)
			if (addr->ai_family == AF_INET6 &&
				errno == EAFNOSUPPORT && nsd->grab_ip6_optional) {
				log_msg(LOG_WARNING, "fallback to TCP4, no IPv6: not supported");
				continue;
			}
#endif /* INET6 */
			log_msg(LOG_ERR, "can't create a socket: %s", strerror(errno));
			return -1;
		}

#ifdef SO_REUSEPORT
		if(nsd->reuseport && *reuseport_works &&
			setsockopt(nsd->tcp[i].s, SOL_SOCKET, SO_REUSEPORT,
			(void*)&on, (socklen_t)sizeof(on)) < 0) {
			if(verbosity >= 3
#ifdef ENOPROTOOPT
				|| errno != ENOPROTOOPT
#endif
				)
			    log_msg(LOG_ERR, "setsockopt(..., SO_REUSEPORT, "
				"...) failed: %s", strerror(errno));
			*reuseport_works = 0;
		}
#endif /* SO_REUSEPORT */
#ifdef	SO_REUSEADDR
		if (setsockopt(nsd->tcp[i].s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
			log_msg(LOG_ERR, "setsockopt(..., SO_REUSEADDR, ...) failed: %s", strerror(errno));
		}
#endif /* SO_REUSEADDR */

#if defined(INET6)
		if (addr->ai_family == AF_INET6) {
# if defined(IPV6_V6ONLY)
			if (setsockopt(nsd->tcp[i].s, IPPROTO_IPV6, IPV6_V6ONLY,
				&on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(..., IPV6_V6ONLY, ...) failed: %s", strerror(errno));
				return -1;
			}
# endif
# if defined(IPV6_USE_MIN_MTU)
			/*
			 * Use minimum MTU to minimize delays learning working
			 * PMTU when communicating through a tunnel.
			 */
			if (setsockopt(nsd->tcp[i].s,
				       IPPROTO_IPV6, IPV6_USE_MIN_MTU,
				       &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(..., IPV6_USE_MIN_MTU, ...) failed: %s", strerror(errno));
				return -1;
			}
# elif defined(IPV6_MTU)
			/*
			 * On Linux, PMTUD is disabled by default for datagrams
			 * so set the MTU equal to the MIN MTU to get the same.
			 */
			on = IPV6_MIN_MTU;
			if (setsockopt(nsd->tcp[i].s, IPPROTO_IPV6, IPV6_MTU,
				&on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(..., IPV6_MTU, ...) failed: %s", strerror(errno));
				return -1;
			}
			on = 1;
# endif
		}
#endif
		/* set maximum segment size to tcp socket */
		if(nsd->tcp_mss > 0) {
#if defined(IPPROTO_TCP) && defined(TCP_MAXSEG)
			if(setsockopt(nsd->tcp[i].s, IPPROTO_TCP, TCP_MAXSEG,
					(void*)&nsd->tcp_mss,
					sizeof(nsd->tcp_mss)) < 0) {
				log_msg(LOG_ERR,
					"setsockopt(...,TCP_MAXSEG,...)"
					" failed for tcp: %s", strerror(errno));
			}
#else
			log_msg(LOG_ERR, "setsockopt(TCP_MAXSEG) unsupported");
#endif /* defined(IPPROTO_TCP) && defined(TCP_MAXSEG) */
		}

		/* set it nonblocking */
		/* (StevensUNP p463), if tcp listening socket is blocking, then
		   it may block in accept, even if select() says readable. */
		if (fcntl(nsd->tcp[i].s, F_SETFL, O_NONBLOCK) == -1) {
			log_msg(LOG_ERR, "cannot fcntl tcp: %s", strerror(errno));
		}

		/* Bind it... */
		if (nsd->options->ip_freebind) {
#ifdef IP_FREEBIND
			if (setsockopt(nsd->tcp[i].s, IPPROTO_IP, IP_FREEBIND, &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(...,IP_FREEBIND, ...) failed for tcp: %s",
					strerror(errno));
			}
#endif /* IP_FREEBIND */
		}

		if (nsd->options->ip_transparent) {
#ifdef IP_TRANSPARENT
			if (setsockopt(nsd->tcp[i].s, IPPROTO_IP, IP_TRANSPARENT, &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(...,IP_TRANSPARENT, ...) failed for tcp: %s",
					strerror(errno));
			}
#endif /* IP_TRANSPARENT */
#ifdef SO_BINDANY
			if (setsockopt(nsd->tcp[i].s, SOL_SOCKET, SO_BINDANY, &on, sizeof(on)) < 0) {
				log_msg(LOG_ERR, "setsockopt(...,SO_BINDANY, ...) failed for tcp: %s",
					strerror(errno));
			}
#endif /* SO_BINDANY */
		}

		if(
			bind(nsd->tcp[i].s, (struct sockaddr *) addr->ai_addr, addr->ai_addrlen) != 0) {
			char buf[256];
			addrport2str((void*)addr->ai_addr, buf, sizeof(buf));
			log_msg(LOG_ERR, "can't bind tcp socket %s: %s", buf, strerror(errno));
			return -1;
		}

#ifdef USE_TCP_FASTOPEN
		/* qlen specifies how many outstanding TFO requests to allow. Limit is a defense
		   against IP spoofing attacks as suggested in RFC7413 */
#ifdef __APPLE__
		/* OS X implementation only supports qlen of 1 via this call. Actual
		   value is configured by the net.inet.tcp.fastopen_backlog kernel parm. */
		qlen = 1;
#else
		/* 5 is recommended on linux */
		qlen = 5;
#endif
		if ((setsockopt(nsd->tcp[i].s, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen))) == -1 ) {
#ifdef ENOPROTOOPT
                /* squelch ENOPROTOOPT: freebsd server mode with kernel support
                   disabled, except when verbosity enabled for debugging */
                    if(errno != ENOPROTOOPT || verbosity >= 3) {
#endif
			if(errno == EPERM) {
				log_msg(LOG_ERR, "Setting TCP Fast Open as server failed: %s ; this could likely be because sysctl net.inet.tcp.fastopen.enabled, net.inet.tcp.fastopen.server_enable, or net.ipv4.tcp_fastopen is disabled", strerror(errno));
			} else {
				log_msg(LOG_ERR, "Setting TCP Fast Open as server failed: %s", strerror(errno));
			}
#ifdef ENOPROTOOPT
		    }
#endif
		}
#endif

		/* Listen to it... */
		if (listen(nsd->tcp[i].s, TCP_BACKLOG) == -1) {
			log_msg(LOG_ERR, "can't listen: %s", strerror(errno));
			return -1;
		}
	}

	return 0;
}

/*
 * Initialize the server, reuseport, create and bind the sockets.
 */
int
server_init(struct nsd *nsd)
{
	int reuseport_successful = 1; /* see if reuseport works in OS */
	if(nsd->reuseport) {
		/* increase the size of the udp and tcp interface arrays,
		 * there are going to be separate interface file descriptors
		 * for every server instance */
		nsd->udp = xrealloc(nsd->udp, (nsd->ifs*nsd->reuseport)*
			sizeof(*nsd->udp));
		nsd->tcp = xrealloc(nsd->tcp, (nsd->ifs*nsd->reuseport)*
			sizeof(*nsd->tcp));
		memset(&nsd->udp[nsd->ifs], 0, sizeof(*nsd->udp)*
			(nsd->ifs*(nsd->reuseport-1)));
		memset(&nsd->tcp[nsd->ifs], 0, sizeof(*nsd->tcp)*
			(nsd->ifs*(nsd->reuseport-1)));
	}

	/* open the server interface ports */
	if(server_init_ifs(nsd, 0, nsd->ifs, &reuseport_successful) == -1)
		return -1;

	/* continue to open the remaining reuseport ports */
	if(nsd->reuseport && reuseport_successful) {
		if(server_init_ifs(nsd, nsd->ifs, nsd->ifs*nsd->reuseport,
			&reuseport_successful) == -1)
			return -1;
		nsd->ifs *= nsd->reuseport;
	} else {
		nsd->reuseport = 0;
	}
	return 0;
}

/*
 * Prepare the server for take off.
 *
 */
int
server_prepare(struct nsd *nsd)
{
#ifdef RATELIMIT
	/* set secret modifier for hashing (udb ptr buckets and rate limits) */
#ifdef HAVE_ARC4RANDOM
	hash_set_raninit(arc4random());
#else
	uint32_t v = getpid() ^ time(NULL);
	srandom((unsigned long)v);
	if(RAND_status() && RAND_bytes((unsigned char*)&v, sizeof(v)) > 0)
		hash_set_raninit(v);
	else	hash_set_raninit(random());
#endif
	rrl_mmap_init(nsd->child_count, nsd->options->rrl_size,
		nsd->options->rrl_ratelimit,
		nsd->options->rrl_whitelist_ratelimit,
		nsd->options->rrl_slip,
		nsd->options->rrl_ipv4_prefix_length,
		nsd->options->rrl_ipv6_prefix_length);
#endif /* RATELIMIT */

	/* Open the database... */
	if ((nsd->db = namedb_open(nsd->dbfile, nsd->options)) == NULL) {
		log_msg(LOG_ERR, "unable to open the database %s: %s",
			nsd->dbfile, strerror(errno));
		unlink(nsd->task[0]->fname);
		unlink(nsd->task[1]->fname);
#ifdef USE_ZONE_STATS
		unlink(nsd->zonestatfname[0]);
		unlink(nsd->zonestatfname[1]);
#endif
		xfrd_del_tempdir(nsd);
		return -1;
	}
	/* check if zone files have been modified */
	/* NULL for taskudb because we send soainfo in a moment, batched up,
	 * for all zones */
	if(nsd->options->zonefiles_check || (nsd->options->database == NULL ||
		nsd->options->database[0] == 0))
		namedb_check_zonefiles(nsd, nsd->options, NULL, NULL);
	zonestatid_tree_set(nsd);

	compression_table_capacity = 0;
	initialize_dname_compression_tables(nsd);

#ifdef	BIND8_STATS
	/* Initialize times... */
	time(&nsd->st.boot);
	set_bind8_alarm(nsd);
#endif /* BIND8_STATS */

	return 0;
}

/*
 * Fork the required number of servers.
 */
static int
server_start_children(struct nsd *nsd, region_type* region, netio_type* netio,
	int* xfrd_sock_p)
{
	size_t i;

	/* Start all child servers initially.  */
	for (i = 0; i < nsd->child_count; ++i) {
		nsd->children[i].pid = 0;
	}

	return restart_child_servers(nsd, region, netio, xfrd_sock_p);
}

void
server_close_all_sockets(struct nsd_socket sockets[], size_t n)
{
	size_t i;

	/* Close all the sockets... */
	for (i = 0; i < n; ++i) {
		if (sockets[i].s != -1) {
			close(sockets[i].s);
			if(sockets[i].addr)
				freeaddrinfo(sockets[i].addr);
			sockets[i].s = -1;
		}
	}
}

/*
 * Close the sockets, shutdown the server and exit.
 * Does not return.
 *
 */
void
server_shutdown(struct nsd *nsd)
{
	size_t i;

	server_close_all_sockets(nsd->udp, nsd->ifs);
	server_close_all_sockets(nsd->tcp, nsd->ifs);
	/* CHILD: close command channel to parent */
	if(nsd->this_child && nsd->this_child->parent_fd != -1)
	{
		close(nsd->this_child->parent_fd);
		nsd->this_child->parent_fd = -1;
	}
	/* SERVER: close command channels to children */
	if(!nsd->this_child)
	{
		for(i=0; i < nsd->child_count; ++i)
			if(nsd->children[i].child_fd != -1)
			{
				close(nsd->children[i].child_fd);
				nsd->children[i].child_fd = -1;
			}
	}

	tsig_finalize();
#ifdef HAVE_SSL
	daemon_remote_delete(nsd->rc); /* ssl-delete secret keys */
	if (nsd->tls_ctx)
		SSL_CTX_free(nsd->tls_ctx);
#endif

#ifdef MEMCLEAN /* OS collects memory pages */
#ifdef RATELIMIT
	rrl_mmap_deinit_keep_mmap();
#endif
#ifdef USE_DNSTAP
	dt_collector_destroy(nsd->dt_collector, nsd);
#endif
	udb_base_free_keep_mmap(nsd->task[0]);
	udb_base_free_keep_mmap(nsd->task[1]);
	namedb_close_udb(nsd->db); /* keeps mmap */
	namedb_close(nsd->db);
	nsd_options_destroy(nsd->options);
	region_destroy(nsd->region);
#endif
	log_finalize();
	exit(0);
}

void
server_prepare_xfrd(struct nsd* nsd)
{
	char tmpfile[256];
	/* create task mmaps */
	nsd->mytask = 0;
	snprintf(tmpfile, sizeof(tmpfile), "%snsd-xfr-%d/nsd.%u.task.0",
		nsd->options->xfrdir, (int)getpid(), (unsigned)getpid());
	nsd->task[0] = task_file_create(tmpfile);
	if(!nsd->task[0]) {
#ifdef USE_ZONE_STATS
		unlink(nsd->zonestatfname[0]);
		unlink(nsd->zonestatfname[1]);
#endif
		xfrd_del_tempdir(nsd);
		exit(1);
	}
	snprintf(tmpfile, sizeof(tmpfile), "%snsd-xfr-%d/nsd.%u.task.1",
		nsd->options->xfrdir, (int)getpid(), (unsigned)getpid());
	nsd->task[1] = task_file_create(tmpfile);
	if(!nsd->task[1]) {
		unlink(nsd->task[0]->fname);
#ifdef USE_ZONE_STATS
		unlink(nsd->zonestatfname[0]);
		unlink(nsd->zonestatfname[1]);
#endif
		xfrd_del_tempdir(nsd);
		exit(1);
	}
	assert(udb_base_get_userdata(nsd->task[0])->data == 0);
	assert(udb_base_get_userdata(nsd->task[1])->data == 0);
	/* create xfrd listener structure */
	nsd->xfrd_listener = region_alloc(nsd->region,
		sizeof(netio_handler_type));
	nsd->xfrd_listener->user_data = (struct ipc_handler_conn_data*)
		region_alloc(nsd->region, sizeof(struct ipc_handler_conn_data));
	nsd->xfrd_listener->fd = -1;
	((struct ipc_handler_conn_data*)nsd->xfrd_listener->user_data)->nsd =
		nsd;
	((struct ipc_handler_conn_data*)nsd->xfrd_listener->user_data)->conn =
		xfrd_tcp_create(nsd->region, QIOBUFSZ);
}


void
server_start_xfrd(struct nsd *nsd, int del_db, int reload_active)
{
	pid_t pid;
	int sockets[2] = {0,0};
	struct ipc_handler_conn_data *data;

	if(nsd->xfrd_listener->fd != -1)
		close(nsd->xfrd_listener->fd);
	if(del_db) {
		/* recreate taskdb that xfrd was using, it may be corrupt */
		/* we (or reload) use nsd->mytask, and xfrd uses the other */
		char* tmpfile = nsd->task[1-nsd->mytask]->fname;
		nsd->task[1-nsd->mytask]->fname = NULL;
		/* free alloc already, so udb does not shrink itself */
		udb_alloc_delete(nsd->task[1-nsd->mytask]->alloc);
		nsd->task[1-nsd->mytask]->alloc = NULL;
		udb_base_free(nsd->task[1-nsd->mytask]);
		/* create new file, overwrite the old one */
		nsd->task[1-nsd->mytask] = task_file_create(tmpfile);
		free(tmpfile);
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {
		log_msg(LOG_ERR, "startxfrd failed on socketpair: %s", strerror(errno));
		return;
	}
	pid = fork();
	switch (pid) {
	case -1:
		log_msg(LOG_ERR, "fork xfrd failed: %s", strerror(errno));
		break;
	default:
		/* PARENT: close first socket, use second one */
		close(sockets[0]);
		if (fcntl(sockets[1], F_SETFL, O_NONBLOCK) == -1) {
			log_msg(LOG_ERR, "cannot fcntl pipe: %s", strerror(errno));
		}
		if(del_db) xfrd_free_namedb(nsd);
		/* use other task than I am using, since if xfrd died and is
		 * restarted, the reload is using nsd->mytask */
		nsd->mytask = 1 - nsd->mytask;
		xfrd_init(sockets[1], nsd, del_db, reload_active, pid);
		/* ENOTREACH */
		break;
	case 0:
		/* CHILD: close second socket, use first one */
		close(sockets[1]);
		if (fcntl(sockets[0], F_SETFL, O_NONBLOCK) == -1) {
			log_msg(LOG_ERR, "cannot fcntl pipe: %s", strerror(errno));
		}
		nsd->xfrd_listener->fd = sockets[0];
		break;
	}
	/* server-parent only */
	nsd->xfrd_listener->timeout = NULL;
	nsd->xfrd_listener->event_types = NETIO_EVENT_READ;
	nsd->xfrd_listener->event_handler = parent_handle_xfrd_command;
	/* clear ongoing ipc reads */
	data = (struct ipc_handler_conn_data *) nsd->xfrd_listener->user_data;
	data->conn->is_reading = 0;
}

/** add all soainfo to taskdb */
static void
add_all_soa_to_task(struct nsd* nsd, struct udb_base* taskudb)
{
	struct radnode* n;
	udb_ptr task_last; /* last task, mytask is empty so NULL */
	/* add all SOA INFO to mytask */
	udb_ptr_init(&task_last, taskudb);
	for(n=radix_first(nsd->db->zonetree); n; n=radix_next(n)) {
		task_new_soainfo(taskudb, &task_last, (zone_type*)n->elem, 0);
	}
	udb_ptr_unlink(&task_last, taskudb);
}

void
server_send_soa_xfrd(struct nsd* nsd, int shortsoa)
{
	/* normally this exchanges the SOA from nsd->xfrd and the expire back.
	 *   parent fills one taskdb with soas, xfrd fills other with expires.
	 *   then they exchange and process.
	 * shortsoa: xfrd crashes and needs to be restarted and one taskdb
	 *   may be in use by reload.  Fill SOA in taskdb and give to xfrd.
	 *   expire notifications can be sent back via a normal reload later
	 *   (xfrd will wait for current running reload to finish if any).
	 */
	sig_atomic_t cmd = 0;
	pid_t mypid;
	int xfrd_sock = nsd->xfrd_listener->fd;
	struct udb_base* taskudb = nsd->task[nsd->mytask];
	udb_ptr t;
	if(!shortsoa) {
		if(nsd->signal_hint_shutdown) {
		shutdown:
			log_msg(LOG_WARNING, "signal received, shutting down...");
			server_close_all_sockets(nsd->udp, nsd->ifs);
			server_close_all_sockets(nsd->tcp, nsd->ifs);
#ifdef HAVE_SSL
			daemon_remote_close(nsd->rc);
#endif
			/* Unlink it if possible... */
			unlinkpid(nsd->pidfile);
			unlink(nsd->task[0]->fname);
			unlink(nsd->task[1]->fname);
#ifdef USE_ZONE_STATS
			unlink(nsd->zonestatfname[0]);
			unlink(nsd->zonestatfname[1]);
#endif
			/* write the nsd.db to disk, wait for it to complete */
			udb_base_sync(nsd->db->udb, 1);
			udb_base_close(nsd->db->udb);
			server_shutdown(nsd);
			exit(0);
		}
	}
	if(shortsoa) {
		/* put SOA in xfrd task because mytask may be in use */
		taskudb = nsd->task[1-nsd->mytask];
	}

	add_all_soa_to_task(nsd, taskudb);
	if(!shortsoa) {
		/* wait for xfrd to signal task is ready, RELOAD signal */
		if(block_read(nsd, xfrd_sock, &cmd, sizeof(cmd), -1) != sizeof(cmd) ||
			cmd != NSD_RELOAD) {
			log_msg(LOG_ERR, "did not get start signal from xfrd");
			exit(1);
		} 
		if(nsd->signal_hint_shutdown) {
			goto shutdown;
		}
	}
	/* give xfrd our task, signal it with RELOAD_DONE */
	task_process_sync(taskudb);
	cmd = NSD_RELOAD_DONE;
	if(!write_socket(xfrd_sock, &cmd,  sizeof(cmd))) {
		log_msg(LOG_ERR, "problems sending soa end from reload %d to xfrd: %s",
			(int)nsd->pid, strerror(errno));
	}
	mypid = getpid();
	if(!write_socket(nsd->xfrd_listener->fd, &mypid,  sizeof(mypid))) {
		log_msg(LOG_ERR, "problems sending reloadpid to xfrd: %s",
			strerror(errno));
	}

	if(!shortsoa) {
		/* process the xfrd task works (expiry data) */
		nsd->mytask = 1 - nsd->mytask;
		taskudb = nsd->task[nsd->mytask];
		task_remap(taskudb);
		udb_ptr_new(&t, taskudb, udb_base_get_userdata(taskudb));
		while(!udb_ptr_is_null(&t)) {
			task_process_expire(nsd->db, TASKLIST(&t));
			udb_ptr_set_rptr(&t, taskudb, &TASKLIST(&t)->next);
		}
		udb_ptr_unlink(&t, taskudb);
		task_clear(taskudb);

		/* tell xfrd that the task is emptied, signal with RELOAD_DONE */
		cmd = NSD_RELOAD_DONE;
		if(!write_socket(xfrd_sock, &cmd,  sizeof(cmd))) {
			log_msg(LOG_ERR, "problems sending soa end from reload %d to xfrd: %s",
				(int)nsd->pid, strerror(errno));
		}
	}
}

#ifdef HAVE_SSL
void
log_crypto_err(const char* str)
{
	/* error:[error code]:[library name]:[function name]:[reason string] */
	char buf[128];
	unsigned long e;
	ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
	log_msg(LOG_ERR, "%s crypto %s", str, buf);
	while( (e=ERR_get_error()) ) {
		ERR_error_string_n(e, buf, sizeof(buf));
		log_msg(LOG_ERR, "and additionally crypto %s", buf);
	}
}

void
perform_openssl_init(void)
{
	/* init SSL library */
#ifdef HAVE_ERR_LOAD_CRYPTO_STRINGS
	ERR_load_crypto_strings();
#endif
	ERR_load_SSL_strings();
#if OPENSSL_VERSION_NUMBER < 0x10100000 || !defined(HAVE_OPENSSL_INIT_CRYPTO)
	OpenSSL_add_all_algorithms();
#else
	OPENSSL_init_crypto(OPENSSL_INIT_ADD_ALL_CIPHERS
		| OPENSSL_INIT_ADD_ALL_DIGESTS
		| OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
#endif
#if OPENSSL_VERSION_NUMBER < 0x10100000 || !defined(HAVE_OPENSSL_INIT_SSL)
	(void)SSL_library_init();
#else
	OPENSSL_init_ssl(0, NULL);
#endif

	if(!RAND_status()) {
		/* try to seed it */
		unsigned char buf[256];
		unsigned int v, seed=(unsigned)time(NULL) ^ (unsigned)getpid();
		size_t i;
		v = seed;
		for(i=0; i<256/sizeof(v); i++) {
			memmove(buf+i*sizeof(v), &v, sizeof(v));
			v = v*seed + (unsigned int)i;
		}
		RAND_seed(buf, 256);
		log_msg(LOG_WARNING, "warning: no entropy, seeding openssl PRNG with time");
	}
}

static int
get_ocsp(char *filename, unsigned char **ocsp)
{
	BIO *bio;
	OCSP_RESPONSE *response;
	int len = -1;
	unsigned char *p, *buf;
	assert(filename);

	if ((bio = BIO_new_file(filename, "r")) == NULL) {
		log_crypto_err("get_ocsp: BIO_new_file failed");
		return -1;
	}

	if ((response = d2i_OCSP_RESPONSE_bio(bio, NULL)) == NULL) {
		log_crypto_err("get_ocsp: d2i_OCSP_RESPONSE_bio failed");
		BIO_free(bio);
		return -1;
	}

	if ((len = i2d_OCSP_RESPONSE(response, NULL)) <= 0) {
		log_crypto_err("get_ocsp: i2d_OCSP_RESPONSE #1 failed");
		OCSP_RESPONSE_free(response);
		BIO_free(bio);
		return -1;
	}

	if ((buf = malloc((size_t) len)) == NULL) {
		log_msg(LOG_ERR, "get_ocsp: malloc failed");
		OCSP_RESPONSE_free(response);
		BIO_free(bio);
		return -1;
	}

	p = buf;
	if ((len = i2d_OCSP_RESPONSE(response, &p)) <= 0) {
		log_crypto_err("get_ocsp: i2d_OCSP_RESPONSE #2 failed");
		free(buf);
		OCSP_RESPONSE_free(response);
		BIO_free(bio);
		return -1;
	}

	OCSP_RESPONSE_free(response);
	BIO_free(bio);

	*ocsp = buf;
	return len;
}

/* further setup ssl ctx after the keys are loaded */
static void
listen_sslctx_setup_2(void* ctxt)
{
	SSL_CTX* ctx = (SSL_CTX*)ctxt;
	(void)ctx;
#if HAVE_DECL_SSL_CTX_SET_ECDH_AUTO
	if(!SSL_CTX_set_ecdh_auto(ctx,1)) {
		log_crypto_err("Error in SSL_CTX_ecdh_auto, not enabling ECDHE");
	}
#elif defined(HAVE_DECL_SSL_CTX_SET_TMP_ECDH) && defined(NID_X9_62_prime256v1)
	if(1) {
		EC_KEY *ecdh = EC_KEY_new_by_curve_name (NID_X9_62_prime256v1);
		if (!ecdh) {
			log_crypto_err("could not find p256, not enabling ECDHE");
		} else {
			if (1 != SSL_CTX_set_tmp_ecdh (ctx, ecdh)) {
				log_crypto_err("Error in SSL_CTX_set_tmp_ecdh, not enabling ECDHE");
			}
			EC_KEY_free (ecdh);
		}
	}
#endif
}

static int
add_ocsp_data_cb(SSL *s, void* ATTR_UNUSED(arg))
{
	if(ocspdata) {
		unsigned char *p;
		if ((p=malloc(ocspdata_len)) == NULL) {
			log_msg(LOG_ERR, "add_ocsp_data_cb: malloc failure");
			return SSL_TLSEXT_ERR_NOACK;
		}
		memcpy(p, ocspdata, ocspdata_len);
		if ((SSL_set_tlsext_status_ocsp_resp(s, p, ocspdata_len)) != 1) {
			log_crypto_err("Error in SSL_set_tlsext_status_ocsp_resp");
			free(p);
			return SSL_TLSEXT_ERR_NOACK;
		}
		return SSL_TLSEXT_ERR_OK;
	} else {
		return SSL_TLSEXT_ERR_NOACK;
	}
}

SSL_CTX*
server_tls_ctx_setup(char* key, char* pem, char* verifypem)
{
	SSL_CTX *ctx = SSL_CTX_new(SSLv23_server_method());
	if(!ctx) {
		log_crypto_err("could not SSL_CTX_new");
		return NULL;
	}
	/* no SSLv2, SSLv3 because has defects */
	if((SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2) & SSL_OP_NO_SSLv2) != SSL_OP_NO_SSLv2){
		log_crypto_err("could not set SSL_OP_NO_SSLv2");
		SSL_CTX_free(ctx);
		return NULL;
	}
	if((SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3) & SSL_OP_NO_SSLv3)
		!= SSL_OP_NO_SSLv3){
		log_crypto_err("could not set SSL_OP_NO_SSLv3");
		SSL_CTX_free(ctx);
		return 0;
	}
#if defined(SSL_OP_NO_TLSv1) && defined(SSL_OP_NO_TLSv1_1)
	/* if we have tls 1.1 disable 1.0 */
	if((SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1) & SSL_OP_NO_TLSv1)
		!= SSL_OP_NO_TLSv1){
		log_crypto_err("could not set SSL_OP_NO_TLSv1");
		SSL_CTX_free(ctx);
		return 0;
	}
#endif
#if defined(SSL_OP_NO_TLSv1_1) && defined(SSL_OP_NO_TLSv1_2)
	/* if we have tls 1.2 disable 1.1 */
	if((SSL_CTX_set_options(ctx, SSL_OP_NO_TLSv1_1) & SSL_OP_NO_TLSv1_1)
		!= SSL_OP_NO_TLSv1_1){
		log_crypto_err("could not set SSL_OP_NO_TLSv1_1");
		SSL_CTX_free(ctx);
		return 0;
	}
#endif
#if defined(SHA256_DIGEST_LENGTH) && defined(SSL_TXT_CHACHA20)
	/* if we have sha256, set the cipher list to have no known vulns */
	if(!SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM:ECDHE+CHACHA20"))
		log_crypto_err("could not set cipher list with SSL_CTX_set_cipher_list");
#endif
	if((SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE) &
		SSL_OP_CIPHER_SERVER_PREFERENCE) !=
		SSL_OP_CIPHER_SERVER_PREFERENCE) {
		log_crypto_err("could not set SSL_OP_CIPHER_SERVER_PREFERENCE");
		SSL_CTX_free(ctx);
		return 0;
	}
#ifdef HAVE_SSL_CTX_SET_SECURITY_LEVEL
	SSL_CTX_set_security_level(ctx, 0);
#endif
	if(!SSL_CTX_use_certificate_chain_file(ctx, pem)) {
		log_msg(LOG_ERR, "error for cert file: %s", pem);
		log_crypto_err("error in SSL_CTX use_certificate_chain_file");
		SSL_CTX_free(ctx);
		return NULL;
	}
	if(!SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM)) {
		log_msg(LOG_ERR, "error for private key file: %s", key);
		log_crypto_err("Error in SSL_CTX use_PrivateKey_file");
		SSL_CTX_free(ctx);
		return NULL;
	}
	if(!SSL_CTX_check_private_key(ctx)) {
		log_msg(LOG_ERR, "error for key file: %s", key);
		log_crypto_err("Error in SSL_CTX check_private_key");
		SSL_CTX_free(ctx);
		return NULL;
	}
	listen_sslctx_setup_2(ctx);
	if(verifypem && verifypem[0]) {
		if(!SSL_CTX_load_verify_locations(ctx, verifypem, NULL)) {
			log_crypto_err("Error in SSL_CTX verify locations");
			SSL_CTX_free(ctx);
			return NULL;
		}
		SSL_CTX_set_client_CA_list(ctx, SSL_load_client_CA_file(verifypem));
		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
	}
	return ctx;
}

SSL_CTX*
server_tls_ctx_create(struct nsd* nsd, char* verifypem, char* ocspfile)
{
	char *key, *pem;
	SSL_CTX *ctx;

	key = nsd->options->tls_service_key;
	pem = nsd->options->tls_service_pem;
	if(!key || key[0] == 0) {
		log_msg(LOG_ERR, "error: no tls-service-key file specified");
		return NULL;
	}
	if(!pem || pem[0] == 0) {
		log_msg(LOG_ERR, "error: no tls-service-pem file specified");
		return NULL;
	}

	/* NOTE:This mimics the existing code in Unbound 1.5.1 by supporting SSL but
	 * raft-ietf-uta-tls-bcp-08 recommends only using TLSv1.2*/
	ctx = server_tls_ctx_setup(key, pem, verifypem);
	if(!ctx) {
		log_msg(LOG_ERR, "could not setup server TLS context");
		return NULL;
	}
	if(ocspfile && ocspfile[0]) {
		if ((ocspdata_len = get_ocsp(ocspfile, &ocspdata)) < 0) {
			log_crypto_err("Error reading OCSPfile");
			SSL_CTX_free(ctx);
			return NULL;
		} else {
			VERBOSITY(2, (LOG_INFO, "ocspfile %s loaded", ocspfile));
			if(!SSL_CTX_set_tlsext_status_cb(ctx, add_ocsp_data_cb)) {
				log_crypto_err("Error in SSL_CTX_set_tlsext_status_cb");
				SSL_CTX_free(ctx);
				return NULL;
			}
		}
	}
	return ctx;
}

/* check if tcp_handler_accept_data created for TLS dedicated port */
int
using_tls_port(struct sockaddr* addr, const char* tls_port)
{
	in_port_t port = 0;

	if (addr->sa_family == AF_INET)
		port = ((struct sockaddr_in*)addr)->sin_port;
#ifndef HAVE_STRUCT_SOCKADDR_IN6
	else
		port = ((struct sockaddr_in6*)addr)->sin6_port;
#endif /* HAVE_STRUCT_SOCKADDR_IN6 */
	if (atoi(tls_port) == ntohs(port))
		return 1;

	return 0;
}
#endif

/* pass timeout=-1 for blocking. Returns size, 0, -1(err), or -2(timeout) */
ssize_t
block_read(struct nsd* nsd, int s, void* p, ssize_t sz, int timeout)
{
	uint8_t* buf = (uint8_t*) p;
	ssize_t total = 0;
	struct pollfd fd;
	memset(&fd, 0, sizeof(fd));
	fd.fd = s;
	fd.events = POLLIN;
	
	while( total < sz) {
		ssize_t ret;
		ret = poll(&fd, 1, (timeout==-1)?-1:timeout*1000);
		if(ret == -1) {
			if(errno == EAGAIN)
				/* blocking read */
				continue;
			if(errno == EINTR) {
				if(nsd && (nsd->signal_hint_quit || nsd->signal_hint_shutdown))
					return -1;
				/* other signals can be handled later */
				continue;
			}
			/* some error */
			return -1;
		}
		if(ret == 0) {
			/* operation timed out */
			return -2;
		}
		ret = read(s, buf+total, sz-total);
		if(ret == -1) {
			if(errno == EAGAIN)
				/* blocking read */
				continue;
			if(errno == EINTR) {
				if(nsd && (nsd->signal_hint_quit || nsd->signal_hint_shutdown))
					return -1;
				/* other signals can be handled later */
				continue;
			}
			/* some error */
			return -1;
		}
		if(ret == 0) {
			/* closed connection! */
			return 0;
		}
		total += ret;
	}
	return total;
}

static void
reload_process_tasks(struct nsd* nsd, udb_ptr* last_task, int cmdsocket)
{
	sig_atomic_t cmd = NSD_QUIT_SYNC;
	udb_ptr t, next;
	udb_base* u = nsd->task[nsd->mytask];
	udb_ptr_init(&next, u);
	udb_ptr_new(&t, u, udb_base_get_userdata(u));
	udb_base_set_userdata(u, 0);
	while(!udb_ptr_is_null(&t)) {
		/* store next in list so this one can be deleted or reused */
		udb_ptr_set_rptr(&next, u, &TASKLIST(&t)->next);
		udb_rptr_zero(&TASKLIST(&t)->next, u);

		/* process task t */
		/* append results for task t and update last_task */
		task_process_in_reload(nsd, u, last_task, &t);

		/* go to next */
		udb_ptr_set_ptr(&t, u, &next);

		/* if the parent has quit, we must quit too, poll the fd for cmds */
		if(block_read(nsd, cmdsocket, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
			DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: ipc command from main %d", (int)cmd));
			if(cmd == NSD_QUIT) {
				DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: quit to follow nsd"));
				/* sync to disk (if needed) */
				udb_base_sync(nsd->db->udb, 0);
				/* unlink files of remainder of tasks */
				while(!udb_ptr_is_null(&t)) {
					if(TASKLIST(&t)->task_type == task_apply_xfr) {
						xfrd_unlink_xfrfile(nsd, TASKLIST(&t)->yesno);
					}
					udb_ptr_set_rptr(&t, u, &TASKLIST(&t)->next);
				}
				udb_ptr_unlink(&t, u);
				udb_ptr_unlink(&next, u);
				exit(0);
			}
		}

	}
	udb_ptr_unlink(&t, u);
	udb_ptr_unlink(&next, u);
}

#ifdef BIND8_STATS
static void
parent_send_stats(struct nsd* nsd, int cmdfd)
{
	size_t i;
	if(!write_socket(cmdfd, &nsd->st, sizeof(nsd->st))) {
		log_msg(LOG_ERR, "could not write stats to reload");
		return;
	}
	for(i=0; i<nsd->child_count; i++)
		if(!write_socket(cmdfd, &nsd->children[i].query_count,
			sizeof(stc_type))) {
			log_msg(LOG_ERR, "could not write stats to reload");
			return;
		}
}

static void
reload_do_stats(int cmdfd, struct nsd* nsd, udb_ptr* last)
{
	struct nsdst s;
	stc_type* p;
	size_t i;
	if(block_read(nsd, cmdfd, &s, sizeof(s),
		RELOAD_SYNC_TIMEOUT) != sizeof(s)) {
		log_msg(LOG_ERR, "could not read stats from oldpar");
		return;
	}
	s.db_disk = (nsd->db->udb?nsd->db->udb->base_size:0);
	s.db_mem = region_get_mem(nsd->db->region);
	p = (stc_type*)task_new_stat_info(nsd->task[nsd->mytask], last, &s,
		nsd->child_count);
	if(!p) return;
	for(i=0; i<nsd->child_count; i++) {
		if(block_read(nsd, cmdfd, p++, sizeof(stc_type), 1)!=
			sizeof(stc_type))
			return;
	}
}
#endif /* BIND8_STATS */

/*
 * Reload the database, stop parent, re-fork children and continue.
 * as server_main.
 */
static void
server_reload(struct nsd *nsd, region_type* server_region, netio_type* netio,
	int cmdsocket)
{
	pid_t mypid;
	sig_atomic_t cmd = NSD_QUIT_SYNC;
	int ret;
	udb_ptr last_task;
	struct sigaction old_sigchld, ign_sigchld;
	/* ignore SIGCHLD from the previous server_main that used this pid */
	memset(&ign_sigchld, 0, sizeof(ign_sigchld));
	ign_sigchld.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &ign_sigchld, &old_sigchld);

	/* see what tasks we got from xfrd */
	task_remap(nsd->task[nsd->mytask]);
	udb_ptr_init(&last_task, nsd->task[nsd->mytask]);
	udb_compact_inhibited(nsd->db->udb, 1);
	reload_process_tasks(nsd, &last_task, cmdsocket);
	udb_compact_inhibited(nsd->db->udb, 0);
	udb_compact(nsd->db->udb);

#ifndef NDEBUG
	if(nsd_debug_level >= 1)
		region_log_stats(nsd->db->region);
#endif /* NDEBUG */
	/* sync to disk (if needed) */
	udb_base_sync(nsd->db->udb, 0);

	initialize_dname_compression_tables(nsd);

#ifdef BIND8_STATS
	/* Restart dumping stats if required.  */
	time(&nsd->st.boot);
	set_bind8_alarm(nsd);
#endif
#ifdef USE_ZONE_STATS
	server_zonestat_realloc(nsd); /* realloc for new children */
	server_zonestat_switch(nsd);
#endif

	/* listen for the signals of failed children again */
	sigaction(SIGCHLD, &old_sigchld, NULL);
	/* Start new child processes */
	if (server_start_children(nsd, server_region, netio, &nsd->
		xfrd_listener->fd) != 0) {
		send_children_quit(nsd);
		exit(1);
	}

	/* if the parent has quit, we must quit too, poll the fd for cmds */
	if(block_read(nsd, cmdsocket, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
		DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: ipc command from main %d", (int)cmd));
		if(cmd == NSD_QUIT) {
			DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: quit to follow nsd"));
			send_children_quit(nsd);
			exit(0);
		}
	}

	/* Send quit command to parent: blocking, wait for receipt. */
	do {
		DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: ipc send quit to main"));
		if (!write_socket(cmdsocket, &cmd, sizeof(cmd)))
		{
			log_msg(LOG_ERR, "problems sending command from reload to oldnsd: %s",
				strerror(errno));
		}
		/* blocking: wait for parent to really quit. (it sends RELOAD as ack) */
		DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: ipc wait for ack main"));
		ret = block_read(nsd, cmdsocket, &cmd, sizeof(cmd),
			RELOAD_SYNC_TIMEOUT);
		if(ret == -2) {
			DEBUG(DEBUG_IPC, 1, (LOG_ERR, "reload timeout QUITSYNC. retry"));
		}
	} while (ret == -2);
	if(ret == -1) {
		log_msg(LOG_ERR, "reload: could not wait for parent to quit: %s",
			strerror(errno));
	}
	DEBUG(DEBUG_IPC,1, (LOG_INFO, "reload: ipc reply main %d %d", ret, (int)cmd));
	if(cmd == NSD_QUIT) {
		/* small race condition possible here, parent got quit cmd. */
		send_children_quit(nsd);
		exit(1);
	}
	assert(ret==-1 || ret == 0 || cmd == NSD_RELOAD);
#ifdef BIND8_STATS
	reload_do_stats(cmdsocket, nsd, &last_task);
#endif
	udb_ptr_unlink(&last_task, nsd->task[nsd->mytask]);
	task_process_sync(nsd->task[nsd->mytask]);
#ifdef USE_ZONE_STATS
	server_zonestat_realloc(nsd); /* realloc for next children */
#endif

	/* send soainfo to the xfrd process, signal it that reload is done,
	 * it picks up the taskudb */
	cmd = NSD_RELOAD_DONE;
	if(!write_socket(nsd->xfrd_listener->fd, &cmd,  sizeof(cmd))) {
		log_msg(LOG_ERR, "problems sending reload_done xfrd: %s",
			strerror(errno));
	}
	mypid = getpid();
	if(!write_socket(nsd->xfrd_listener->fd, &mypid,  sizeof(mypid))) {
		log_msg(LOG_ERR, "problems sending reloadpid to xfrd: %s",
			strerror(errno));
	}

	/* try to reopen file */
	if (nsd->file_rotation_ok)
		log_reopen(nsd->log_filename, 1);
	/* exit reload, continue as new server_main */
}

/*
 * Get the mode depending on the signal hints that have been received.
 * Multiple signal hints can be received and will be handled in turn.
 */
static sig_atomic_t
server_signal_mode(struct nsd *nsd)
{
	if(nsd->signal_hint_quit) {
		nsd->signal_hint_quit = 0;
		return NSD_QUIT;
	}
	else if(nsd->signal_hint_shutdown) {
		nsd->signal_hint_shutdown = 0;
		return NSD_SHUTDOWN;
	}
	else if(nsd->signal_hint_child) {
		nsd->signal_hint_child = 0;
		return NSD_REAP_CHILDREN;
	}
	else if(nsd->signal_hint_reload) {
		nsd->signal_hint_reload = 0;
		return NSD_RELOAD;
	}
	else if(nsd->signal_hint_reload_hup) {
		nsd->signal_hint_reload_hup = 0;
		return NSD_RELOAD_REQ;
	}
	else if(nsd->signal_hint_stats) {
		nsd->signal_hint_stats = 0;
#ifdef BIND8_STATS
		set_bind8_alarm(nsd);
#endif
		return NSD_STATS;
	}
	else if(nsd->signal_hint_statsusr) {
		nsd->signal_hint_statsusr = 0;
		return NSD_STATS;
	}
	return NSD_RUN;
}

/*
 * The main server simply waits for signals and child processes to
 * terminate.  Child processes are restarted as necessary.
 */
void
server_main(struct nsd *nsd)
{
	region_type *server_region = region_create(xalloc, free);
	netio_type *netio = netio_create(server_region);
	netio_handler_type reload_listener;
	int reload_sockets[2] = {-1, -1};
	struct timespec timeout_spec;
	int status;
	pid_t child_pid;
	pid_t reload_pid = -1;
	sig_atomic_t mode;

	/* Ensure we are the main process */
	assert(nsd->server_kind == NSD_SERVER_MAIN);

	/* Add listener for the XFRD process */
	netio_add_handler(netio, nsd->xfrd_listener);

	/* Start the child processes that handle incoming queries */
	if (server_start_children(nsd, server_region, netio,
		&nsd->xfrd_listener->fd) != 0) {
		send_children_quit(nsd);
		exit(1);
	}
	reload_listener.fd = -1;

	/* This_child MUST be 0, because this is the parent process */
	assert(nsd->this_child == 0);

	/* Run the server until we get a shutdown signal */
	while ((mode = nsd->mode) != NSD_SHUTDOWN) {
		/* Did we receive a signal that changes our mode? */
		if(mode == NSD_RUN) {
			nsd->mode = mode = server_signal_mode(nsd);
		}

		switch (mode) {
		case NSD_RUN:
			/* see if any child processes terminated */
			while((child_pid = waitpid(-1, &status, WNOHANG)) != -1 && child_pid != 0) {
				int is_child = delete_child_pid(nsd, child_pid);
				if (is_child != -1 && nsd->children[is_child].need_to_exit) {
					if(nsd->children[is_child].child_fd == -1)
						nsd->children[is_child].has_exited = 1;
					parent_check_all_children_exited(nsd);
				} else if(is_child != -1) {
					log_msg(LOG_WARNING,
					       "server %d died unexpectedly with status %d, restarting",
					       (int) child_pid, status);
					restart_child_servers(nsd, server_region, netio,
						&nsd->xfrd_listener->fd);
				} else if (child_pid == reload_pid) {
					sig_atomic_t cmd = NSD_RELOAD_DONE;
					pid_t mypid;
					log_msg(LOG_WARNING,
					       "Reload process %d failed with status %d, continuing with old database",
					       (int) child_pid, status);
					reload_pid = -1;
					if(reload_listener.fd != -1) close(reload_listener.fd);
					reload_listener.fd = -1;
					reload_listener.event_types = NETIO_EVENT_NONE;
					task_process_sync(nsd->task[nsd->mytask]);
					/* inform xfrd reload attempt ended */
					if(!write_socket(nsd->xfrd_listener->fd,
						&cmd, sizeof(cmd))) {
						log_msg(LOG_ERR, "problems "
						  "sending SOAEND to xfrd: %s",
						  strerror(errno));
					}
					mypid = getpid();
					if(!write_socket(nsd->xfrd_listener->fd, &mypid,  sizeof(mypid))) {
						log_msg(LOG_ERR, "problems sending reloadpid to xfrd: %s",
							strerror(errno));
					}
				} else if(status != 0) {
					/* check for status, because we get
					 * the old-servermain because reload
					 * is the process-parent of old-main,
					 * and we get older server-processes
					 * that are exiting after a reload */
					log_msg(LOG_WARNING,
					       "process %d terminated with status %d",
					       (int) child_pid, status);
				}
			}
			if (child_pid == -1) {
				if (errno == EINTR) {
					continue;
				}
				if (errno != ECHILD)
					log_msg(LOG_WARNING, "wait failed: %s", strerror(errno));
			}
			if (nsd->mode != NSD_RUN)
				break;

			/* timeout to collect processes. In case no sigchild happens. */
			timeout_spec.tv_sec = 60;
			timeout_spec.tv_nsec = 0;

			/* listen on ports, timeout for collecting terminated children */
			if(netio_dispatch(netio, &timeout_spec, 0) == -1) {
				if (errno != EINTR) {
					log_msg(LOG_ERR, "netio_dispatch failed: %s", strerror(errno));
				}
			}
			if(nsd->restart_children) {
				restart_child_servers(nsd, server_region, netio,
					&nsd->xfrd_listener->fd);
				nsd->restart_children = 0;
			}
			if(nsd->reload_failed) {
				sig_atomic_t cmd = NSD_RELOAD_DONE;
				pid_t mypid;
				nsd->reload_failed = 0;
				log_msg(LOG_WARNING,
				       "Reload process %d failed, continuing with old database",
				       (int) reload_pid);
				reload_pid = -1;
				if(reload_listener.fd != -1) close(reload_listener.fd);
				reload_listener.fd = -1;
				reload_listener.event_types = NETIO_EVENT_NONE;
				task_process_sync(nsd->task[nsd->mytask]);
				/* inform xfrd reload attempt ended */
				if(!write_socket(nsd->xfrd_listener->fd,
					&cmd, sizeof(cmd))) {
					log_msg(LOG_ERR, "problems "
					  "sending SOAEND to xfrd: %s",
					  strerror(errno));
				}
				mypid = getpid();
				if(!write_socket(nsd->xfrd_listener->fd, &mypid,  sizeof(mypid))) {
					log_msg(LOG_ERR, "problems sending reloadpid to xfrd: %s",
						strerror(errno));
				}
			}

			break;
		case NSD_RELOAD_REQ: {
			sig_atomic_t cmd = NSD_RELOAD_REQ;
			log_msg(LOG_WARNING, "SIGHUP received, reloading...");
			DEBUG(DEBUG_IPC,1, (LOG_INFO,
				"main: ipc send reload_req to xfrd"));
			if(!write_socket(nsd->xfrd_listener->fd,
				&cmd, sizeof(cmd))) {
				log_msg(LOG_ERR, "server_main: could not send "
				"reload_req to xfrd: %s", strerror(errno));
			}
			nsd->mode = NSD_RUN;
			} break;
		case NSD_RELOAD:
			/* Continue to run nsd after reload */
			nsd->mode = NSD_RUN;
			DEBUG(DEBUG_IPC,1, (LOG_INFO, "reloading..."));
			if (reload_pid != -1) {
				log_msg(LOG_WARNING, "Reload already in progress (pid = %d)",
				       (int) reload_pid);
				break;
			}

			/* switch the mytask to keep track of who owns task*/
			nsd->mytask = 1 - nsd->mytask;
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, reload_sockets) == -1) {
				log_msg(LOG_ERR, "reload failed on socketpair: %s", strerror(errno));
				reload_pid = -1;
				break;
			}

			/* Do actual reload */
			reload_pid = fork();
			switch (reload_pid) {
			case -1:
				log_msg(LOG_ERR, "fork failed: %s", strerror(errno));
				break;
			default:
				/* PARENT */
				close(reload_sockets[0]);
				server_reload(nsd, server_region, netio,
					reload_sockets[1]);
				DEBUG(DEBUG_IPC,2, (LOG_INFO, "Reload exited to become new main"));
				close(reload_sockets[1]);
				DEBUG(DEBUG_IPC,2, (LOG_INFO, "Reload closed"));
				/* drop stale xfrd ipc data */
				((struct ipc_handler_conn_data*)nsd->
					xfrd_listener->user_data)
					->conn->is_reading = 0;
				reload_pid = -1;
				reload_listener.fd = -1;
				reload_listener.event_types = NETIO_EVENT_NONE;
				DEBUG(DEBUG_IPC,2, (LOG_INFO, "Reload resetup; run"));
				break;
			case 0:
				/* CHILD */
				/* server_main keep running until NSD_QUIT_SYNC
				 * received from reload. */
				close(reload_sockets[1]);
				reload_listener.fd = reload_sockets[0];
				reload_listener.timeout = NULL;
				reload_listener.user_data = nsd;
				reload_listener.event_types = NETIO_EVENT_READ;
				reload_listener.event_handler = parent_handle_reload_command; /* listens to Quit */
				netio_add_handler(netio, &reload_listener);
				reload_pid = getppid();
				break;
			}
			break;
		case NSD_QUIT_SYNC:
			/* synchronisation of xfrd, parent and reload */
			if(!nsd->quit_sync_done && reload_listener.fd != -1) {
				sig_atomic_t cmd = NSD_RELOAD;
				/* stop xfrd ipc writes in progress */
				DEBUG(DEBUG_IPC,1, (LOG_INFO,
					"main: ipc send indication reload"));
				if(!write_socket(nsd->xfrd_listener->fd,
					&cmd, sizeof(cmd))) {
					log_msg(LOG_ERR, "server_main: could not send reload "
					"indication to xfrd: %s", strerror(errno));
				}
				/* wait for ACK from xfrd */
				DEBUG(DEBUG_IPC,1, (LOG_INFO, "main: wait ipc reply xfrd"));
				nsd->quit_sync_done = 1;
			}
			nsd->mode = NSD_RUN;
			break;
		case NSD_QUIT:
			/* silent shutdown during reload */
			if(reload_listener.fd != -1) {
				/* acknowledge the quit, to sync reload that we will really quit now */
				sig_atomic_t cmd = NSD_RELOAD;
				DEBUG(DEBUG_IPC,1, (LOG_INFO, "main: ipc ack reload"));
				if(!write_socket(reload_listener.fd, &cmd, sizeof(cmd))) {
					log_msg(LOG_ERR, "server_main: "
						"could not ack quit: %s", strerror(errno));
				}
#ifdef BIND8_STATS
				parent_send_stats(nsd, reload_listener.fd);
#endif /* BIND8_STATS */
				close(reload_listener.fd);
			}
			DEBUG(DEBUG_IPC,1, (LOG_INFO, "server_main: shutdown sequence"));
			/* only quit children after xfrd has acked */
			send_children_quit(nsd);

#ifdef MEMCLEAN /* OS collects memory pages */
			region_destroy(server_region);
#endif
			server_shutdown(nsd);

			/* ENOTREACH */
			break;
		case NSD_SHUTDOWN:
			break;
		case NSD_REAP_CHILDREN:
			/* continue; wait for child in run loop */
			nsd->mode = NSD_RUN;
			break;
		case NSD_STATS:
#ifdef BIND8_STATS
			set_children_stats(nsd);
#endif
			nsd->mode = NSD_RUN;
			break;
		default:
			log_msg(LOG_WARNING, "NSD main server mode invalid: %d", (int)nsd->mode);
			nsd->mode = NSD_RUN;
			break;
		}
	}
	log_msg(LOG_WARNING, "signal received, shutting down...");

	/* close opened ports to avoid race with restart of nsd */
	server_close_all_sockets(nsd->udp, nsd->ifs);
	server_close_all_sockets(nsd->tcp, nsd->ifs);
#ifdef HAVE_SSL
	daemon_remote_close(nsd->rc);
#endif
	send_children_quit_and_wait(nsd);

	/* Unlink it if possible... */
	unlinkpid(nsd->pidfile);
	unlink(nsd->task[0]->fname);
	unlink(nsd->task[1]->fname);
#ifdef USE_ZONE_STATS
	unlink(nsd->zonestatfname[0]);
	unlink(nsd->zonestatfname[1]);
#endif
#ifdef USE_DNSTAP
	dt_collector_close(nsd->dt_collector, nsd);
#endif

	if(reload_listener.fd != -1) {
		sig_atomic_t cmd = NSD_QUIT;
		DEBUG(DEBUG_IPC,1, (LOG_INFO,
			"main: ipc send quit to reload-process"));
		if(!write_socket(reload_listener.fd, &cmd, sizeof(cmd))) {
			log_msg(LOG_ERR, "server_main: could not send quit to reload: %s",
				strerror(errno));
		}
		fsync(reload_listener.fd);
		close(reload_listener.fd);
		/* wait for reload to finish processing */
		while(1) {
			if(waitpid(reload_pid, NULL, 0) == -1) {
				if(errno == EINTR) continue;
				if(errno == ECHILD) break;
				log_msg(LOG_ERR, "waitpid(reload %d): %s",
					(int)reload_pid, strerror(errno));
			}
			break;
		}
	}
	if(nsd->xfrd_listener->fd != -1) {
		/* complete quit, stop xfrd */
		sig_atomic_t cmd = NSD_QUIT;
		DEBUG(DEBUG_IPC,1, (LOG_INFO,
			"main: ipc send quit to xfrd"));
		if(!write_socket(nsd->xfrd_listener->fd, &cmd, sizeof(cmd))) {
			log_msg(LOG_ERR, "server_main: could not send quit to xfrd: %s",
				strerror(errno));
		}
		fsync(nsd->xfrd_listener->fd);
		close(nsd->xfrd_listener->fd);
		(void)kill(nsd->pid, SIGTERM);
	}

#ifdef MEMCLEAN /* OS collects memory pages */
	region_destroy(server_region);
#endif
	/* write the nsd.db to disk, wait for it to complete */
	udb_base_sync(nsd->db->udb, 1);
	udb_base_close(nsd->db->udb);
	server_shutdown(nsd);
}

static query_state_type
server_process_query(struct nsd *nsd, struct query *query)
{
	return query_process(query, nsd);
}

static query_state_type
server_process_query_udp(struct nsd *nsd, struct query *query)
{
#ifdef RATELIMIT
	if(query_process(query, nsd) != QUERY_DISCARDED) {
		if(rrl_process_query(query))
			return rrl_slip(query);
		else	return QUERY_PROCESSED;
	}
	return QUERY_DISCARDED;
#else
	return query_process(query, nsd);
#endif
}

struct event_base*
nsd_child_event_base(void)
{
	struct event_base* base;
#ifdef USE_MINI_EVENT
	static time_t secs;
	static struct timeval now;
	base = event_init(&secs, &now);
#else
#  if defined(HAVE_EV_LOOP) || defined(HAVE_EV_DEFAULT_LOOP)
	/* libev */
	base = (struct event_base *)ev_default_loop(EVFLAG_AUTO);
#  else
	/* libevent */
#    ifdef HAVE_EVENT_BASE_NEW
	base = event_base_new();
#    else
	base = event_init();
#    endif
#  endif
#endif
	return base;
}

/*
 * Serve DNS requests.
 */
void
server_child(struct nsd *nsd)
{
	size_t i, from, numifs;
	region_type *server_region = region_create(xalloc, free);
	struct event_base* event_base = nsd_child_event_base();
	query_type *udp_query;
	sig_atomic_t mode;

	if(!event_base) {
		log_msg(LOG_ERR, "nsd server could not create event base");
		exit(1);
	}
	nsd->event_base = event_base;
	nsd->server_region = server_region;

#ifdef RATELIMIT
	rrl_init(nsd->this_child->child_num);
#endif

	assert(nsd->server_kind != NSD_SERVER_MAIN);
	DEBUG(DEBUG_IPC, 2, (LOG_INFO, "child process started"));

	if (!(nsd->server_kind & NSD_SERVER_TCP)) {
		server_close_all_sockets(nsd->tcp, nsd->ifs);
	}
	if (!(nsd->server_kind & NSD_SERVER_UDP)) {
		server_close_all_sockets(nsd->udp, nsd->ifs);
	}

	if (nsd->this_child->parent_fd != -1) {
		struct event *handler;
		struct ipc_handler_conn_data* user_data =
			(struct ipc_handler_conn_data*)region_alloc(
			server_region, sizeof(struct ipc_handler_conn_data));
		user_data->nsd = nsd;
		user_data->conn = xfrd_tcp_create(server_region, QIOBUFSZ);

		handler = (struct event*) region_alloc(
			server_region, sizeof(*handler));
		memset(handler, 0, sizeof(*handler));
		event_set(handler, nsd->this_child->parent_fd, EV_PERSIST|
			EV_READ, child_handle_parent_command, user_data);
		if(event_base_set(event_base, handler) != 0)
			log_msg(LOG_ERR, "nsd ipcchild: event_base_set failed");
		if(event_add(handler, NULL) != 0)
			log_msg(LOG_ERR, "nsd ipcchild: event_add failed");
	}

	if(nsd->reuseport) {
		numifs = nsd->ifs / nsd->reuseport;
		from = numifs * nsd->this_child->child_num;
		if(from+numifs > nsd->ifs) { /* should not happen */
			from = 0;
			numifs = nsd->ifs;
		}
	} else {
		from = 0;
		numifs = nsd->ifs;
	}

	if (nsd->server_kind & NSD_SERVER_UDP) {
#if (defined(NONBLOCKING_IS_BROKEN) || !defined(HAVE_RECVMMSG))
		udp_query = query_create(server_region,
			compressed_dname_offsets, compression_table_size,
			compressed_dnames);
#else
		udp_query = NULL;
		memset(msgs, 0, sizeof(msgs));
		for (i = 0; i < NUM_RECV_PER_SELECT; i++) {
			queries[i] = query_create(server_region,
				compressed_dname_offsets,
				compression_table_size, compressed_dnames);
			query_reset(queries[i], UDP_MAX_MESSAGE_LEN, 0);
			iovecs[i].iov_base          = buffer_begin(queries[i]->packet);
			iovecs[i].iov_len           = buffer_remaining(queries[i]->packet);;
			msgs[i].msg_hdr.msg_iov     = &iovecs[i];
			msgs[i].msg_hdr.msg_iovlen  = 1;
			msgs[i].msg_hdr.msg_name    = &queries[i]->addr;
			msgs[i].msg_hdr.msg_namelen = queries[i]->addrlen;
		}
#endif
		for (i = from; i < from+numifs; ++i) {
			struct udp_handler_data *data;
			struct event *handler;

			data = (struct udp_handler_data *) region_alloc(
				server_region,
				sizeof(struct udp_handler_data));
			data->query = udp_query;
			data->nsd = nsd;
			data->socket = &nsd->udp[i];

			handler = (struct event*) region_alloc(
				server_region, sizeof(*handler));
			memset(handler, 0, sizeof(*handler));
			event_set(handler, nsd->udp[i].s, EV_PERSIST|EV_READ,
				handle_udp, data);
			if(event_base_set(event_base, handler) != 0)
				log_msg(LOG_ERR, "nsd udp: event_base_set failed");
			if(event_add(handler, NULL) != 0)
				log_msg(LOG_ERR, "nsd udp: event_add failed");
		}
	}

	/*
	 * Keep track of all the TCP accept handlers so we can enable
	 * and disable them based on the current number of active TCP
	 * connections.
	 */
	tcp_accept_handler_count = numifs;
	tcp_accept_handlers = (struct tcp_accept_handler_data*)
		region_alloc_array(server_region,
		numifs, sizeof(*tcp_accept_handlers));
	if (nsd->server_kind & NSD_SERVER_TCP) {
		for (i = from; i < numifs; ++i) {
			struct event *handler = &tcp_accept_handlers[i-from].event;
			struct tcp_accept_handler_data* data =
				&tcp_accept_handlers[i-from];
			data->nsd = nsd;
			data->socket = &nsd->tcp[i];
#ifdef HAVE_SSL
			if (nsd->tls_ctx && nsd->options->tls_port && using_tls_port(
			    data->socket->addr->ai_addr, nsd->options->tls_port)) {
				data->tls_accept = 1;
				if(verbosity >= 2) {
					char buf[48];
					addrport2str((struct sockaddr_storage*)data->socket->addr->ai_addr, buf, sizeof(buf));
					VERBOSITY(2, (LOG_NOTICE, "setup TCP for TLS service on interface %s", buf));
				}
			}
			else
				data->tls_accept = 0;
#endif
			memset(handler, 0, sizeof(*handler));
			event_set(handler, nsd->tcp[i].s, EV_PERSIST|EV_READ,
				handle_tcp_accept, data);
			if(event_base_set(event_base, handler) != 0)
				log_msg(LOG_ERR, "nsd tcp: event_base_set failed");
			if(event_add(handler, NULL) != 0)
				log_msg(LOG_ERR, "nsd tcp: event_add failed");
			data->event_added = 1;
		}
	} else tcp_accept_handler_count = 0;

	/* The main loop... */
	while ((mode = nsd->mode) != NSD_QUIT) {
		if(mode == NSD_RUN) nsd->mode = mode = server_signal_mode(nsd);

		/* Do we need to do the statistics... */
		if (mode == NSD_STATS) {
#ifdef BIND8_STATS
			int p = nsd->st.period;
			nsd->st.period = 1; /* force stats printout */
			/* Dump the statistics */
			bind8_stats(nsd);
			nsd->st.period = p;
#else /* !BIND8_STATS */
			log_msg(LOG_NOTICE, "Statistics support not enabled at compile time.");
#endif /* BIND8_STATS */

			nsd->mode = NSD_RUN;
		}
		else if (mode == NSD_REAP_CHILDREN) {
			/* got signal, notify parent. parent reaps terminated children. */
			if (nsd->this_child->parent_fd != -1) {
				sig_atomic_t parent_notify = NSD_REAP_CHILDREN;
				if (write(nsd->this_child->parent_fd,
				    &parent_notify,
				    sizeof(parent_notify)) == -1)
				{
					log_msg(LOG_ERR, "problems sending command from %d to parent: %s",
						(int) nsd->this_child->pid, strerror(errno));
				}
			} else /* no parent, so reap 'em */
				while (waitpid(-1, NULL, WNOHANG) > 0) ;
			nsd->mode = NSD_RUN;
		}
		else if(mode == NSD_RUN) {
			/* Wait for a query... */
			if(event_base_loop(event_base, EVLOOP_ONCE) == -1) {
				if (errno != EINTR) {
					log_msg(LOG_ERR, "dispatch failed: %s", strerror(errno));
					break;
				}
			}
		} else if(mode == NSD_QUIT) {
			/* ignore here, quit */
		} else {
			log_msg(LOG_ERR, "mode bad value %d, back to service.",
				(int)mode);
			nsd->mode = NSD_RUN;
		}
	}

	service_remaining_tcp(nsd);
#ifdef	BIND8_STATS
	bind8_stats(nsd);
#endif /* BIND8_STATS */

#ifdef MEMCLEAN /* OS collects memory pages */
#ifdef RATELIMIT
	rrl_deinit(nsd->this_child->child_num);
#endif
	event_base_free(event_base);
	region_destroy(server_region);
#endif
	server_shutdown(nsd);
}

static void remaining_tcp_timeout(int ATTR_UNUSED(fd), short event, void* arg)
{
	int* timed_out = (int*)arg;
        assert(event & EV_TIMEOUT);
	/* wake up the service tcp thread, note event is no longer
	 * registered */
	*timed_out = 1;
}

void
service_remaining_tcp(struct nsd* nsd)
{
	struct tcp_handler_data* p;
	struct event_base* event_base;
	/* check if it is needed */
	if(nsd->current_tcp_count == 0 || tcp_active_list == NULL)
		return;
	VERBOSITY(4, (LOG_INFO, "service remaining TCP connections"));

	/* setup event base */
	event_base = nsd_child_event_base();
	if(!event_base) {
		log_msg(LOG_ERR, "nsd remain tcp could not create event base");
		return;
	}
	/* register tcp connections */
	for(p = tcp_active_list; p != NULL; p = p->next) {
		struct timeval timeout;
		int fd = p->event.ev_fd;
#ifdef USE_MINI_EVENT
		short event = p->event.ev_flags & (EV_READ|EV_WRITE);
#else
		short event = p->event.ev_events & (EV_READ|EV_WRITE);
#endif
		void (*fn)(int, short, void*);
#ifdef HAVE_SSL
		if(p->tls) {
			if((event&EV_READ))
				fn = handle_tls_reading;
			else	fn = handle_tls_writing;
		} else {
#endif
			if((event&EV_READ))
				fn = handle_tcp_reading;
			else	fn = handle_tcp_writing;
#ifdef HAVE_SSL
		}
#endif

		/* set timeout to 1/10 second */
		if(p->tcp_timeout > 100)
			p->tcp_timeout = 100;
		timeout.tv_sec = p->tcp_timeout / 1000;
		timeout.tv_usec = (p->tcp_timeout % 1000)*1000;
		event_del(&p->event);
		memset(&p->event, 0, sizeof(p->event));
		event_set(&p->event, fd, EV_PERSIST | event | EV_TIMEOUT,
			fn, p);
		if(event_base_set(event_base, &p->event) != 0)
			log_msg(LOG_ERR, "event base set failed");
		if(event_add(&p->event, &timeout) != 0)
			log_msg(LOG_ERR, "event add failed");
	}

	/* handle it */
	while(nsd->current_tcp_count > 0) {
		mode_t m = server_signal_mode(nsd);
		struct event timeout;
		struct timeval tv;
		int timed_out = 0;
		if(m == NSD_QUIT || m == NSD_SHUTDOWN ||
			m == NSD_REAP_CHILDREN) {
			/* quit */
			break;
		}
		/* timer */
		/* have to do something every second */
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		memset(&timeout, 0, sizeof(timeout));
		event_set(&timeout, -1, EV_TIMEOUT, remaining_tcp_timeout,
			&timed_out);
		if(event_base_set(event_base, &timeout) != 0)
			log_msg(LOG_ERR, "remaintcp timer: event_base_set failed");
		if(event_add(&timeout, &tv) != 0)
			log_msg(LOG_ERR, "remaintcp timer: event_add failed");

		/* service loop */
		if(event_base_loop(event_base, EVLOOP_ONCE) == -1) {
			if (errno != EINTR) {
				log_msg(LOG_ERR, "dispatch failed: %s", strerror(errno));
				break;
			}
		}
		if(!timed_out) {
			event_del(&timeout);
		} else {
			/* timed out, quit */
			VERBOSITY(4, (LOG_INFO, "service remaining TCP connections: timed out, quit"));
			break;
		}
	}
	event_base_free(event_base);
	/* continue to quit after return */
}

#if defined(HAVE_SENDMMSG) && !defined(NONBLOCKING_IS_BROKEN) && defined(HAVE_RECVMMSG)
static void
handle_udp(int fd, short event, void* arg)
{
	struct udp_handler_data *data = (struct udp_handler_data *) arg;
	int received, sent, recvcount, i;
	struct query *q;

	if (!(event & EV_READ)) {
		return;
	}
	recvcount = recvmmsg(fd, msgs, NUM_RECV_PER_SELECT, 0, NULL);
	/* this printf strangely gave a performance increase on Linux */
	/* printf("recvcount %d \n", recvcount); */
	if (recvcount == -1) {
		if (errno != EAGAIN && errno != EINTR) {
			log_msg(LOG_ERR, "recvmmsg failed: %s", strerror(errno));
			STATUP(data->nsd, rxerr);
			/* No zone statup */
		}
		/* Simply no data available */
		return;
	}
	for (i = 0; i < recvcount; i++) {
	loopstart:
		received = msgs[i].msg_len;
		queries[i]->addrlen = msgs[i].msg_hdr.msg_namelen;
		q = queries[i];
		if (received == -1) {
			log_msg(LOG_ERR, "recvmmsg %d failed %s", i, strerror(
				msgs[i].msg_hdr.msg_flags));
			STATUP(data->nsd, rxerr);
			/* No zone statup */
			query_reset(queries[i], UDP_MAX_MESSAGE_LEN, 0);
			iovecs[i].iov_len = buffer_remaining(q->packet);
			msgs[i].msg_hdr.msg_namelen = queries[i]->addrlen;
			goto swap_drop;
		}

		/* Account... */
#ifdef BIND8_STATS
		if (data->socket->fam == AF_INET) {
			STATUP(data->nsd, qudp);
		} else if (data->socket->fam == AF_INET6) {
			STATUP(data->nsd, qudp6);
		}
#endif

		buffer_skip(q->packet, received);
		buffer_flip(q->packet);
#ifdef USE_DNSTAP
		dt_collector_submit_auth_query(data->nsd, &q->addr, q->addrlen,
			q->tcp, q->packet);
#endif /* USE_DNSTAP */

		/* Process and answer the query... */
		if (server_process_query_udp(data->nsd, q) != QUERY_DISCARDED) {
			if (RCODE(q->packet) == RCODE_OK && !AA(q->packet)) {
				STATUP(data->nsd, nona);
				ZTATUP(data->nsd, q->zone, nona);
			}

#ifdef USE_ZONE_STATS
			if (data->socket->fam == AF_INET) {
				ZTATUP(data->nsd, q->zone, qudp);
			} else if (data->socket->fam == AF_INET6) {
				ZTATUP(data->nsd, q->zone, qudp6);
			}
#endif

			/* Add EDNS0 and TSIG info if necessary.  */
			query_add_optional(q, data->nsd);

			buffer_flip(q->packet);
			iovecs[i].iov_len = buffer_remaining(q->packet);
#ifdef BIND8_STATS
			/* Account the rcode & TC... */
			STATUP2(data->nsd, rcode, RCODE(q->packet));
			ZTATUP2(data->nsd, q->zone, rcode, RCODE(q->packet));
			if (TC(q->packet)) {
				STATUP(data->nsd, truncated);
				ZTATUP(data->nsd, q->zone, truncated);
			}
#endif /* BIND8_STATS */
#ifdef USE_DNSTAP
			dt_collector_submit_auth_response(data->nsd,
				&q->addr, q->addrlen, q->tcp, q->packet,
				q->zone);
#endif /* USE_DNSTAP */
		} else {
			query_reset(queries[i], UDP_MAX_MESSAGE_LEN, 0);
			iovecs[i].iov_len = buffer_remaining(q->packet);
			msgs[i].msg_hdr.msg_namelen = queries[i]->addrlen;
		swap_drop:
			STATUP(data->nsd, dropped);
			ZTATUP(data->nsd, q->zone, dropped);
			if(i != recvcount-1) {
				/* swap with last and decrease recvcount */
				struct mmsghdr mtmp = msgs[i];
				struct iovec iotmp = iovecs[i];
				recvcount--;
				msgs[i] = msgs[recvcount];
				iovecs[i] = iovecs[recvcount];
				queries[i] = queries[recvcount];
				msgs[recvcount] = mtmp;
				iovecs[recvcount] = iotmp;
				queries[recvcount] = q;
				msgs[i].msg_hdr.msg_iov = &iovecs[i];
				msgs[recvcount].msg_hdr.msg_iov = &iovecs[recvcount];
				goto loopstart;
			} else { recvcount --; }
		}
	}

	/* send until all are sent */
	i = 0;
	while(i<recvcount) {
		sent = sendmmsg(fd, &msgs[i], recvcount-i, 0);
		if(sent == -1) {
			const char* es = strerror(errno);
			char a[48];
			addr2str(&queries[i]->addr, a, sizeof(a));
			log_msg(LOG_ERR, "sendmmsg [0]=%s count=%d failed: %s", a, (int)(recvcount-i), es);
#ifdef BIND8_STATS
			data->nsd->st.txerr += recvcount-i;
#endif /* BIND8_STATS */
			break;
		}
		i += sent;
	}
	for(i=0; i<recvcount; i++) {
		query_reset(queries[i], UDP_MAX_MESSAGE_LEN, 0);
		iovecs[i].iov_len = buffer_remaining(queries[i]->packet);
		msgs[i].msg_hdr.msg_namelen = queries[i]->addrlen;
	}
}

#else /* defined(HAVE_SENDMMSG) && !defined(NONBLOCKING_IS_BROKEN) && defined(HAVE_RECVMMSG) */

static void
handle_udp(int fd, short event, void* arg)
{
	struct udp_handler_data *data = (struct udp_handler_data *) arg;
	int received, sent;
#ifndef NONBLOCKING_IS_BROKEN
#ifdef HAVE_RECVMMSG
	int recvcount;
#endif /* HAVE_RECVMMSG */
	int i;
#endif /* NONBLOCKING_IS_BROKEN */
	struct query *q;
#if (defined(NONBLOCKING_IS_BROKEN) || !defined(HAVE_RECVMMSG))
	q = data->query;
#endif

	if (!(event & EV_READ)) {
		return;
	}
#ifndef NONBLOCKING_IS_BROKEN
#ifdef HAVE_RECVMMSG
	recvcount = recvmmsg(fd, msgs, NUM_RECV_PER_SELECT, 0, NULL);
	/* this printf strangely gave a performance increase on Linux */
	/* printf("recvcount %d \n", recvcount); */
	if (recvcount == -1) {
		if (errno != EAGAIN && errno != EINTR) {
			log_msg(LOG_ERR, "recvmmsg failed: %s", strerror(errno));
			STATUP(data->nsd, rxerr);
			/* No zone statup */
		}
		/* Simply no data available */
		return;
	}
	for (i = 0; i < recvcount; i++) {
		received = msgs[i].msg_len;
		queries[i]->addrlen = msgs[i].msg_hdr.msg_namelen;
		if (received == -1) {
			log_msg(LOG_ERR, "recvmmsg failed");
			STATUP(data->nsd, rxerr);
			/* No zone statup */
			/* the error can be found in msgs[i].msg_hdr.msg_flags */
			query_reset(queries[i], UDP_MAX_MESSAGE_LEN, 0);
			iovecs[i].iov_len = buffer_remaining(queries[i]->packet);
			msgs[i].msg_hdr.msg_namelen = queries[i]->addrlen;
			continue;
		}
		q = queries[i];
#else
	for(i=0; i<NUM_RECV_PER_SELECT; i++) {
#endif /* HAVE_RECVMMSG */
#endif /* NONBLOCKING_IS_BROKEN */

#if (defined(NONBLOCKING_IS_BROKEN) || !defined(HAVE_RECVMMSG))
		/* Initialize the query... */
		query_reset(q, UDP_MAX_MESSAGE_LEN, 0);

		received = recvfrom(fd,
				    buffer_begin(q->packet),
				    buffer_remaining(q->packet),
				    0,
				    (struct sockaddr *)&q->addr,
				    &q->addrlen);
		if (received == -1) {
			if (errno != EAGAIN && errno != EINTR) {
				log_msg(LOG_ERR, "recvfrom failed: %s", strerror(errno));
				STATUP(data->nsd, rxerr);
				/* No zone statup */
			}
			return;
		}
#endif /* NONBLOCKING_IS_BROKEN || !HAVE_RECVMMSG */

		/* Account... */
		if (data->socket->fam == AF_INET) {
			STATUP(data->nsd, qudp);
		} else if (data->socket->fam == AF_INET6) {
			STATUP(data->nsd, qudp6);
		}

		buffer_skip(q->packet, received);
		buffer_flip(q->packet);
#ifdef USE_DNSTAP
		dt_collector_submit_auth_query(data->nsd, &q->addr, q->addrlen,
			q->tcp, q->packet);
#endif /* USE_DNSTAP */

		/* Process and answer the query... */
		if (server_process_query_udp(data->nsd, q) != QUERY_DISCARDED) {
			if (RCODE(q->packet) == RCODE_OK && !AA(q->packet)) {
				STATUP(data->nsd, nona);
				ZTATUP(data->nsd, q->zone, nona);
			}

#ifdef USE_ZONE_STATS
			if (data->socket->fam == AF_INET) {
				ZTATUP(data->nsd, q->zone, qudp);
			} else if (data->socket->fam == AF_INET6) {
				ZTATUP(data->nsd, q->zone, qudp6);
			}
#endif

			/* Add EDNS0 and TSIG info if necessary.  */
			query_add_optional(q, data->nsd);

			buffer_flip(q->packet);

			sent = sendto(fd,
				      buffer_begin(q->packet),
				      buffer_remaining(q->packet),
				      0,
				      (struct sockaddr *) &q->addr,
				      q->addrlen);
			if (sent == -1) {
				const char* es = strerror(errno);
				char a[48];
				addr2str(&q->addr, a, sizeof(a));
				log_msg(LOG_ERR, "sendto %s failed: %s", a, es);
				STATUP(data->nsd, txerr);
				ZTATUP(data->nsd, q->zone, txerr);
			} else if ((size_t) sent != buffer_remaining(q->packet)) {
				log_msg(LOG_ERR, "sent %d in place of %d bytes", sent, (int) buffer_remaining(q->packet));
			} else {
#ifdef BIND8_STATS
				/* Account the rcode & TC... */
				STATUP2(data->nsd, rcode, RCODE(q->packet));
				ZTATUP2(data->nsd, q->zone, rcode, RCODE(q->packet));
				if (TC(q->packet)) {
					STATUP(data->nsd, truncated);
					ZTATUP(data->nsd, q->zone, truncated);
				}
#endif /* BIND8_STATS */
#ifdef USE_DNSTAP
				dt_collector_submit_auth_response(data->nsd,
					&q->addr, q->addrlen, q->tcp,
					q->packet, q->zone);
#endif /* USE_DNSTAP */
			}
		} else {
			STATUP(data->nsd, dropped);
			ZTATUP(data->nsd, q->zone, dropped);
		}
#ifndef NONBLOCKING_IS_BROKEN
#ifdef HAVE_RECVMMSG
		query_reset(queries[i], UDP_MAX_MESSAGE_LEN, 0);
		iovecs[i].iov_len = buffer_remaining(queries[i]->packet);
		msgs[i].msg_hdr.msg_namelen = queries[i]->addrlen;
#endif
	}
#endif
}
#endif /* defined(HAVE_SENDMMSG) && !defined(NONBLOCKING_IS_BROKEN) && defined(HAVE_RECVMMSG) */

/*
 * Setup an event for the tcp handler.
 */
static void
tcp_handler_setup_event(struct tcp_handler_data* data, void (*fn)(int, short, void *),
       int fd, short event)
{
	struct timeval timeout;
	struct event_base* ev_base;

	timeout.tv_sec = data->nsd->tcp_timeout;
	timeout.tv_usec = 0L;

	ev_base = data->event.ev_base;
	event_del(&data->event);
	memset(&data->event, 0, sizeof(data->event));
	event_set(&data->event, fd, event, fn, data);
	if(event_base_set(ev_base, &data->event) != 0)
		log_msg(LOG_ERR, "event base set failed");
	if(event_add(&data->event, &timeout) != 0)
		log_msg(LOG_ERR, "event add failed");
}

static void
cleanup_tcp_handler(struct tcp_handler_data* data)
{
	event_del(&data->event);
#ifdef HAVE_SSL
	if(data->tls) {
		SSL_shutdown(data->tls);
		SSL_free(data->tls);
		data->tls = NULL;
	}
#endif
	close(data->event.ev_fd);
	if(data->prev)
		data->prev->next = data->next;
	else	tcp_active_list = data->next;
	if(data->next)
		data->next->prev = data->prev;

	/*
	 * Enable the TCP accept handlers when the current number of
	 * TCP connections is about to drop below the maximum number
	 * of TCP connections.
	 */
	if (slowaccept || data->nsd->current_tcp_count == data->nsd->maximum_tcp_count) {
		configure_handler_event_types(EV_READ|EV_PERSIST);
		if(slowaccept) {
			event_del(&slowaccept_event);
			slowaccept = 0;
		}
	}
	--data->nsd->current_tcp_count;
	assert(data->nsd->current_tcp_count >= 0);

	region_destroy(data->region);
}

static void
handle_tcp_reading(int fd, short event, void* arg)
{
	struct tcp_handler_data *data = (struct tcp_handler_data *) arg;
	ssize_t received;
	struct event_base* ev_base;
	struct timeval timeout;

	if ((event & EV_TIMEOUT)) {
		/* Connection timed out.  */
		cleanup_tcp_handler(data);
		return;
	}

	if (data->nsd->tcp_query_count > 0 &&
		data->query_count >= data->nsd->tcp_query_count) {
		/* No more queries allowed on this tcp connection. */
		cleanup_tcp_handler(data);
		return;
	}

	assert((event & EV_READ));

	if (data->bytes_transmitted == 0) {
		query_reset(data->query, TCP_MAX_MESSAGE_LEN, 1);
	}

	/*
	 * Check if we received the leading packet length bytes yet.
	 */
	if (data->bytes_transmitted < sizeof(uint16_t)) {
		received = read(fd,
				(char *) &data->query->tcplen
				+ data->bytes_transmitted,
				sizeof(uint16_t) - data->bytes_transmitted);
		if (received == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				/*
				 * Read would block, wait until more
				 * data is available.
				 */
				return;
			} else {
				char buf[48];
				addr2str(&data->query->addr, buf, sizeof(buf));
#ifdef ECONNRESET
				if (verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
				log_msg(LOG_ERR, "failed reading from %s tcp: %s", buf, strerror(errno));
				cleanup_tcp_handler(data);
				return;
			}
		} else if (received == 0) {
			/* EOF */
			cleanup_tcp_handler(data);
			return;
		}

		data->bytes_transmitted += received;
		if (data->bytes_transmitted < sizeof(uint16_t)) {
			/*
			 * Not done with the tcplen yet, wait for more
			 * data to become available.
			 */
			return;
		}

		assert(data->bytes_transmitted == sizeof(uint16_t));

		data->query->tcplen = ntohs(data->query->tcplen);

		/*
		 * Minimum query size is:
		 *
		 *     Size of the header (12)
		 *   + Root domain name   (1)
		 *   + Query class        (2)
		 *   + Query type         (2)
		 */
		if (data->query->tcplen < QHEADERSZ + 1 + sizeof(uint16_t) + sizeof(uint16_t)) {
			VERBOSITY(2, (LOG_WARNING, "packet too small, dropping tcp connection"));
			cleanup_tcp_handler(data);
			return;
		}

		if (data->query->tcplen > data->query->maxlen) {
			VERBOSITY(2, (LOG_WARNING, "insufficient tcp buffer, dropping connection"));
			cleanup_tcp_handler(data);
			return;
		}

		buffer_set_limit(data->query->packet, data->query->tcplen);
	}

	assert(buffer_remaining(data->query->packet) > 0);

	/* Read the (remaining) query data.  */
	received = read(fd,
			buffer_current(data->query->packet),
			buffer_remaining(data->query->packet));
	if (received == -1) {
		if (errno == EAGAIN || errno == EINTR) {
			/*
			 * Read would block, wait until more data is
			 * available.
			 */
			return;
		} else {
			char buf[48];
			addr2str(&data->query->addr, buf, sizeof(buf));
#ifdef ECONNRESET
			if (verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
			log_msg(LOG_ERR, "failed reading from %s tcp: %s", buf, strerror(errno));
			cleanup_tcp_handler(data);
			return;
		}
	} else if (received == 0) {
		/* EOF */
		cleanup_tcp_handler(data);
		return;
	}

	data->bytes_transmitted += received;
	buffer_skip(data->query->packet, received);
	if (buffer_remaining(data->query->packet) > 0) {
		/*
		 * Message not yet complete, wait for more data to
		 * become available.
		 */
		return;
	}

	assert(buffer_position(data->query->packet) == data->query->tcplen);

	/* Account... */
#ifdef BIND8_STATS
#ifndef INET6
	STATUP(data->nsd, ctcp);
#else
	if (data->query->addr.ss_family == AF_INET) {
		STATUP(data->nsd, ctcp);
	} else if (data->query->addr.ss_family == AF_INET6) {
		STATUP(data->nsd, ctcp6);
	}
#endif
#endif /* BIND8_STATS */

	/* We have a complete query, process it.  */

	/* tcp-query-count: handle query counter ++ */
	data->query_count++;

	buffer_flip(data->query->packet);
#ifdef USE_DNSTAP
	dt_collector_submit_auth_query(data->nsd, &data->query->addr,
		data->query->addrlen, data->query->tcp, data->query->packet);
#endif /* USE_DNSTAP */
	data->query_state = server_process_query(data->nsd, data->query);
	if (data->query_state == QUERY_DISCARDED) {
		/* Drop the packet and the entire connection... */
		STATUP(data->nsd, dropped);
		ZTATUP(data->nsd, data->query->zone, dropped);
		cleanup_tcp_handler(data);
		return;
	}

#ifdef BIND8_STATS
	if (RCODE(data->query->packet) == RCODE_OK
	    && !AA(data->query->packet))
	{
		STATUP(data->nsd, nona);
		ZTATUP(data->nsd, data->query->zone, nona);
	}
#endif /* BIND8_STATS */

#ifdef USE_ZONE_STATS
#ifndef INET6
	ZTATUP(data->nsd, data->query->zone, ctcp);
#else
	if (data->query->addr.ss_family == AF_INET) {
		ZTATUP(data->nsd, data->query->zone, ctcp);
	} else if (data->query->addr.ss_family == AF_INET6) {
		ZTATUP(data->nsd, data->query->zone, ctcp6);
	}
#endif
#endif /* USE_ZONE_STATS */

	query_add_optional(data->query, data->nsd);

	/* Switch to the tcp write handler.  */
	buffer_flip(data->query->packet);
	data->query->tcplen = buffer_remaining(data->query->packet);
#ifdef BIND8_STATS
	/* Account the rcode & TC... */
	STATUP2(data->nsd, rcode, RCODE(data->query->packet));
	ZTATUP2(data->nsd, data->query->zone, rcode, RCODE(data->query->packet));
	if (TC(data->query->packet)) {
		STATUP(data->nsd, truncated);
		ZTATUP(data->nsd, data->query->zone, truncated);
	}
#endif /* BIND8_STATS */
#ifdef USE_DNSTAP
	dt_collector_submit_auth_response(data->nsd, &data->query->addr,
		data->query->addrlen, data->query->tcp, data->query->packet,
		data->query->zone);
#endif /* USE_DNSTAP */
	data->bytes_transmitted = 0;

	timeout.tv_sec = data->tcp_timeout / 1000;
	timeout.tv_usec = (data->tcp_timeout % 1000)*1000;

	ev_base = data->event.ev_base;
	event_del(&data->event);
	memset(&data->event, 0, sizeof(data->event));
	event_set(&data->event, fd, EV_PERSIST | EV_READ | EV_TIMEOUT,
		handle_tcp_reading, data);
	if(event_base_set(ev_base, &data->event) != 0)
		log_msg(LOG_ERR, "event base set tcpr failed");
	if(event_add(&data->event, &timeout) != 0)
		log_msg(LOG_ERR, "event add tcpr failed");
	/* see if we can write the answer right away(usually so,EAGAIN ifnot)*/
	handle_tcp_writing(fd, EV_WRITE, data);
}

static void
handle_tcp_writing(int fd, short event, void* arg)
{
	struct tcp_handler_data *data = (struct tcp_handler_data *) arg;
	ssize_t sent;
	struct query *q = data->query;
	struct timeval timeout;
	struct event_base* ev_base;

	if ((event & EV_TIMEOUT)) {
		/* Connection timed out.  */
		cleanup_tcp_handler(data);
		return;
	}

	assert((event & EV_WRITE));

	if (data->bytes_transmitted < sizeof(q->tcplen)) {
		/* Writing the response packet length.  */
		uint16_t n_tcplen = htons(q->tcplen);
#ifdef HAVE_WRITEV
		struct iovec iov[2];
		iov[0].iov_base = (uint8_t*)&n_tcplen + data->bytes_transmitted;
		iov[0].iov_len = sizeof(n_tcplen) - data->bytes_transmitted; 
		iov[1].iov_base = buffer_begin(q->packet);
		iov[1].iov_len = buffer_limit(q->packet);
		sent = writev(fd, iov, 2);
#else /* HAVE_WRITEV */
		sent = write(fd,
			     (const char *) &n_tcplen + data->bytes_transmitted,
			     sizeof(n_tcplen) - data->bytes_transmitted);
#endif /* HAVE_WRITEV */
		if (sent == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				/*
				 * Write would block, wait until
				 * socket becomes writable again.
				 */
				return;
			} else {
#ifdef ECONNRESET
				if(verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
#ifdef EPIPE
				  if(verbosity >= 2 || errno != EPIPE)
#endif /* EPIPE 'broken pipe' */
				    log_msg(LOG_ERR, "failed writing to tcp: %s", strerror(errno));
				cleanup_tcp_handler(data);
				return;
			}
		}

		data->bytes_transmitted += sent;
		if (data->bytes_transmitted < sizeof(q->tcplen)) {
			/*
			 * Writing not complete, wait until socket
			 * becomes writable again.
			 */
			return;
		}

#ifdef HAVE_WRITEV
		sent -= sizeof(n_tcplen);
		/* handle potential 'packet done' code */
		goto packet_could_be_done;
#endif
 	}
 
	sent = write(fd,
		     buffer_current(q->packet),
		     buffer_remaining(q->packet));
	if (sent == -1) {
		if (errno == EAGAIN || errno == EINTR) {
			/*
			 * Write would block, wait until
			 * socket becomes writable again.
			 */
			return;
		} else {
#ifdef ECONNRESET
			if(verbosity >= 2 || errno != ECONNRESET)
#endif /* ECONNRESET */
#ifdef EPIPE
				  if(verbosity >= 2 || errno != EPIPE)
#endif /* EPIPE 'broken pipe' */
			log_msg(LOG_ERR, "failed writing to tcp: %s", strerror(errno));
			cleanup_tcp_handler(data);
			return;
		}
	}

	data->bytes_transmitted += sent;
#ifdef HAVE_WRITEV
  packet_could_be_done:
#endif
	buffer_skip(q->packet, sent);
	if (data->bytes_transmitted < q->tcplen + sizeof(q->tcplen)) {
		/*
		 * Still more data to write when socket becomes
		 * writable again.
		 */
		return;
	}

	assert(data->bytes_transmitted == q->tcplen + sizeof(q->tcplen));

	if (data->query_state == QUERY_IN_AXFR) {
		/* Continue processing AXFR and writing back results.  */
		buffer_clear(q->packet);
		data->query_state = query_axfr(data->nsd, q);
		if (data->query_state != QUERY_PROCESSED) {
			query_add_optional(data->query, data->nsd);

			/* Reset data. */
			buffer_flip(q->packet);
			q->tcplen = buffer_remaining(q->packet);
			data->bytes_transmitted = 0;
			/* Reset timeout.  */
			timeout.tv_sec = data->tcp_timeout / 1000;
			timeout.tv_usec = (data->tcp_timeout % 1000)*1000;
			ev_base = data->event.ev_base;
			event_del(&data->event);
			memset(&data->event, 0, sizeof(data->event));
			event_set(&data->event, fd, EV_PERSIST | EV_WRITE | EV_TIMEOUT,
				handle_tcp_writing, data);
			if(event_base_set(ev_base, &data->event) != 0)
				log_msg(LOG_ERR, "event base set tcpw failed");
			if(event_add(&data->event, &timeout) != 0)
				log_msg(LOG_ERR, "event add tcpw failed");

			/*
			 * Write data if/when the socket is writable
			 * again.
			 */
			return;
		}
	}

	/*
	 * Done sending, wait for the next request to arrive on the
	 * TCP socket by installing the TCP read handler.
	 */
	if (data->nsd->tcp_query_count > 0 &&
		data->query_count >= data->nsd->tcp_query_count) {

		(void) shutdown(fd, SHUT_WR);
	}

	data->bytes_transmitted = 0;

	timeout.tv_sec = data->tcp_timeout / 1000;
	timeout.tv_usec = (data->tcp_timeout % 1000)*1000;
	ev_base = data->event.ev_base;
	event_del(&data->event);
	memset(&data->event, 0, sizeof(data->event));
	event_set(&data->event, fd, EV_PERSIST | EV_READ | EV_TIMEOUT,
		handle_tcp_reading, data);
	if(event_base_set(ev_base, &data->event) != 0)
		log_msg(LOG_ERR, "event base set tcpw failed");
	if(event_add(&data->event, &timeout) != 0)
		log_msg(LOG_ERR, "event add tcpw failed");
}

#ifdef HAVE_SSL
/** create SSL object and associate fd */
static SSL*
incoming_ssl_fd(SSL_CTX* ctx, int fd)
{
	SSL* ssl = SSL_new((SSL_CTX*)ctx);
	if(!ssl) {
		log_crypto_err("could not SSL_new");
		return NULL;
	}
	SSL_set_accept_state(ssl);
	(void)SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	if(!SSL_set_fd(ssl, fd)) {
		log_crypto_err("could not SSL_set_fd");
		SSL_free(ssl);
		return NULL;
	}
	return ssl;
}

/** TLS handshake to upgrade TCP connection */
static int
tls_handshake(struct tcp_handler_data* data, int fd, int writing)
{
	int r;
	if(data->shake_state == tls_hs_read_event) {
		/* read condition satisfied back to writing */
		tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST|EV_TIMEOUT|EV_WRITE);
		data->shake_state = tls_hs_none;
		return 1;
	}
	if(data->shake_state == tls_hs_write_event) {
		/* write condition satisfied back to reading */
		tcp_handler_setup_event(data, handle_tls_reading, fd, EV_PERSIST|EV_TIMEOUT|EV_READ);
		data->shake_state = tls_hs_none;
		return 1;
	}

	/* (continue to) setup the TLS connection */
	ERR_clear_error();
	r = SSL_do_handshake(data->tls);

	if(r != 1) {
		int want = SSL_get_error(data->tls, r);
		if(want == SSL_ERROR_WANT_READ) {
			if(data->shake_state == tls_hs_read) {
				/* try again later */
				return 1;
			}
			data->shake_state = tls_hs_read;
			/* switch back to reading mode */
			tcp_handler_setup_event(data, handle_tls_reading, fd, EV_PERSIST|EV_TIMEOUT|EV_READ);
			return 1;
		} else if(want == SSL_ERROR_WANT_WRITE) {
			if(data->shake_state == tls_hs_write) {
				/* try again later */
				return 1;
			}
			data->shake_state = tls_hs_write;
			/* switch back to writing mode */
			tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST|EV_TIMEOUT|EV_WRITE);
			return 1;
		} else {
			if(r == 0)
				VERBOSITY(3, (LOG_ERR, "TLS handshake: connection closed prematurely"));
			cleanup_tcp_handler(data);
			VERBOSITY(3, (LOG_ERR, "TLS handshake failed"));
			return 0;
		}
	}

	/* Use to log successful upgrade for testing - could be removed*/
	VERBOSITY(3, (LOG_INFO, "TLS handshake succeeded."));
	/* set back to the event we need to have when reading (or writing) */
	if(data->shake_state == tls_hs_read && writing) {
		tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST|EV_TIMEOUT|EV_WRITE);
	} else if(data->shake_state == tls_hs_write && !writing) {
		tcp_handler_setup_event(data, handle_tls_reading, fd, EV_PERSIST|EV_TIMEOUT|EV_READ);
	}
	data->shake_state = tls_hs_none;
	return 1;
}

/** handle TLS reading of incoming query */
static void
handle_tls_reading(int fd, short event, void* arg)
{
	struct tcp_handler_data *data = (struct tcp_handler_data *) arg;
	ssize_t received;

	if ((event & EV_TIMEOUT)) {
		/* Connection timed out.  */
		cleanup_tcp_handler(data);
		return;
	}

	if (data->nsd->tcp_query_count > 0 &&
	    data->query_count >= data->nsd->tcp_query_count) {
		/* No more queries allowed on this tcp connection. */
		cleanup_tcp_handler(data);
		return;
	}

	assert((event & EV_READ));

	if (data->bytes_transmitted == 0) {
		query_reset(data->query, TCP_MAX_MESSAGE_LEN, 1);
	}

	if(data->shake_state != tls_hs_none) {
		if(!tls_handshake(data, fd, 0))
			return;
		if(data->shake_state != tls_hs_none)
			return;
	}

	/*
	 * Check if we received the leading packet length bytes yet.
	 */
	if(data->bytes_transmitted < sizeof(uint16_t)) {
		ERR_clear_error();
		if((received=SSL_read(data->tls, (char *) &data->query->tcplen
		    + data->bytes_transmitted,
		    sizeof(uint16_t) - data->bytes_transmitted)) <= 0) {
			int want = SSL_get_error(data->tls, received);
			if(want == SSL_ERROR_ZERO_RETURN) {
				cleanup_tcp_handler(data);
				return; /* shutdown, closed */
			} else if(want == SSL_ERROR_WANT_READ) {
				/* wants to be called again */
				return;
			}
			else if(want == SSL_ERROR_WANT_WRITE) {
				/* switch to writing */
				data->shake_state = tls_hs_write_event;
				tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST | EV_WRITE | EV_TIMEOUT);
				return;
			}
			cleanup_tcp_handler(data);
			log_crypto_err("could not SSL_read");
			return;
		}

		data->bytes_transmitted += received;
		if (data->bytes_transmitted < sizeof(uint16_t)) {
			/*
			 * Not done with the tcplen yet, wait for more
			 * data to become available.
			 */
			return;
		}

		assert(data->bytes_transmitted == sizeof(uint16_t));

		data->query->tcplen = ntohs(data->query->tcplen);

		/*
		 * Minimum query size is:
		 *
		 *     Size of the header (12)
		 *   + Root domain name   (1)
		 *   + Query class        (2)
		 *   + Query type         (2)
		 */
		if (data->query->tcplen < QHEADERSZ + 1 + sizeof(uint16_t) + sizeof(uint16_t)) {
			VERBOSITY(2, (LOG_WARNING, "packet too small, dropping tcp connection"));
			cleanup_tcp_handler(data);
			return;
		}

		if (data->query->tcplen > data->query->maxlen) {
			VERBOSITY(2, (LOG_WARNING, "insufficient tcp buffer, dropping connection"));
			cleanup_tcp_handler(data);
			return;
		}

		buffer_set_limit(data->query->packet, data->query->tcplen);
	}

	assert(buffer_remaining(data->query->packet) > 0);

	/* Read the (remaining) query data.  */
	ERR_clear_error();
	received = SSL_read(data->tls, (void*)buffer_current(data->query->packet),
			    (int)buffer_remaining(data->query->packet));
	if(received <= 0) {
		int want = SSL_get_error(data->tls, received);
		if(want == SSL_ERROR_ZERO_RETURN) {
			cleanup_tcp_handler(data);
			return; /* shutdown, closed */
		} else if(want == SSL_ERROR_WANT_READ) {
			/* wants to be called again */
			return;
		}
		else if(want == SSL_ERROR_WANT_WRITE) {
			/* switch back writing */
			data->shake_state = tls_hs_write_event;
			tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST | EV_WRITE | EV_TIMEOUT);
			return;
		}
		cleanup_tcp_handler(data);
		log_crypto_err("could not SSL_read");
		return;
	}

	data->bytes_transmitted += received;
	buffer_skip(data->query->packet, received);
	if (buffer_remaining(data->query->packet) > 0) {
		/*
		 * Message not yet complete, wait for more data to
		 * become available.
		 */
		return;
	}

	assert(buffer_position(data->query->packet) == data->query->tcplen);

	/* Account... */
#ifndef INET6
	STATUP(data->nsd, ctls);
#else
	if (data->query->addr.ss_family == AF_INET) {
		STATUP(data->nsd, ctls);
	} else if (data->query->addr.ss_family == AF_INET6) {
		STATUP(data->nsd, ctls6);
	}
#endif

	/* We have a complete query, process it.  */

	/* tcp-query-count: handle query counter ++ */
	data->query_count++;

	buffer_flip(data->query->packet);
#ifdef USE_DNSTAP
	dt_collector_submit_auth_query(data->nsd, &data->query->addr,
		data->query->addrlen, data->query->tcp, data->query->packet);
#endif /* USE_DNSTAP */
	data->query_state = server_process_query(data->nsd, data->query);
	if (data->query_state == QUERY_DISCARDED) {
		/* Drop the packet and the entire connection... */
		STATUP(data->nsd, dropped);
		ZTATUP(data->nsd, data->query->zone, dropped);
		cleanup_tcp_handler(data);
		return;
	}

#ifdef BIND8_STATS
	if (RCODE(data->query->packet) == RCODE_OK
	    && !AA(data->query->packet))
	{
		STATUP(data->nsd, nona);
		ZTATUP(data->nsd, data->query->zone, nona);
	}
#endif /* BIND8_STATS */

#ifdef USE_ZONE_STATS
#ifndef INET6
	ZTATUP(data->nsd, data->query->zone, ctls);
#else
	if (data->query->addr.ss_family == AF_INET) {
		ZTATUP(data->nsd, data->query->zone, ctls);
	} else if (data->query->addr.ss_family == AF_INET6) {
		ZTATUP(data->nsd, data->query->zone, ctls6);
	}
#endif
#endif /* USE_ZONE_STATS */

	query_add_optional(data->query, data->nsd);

	/* Switch to the tcp write handler.  */
	buffer_flip(data->query->packet);
	data->query->tcplen = buffer_remaining(data->query->packet);
#ifdef BIND8_STATS
	/* Account the rcode & TC... */
	STATUP2(data->nsd, rcode, RCODE(data->query->packet));
	ZTATUP2(data->nsd, data->query->zone, rcode, RCODE(data->query->packet));
	if (TC(data->query->packet)) {
		STATUP(data->nsd, truncated);
		ZTATUP(data->nsd, data->query->zone, truncated);
	}
#endif /* BIND8_STATS */
#ifdef USE_DNSTAP
	dt_collector_submit_auth_response(data->nsd, &data->query->addr,
		data->query->addrlen, data->query->tcp, data->query->packet,
		data->query->zone);
#endif /* USE_DNSTAP */
	data->bytes_transmitted = 0;

	tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST | EV_WRITE | EV_TIMEOUT);

	/* see if we can write the answer right away(usually so,EAGAIN ifnot)*/
	handle_tls_writing(fd, EV_WRITE, data);
}

/** handle TLS writing of outgoing response */
static void
handle_tls_writing(int fd, short event, void* arg)
{
	struct tcp_handler_data *data = (struct tcp_handler_data *) arg;
	ssize_t sent;
	struct query *q = data->query;
	/* static variable that holds reassembly buffer used to put the
	 * TCP length in front of the packet, like writev. */
	static buffer_type* global_tls_temp_buffer = NULL;
	buffer_type* write_buffer;

	if ((event & EV_TIMEOUT)) {
		/* Connection timed out.  */
		cleanup_tcp_handler(data);
		return;
	}

	assert((event & EV_WRITE));

	if(data->shake_state != tls_hs_none) {
		if(!tls_handshake(data, fd, 1))
			return;
		if(data->shake_state != tls_hs_none)
			return;
	}

	(void)SSL_set_mode(data->tls, SSL_MODE_ENABLE_PARTIAL_WRITE);

	/* If we are writing the start of a message, we must include the length
	 * this is done with a copy into write_buffer. */
	write_buffer = NULL;
	if (data->bytes_transmitted == 0) {
		if(!global_tls_temp_buffer) {
			/* gets deallocated when nsd shuts down from
			 * nsd.region */
			global_tls_temp_buffer = buffer_create(nsd.region,
				QIOBUFSZ + sizeof(q->tcplen));
			if (!global_tls_temp_buffer) {
				return;
			}
		}
		write_buffer = global_tls_temp_buffer;
		buffer_clear(write_buffer);
		buffer_write_u16(write_buffer, q->tcplen);
		buffer_write(write_buffer, buffer_current(q->packet),
			(int)buffer_remaining(q->packet));
		buffer_flip(write_buffer);
	} else {
		write_buffer = q->packet;
	}

	/* Write the response */
	ERR_clear_error();
	sent = SSL_write(data->tls, buffer_current(write_buffer), buffer_remaining(write_buffer));
	if(sent <= 0) {
		int want = SSL_get_error(data->tls, sent);
		if(want == SSL_ERROR_ZERO_RETURN) {
			cleanup_tcp_handler(data);
			/* closed */
		} else if(want == SSL_ERROR_WANT_READ) {
			/* switch back to reading */
			data->shake_state = tls_hs_read_event;
			tcp_handler_setup_event(data, handle_tls_reading, fd, EV_PERSIST | EV_READ | EV_TIMEOUT);
		} else if(want != SSL_ERROR_WANT_WRITE) {
			cleanup_tcp_handler(data);
			log_crypto_err("could not SSL_write");
		}
		return;
	}

	buffer_skip(write_buffer, sent);
	if(buffer_remaining(write_buffer) != 0) {
		/* If not all sent, sync up the real buffer if it wasn't used.*/
		if (data->bytes_transmitted == 0 && (ssize_t)sent > (ssize_t)sizeof(q->tcplen)) {
			buffer_skip(q->packet, (ssize_t)sent - (ssize_t)sizeof(q->tcplen));
		}
	}

	data->bytes_transmitted += sent;
	if (data->bytes_transmitted < q->tcplen + sizeof(q->tcplen)) {
		/*
		 * Still more data to write when socket becomes
		 * writable again.
		 */
		return;
	}

	assert(data->bytes_transmitted == q->tcplen + sizeof(q->tcplen));

	if (data->query_state == QUERY_IN_AXFR) {
		/* Continue processing AXFR and writing back results.  */
		buffer_clear(q->packet);
		data->query_state = query_axfr(data->nsd, q);
		if (data->query_state != QUERY_PROCESSED) {
			query_add_optional(data->query, data->nsd);

			/* Reset data. */
			buffer_flip(q->packet);
			q->tcplen = buffer_remaining(q->packet);
			data->bytes_transmitted = 0;
			/* Reset to writing mode.  */
			tcp_handler_setup_event(data, handle_tls_writing, fd, EV_PERSIST | EV_WRITE | EV_TIMEOUT);

			/*
			 * Write data if/when the socket is writable
			 * again.
			 */
			return;
		}
	}

	/*
	 * Done sending, wait for the next request to arrive on the
	 * TCP socket by installing the TCP read handler.
	 */
	if (data->nsd->tcp_query_count > 0 &&
		data->query_count >= data->nsd->tcp_query_count) {

		(void) shutdown(fd, SHUT_WR);
	}

	data->bytes_transmitted = 0;

	tcp_handler_setup_event(data, handle_tls_reading, fd, EV_PERSIST | EV_READ | EV_TIMEOUT);
}
#endif

static void
handle_slowaccept_timeout(int ATTR_UNUSED(fd), short ATTR_UNUSED(event),
	void* ATTR_UNUSED(arg))
{
	if(slowaccept) {
		configure_handler_event_types(EV_PERSIST | EV_READ);
		slowaccept = 0;
	}
}

#ifndef HAVE_ACCEPT4
static int nsd_accept4(
	int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	int s = accept(sockfd, addr, addrlen);
	if (s != -1) {
		if (fcntl(s, F_SETFL, O_NONBLOCK) == -1) {
			log_msg(LOG_ERR, "fcntl failed: %s", strerror(errno));
			close(s);
			s = -1;
			errno=EINTR; /* stop error printout as error in accept4
				by setting this errno, it omits printout, in
				later code that calls nsd_accept4 */
		}
	}
	return s;
}
#else
#define nsd_accept4 accept4
#endif /* HAVE_ACCEPT4 */

/*
 * Handle an incoming TCP connection.  The connection is accepted and
 * a new TCP reader event handler is added.  The TCP handler
 * is responsible for cleanup when the connection is closed.
 */
static void
handle_tcp_accept(int fd, short event, void* arg)
{
	struct tcp_accept_handler_data *data
		= (struct tcp_accept_handler_data *) arg;
	int s;
	int reject = 0;
	struct tcp_handler_data *tcp_data;
	region_type *tcp_region;
#ifdef INET6
	struct sockaddr_storage addr;
#else
	struct sockaddr_in addr;
#endif
	socklen_t addrlen;
	struct timeval timeout;

	if (!(event & EV_READ)) {
		return;
	}

	if (data->nsd->current_tcp_count >= data->nsd->maximum_tcp_count) {
		reject = data->nsd->options->tcp_reject_overflow;
		if (!reject) {
			return;
		}
	}

	/* Accept it... */
	addrlen = sizeof(addr);
	s = nsd_accept4(fd, (struct sockaddr *) &addr, &addrlen, SOCK_NONBLOCK);
	if (s == -1) {
		/**
		 * EMFILE and ENFILE is a signal that the limit of open
		 * file descriptors has been reached. Pause accept().
		 * EINTR is a signal interrupt. The others are various OS ways
		 * of saying that the client has closed the connection.
		 */
		if (errno == EMFILE || errno == ENFILE) {
			if (!slowaccept) {
				/* disable accept events */
				struct timeval tv;
				configure_handler_event_types(0);
				tv.tv_sec = SLOW_ACCEPT_TIMEOUT;
				tv.tv_usec = 0L;
				memset(&slowaccept_event, 0,
					sizeof(slowaccept_event));
				event_set(&slowaccept_event, -1, EV_TIMEOUT,
					handle_slowaccept_timeout, NULL);
				(void)event_base_set(data->event.ev_base,
					&slowaccept_event);
				(void)event_add(&slowaccept_event, &tv);
				slowaccept = 1;
				/* We don't want to spam the logs here */
			}
		} else if (errno != EINTR
			&& errno != EWOULDBLOCK
#ifdef ECONNABORTED
			&& errno != ECONNABORTED
#endif /* ECONNABORTED */
#ifdef EPROTO
			&& errno != EPROTO
#endif /* EPROTO */
			) {
			log_msg(LOG_ERR, "accept failed: %s", strerror(errno));
		}
		return;
	}

	if (reject) {
		shutdown(s, SHUT_RDWR);
		close(s);
		return;
	}

	/*
	 * This region is deallocated when the TCP connection is
	 * closed by the TCP handler.
	 */
	tcp_region = region_create(xalloc, free);
	tcp_data = (struct tcp_handler_data *) region_alloc(
		tcp_region, sizeof(struct tcp_handler_data));
	tcp_data->region = tcp_region;
	tcp_data->query = query_create(tcp_region, compressed_dname_offsets,
		compression_table_size, compressed_dnames);
	tcp_data->nsd = data->nsd;
	tcp_data->query_count = 0;
#ifdef HAVE_SSL
	tcp_data->shake_state = tls_hs_none;
	tcp_data->tls = NULL;
#endif
	tcp_data->prev = NULL;
	tcp_data->next = NULL;

	tcp_data->query_state = QUERY_PROCESSED;
	tcp_data->bytes_transmitted = 0;
	memcpy(&tcp_data->query->addr, &addr, addrlen);
	tcp_data->query->addrlen = addrlen;

	tcp_data->tcp_timeout = data->nsd->tcp_timeout * 1000;
	if (data->nsd->current_tcp_count > data->nsd->maximum_tcp_count/2) {
		/* very busy, give smaller timeout */
		tcp_data->tcp_timeout = 200;
	}
	memset(&tcp_data->event, 0, sizeof(tcp_data->event));
	timeout.tv_sec = tcp_data->tcp_timeout / 1000;
	timeout.tv_usec = (tcp_data->tcp_timeout % 1000)*1000;

#ifdef HAVE_SSL
	if (data->tls_accept) {
		tcp_data->tls = incoming_ssl_fd(tcp_data->nsd->tls_ctx, s);
		if(!tcp_data->tls) {
			close(s);
			return;
		}
		tcp_data->shake_state = tls_hs_read;
		memset(&tcp_data->event, 0, sizeof(tcp_data->event));
		event_set(&tcp_data->event, s, EV_PERSIST | EV_READ | EV_TIMEOUT,
			  handle_tls_reading, tcp_data);
	} else {
#endif
		memset(&tcp_data->event, 0, sizeof(tcp_data->event));
		event_set(&tcp_data->event, s, EV_PERSIST | EV_READ | EV_TIMEOUT,
			  handle_tcp_reading, tcp_data);
#ifdef HAVE_SSL
	}
#endif
	if(event_base_set(data->event.ev_base, &tcp_data->event) != 0) {
		log_msg(LOG_ERR, "cannot set tcp event base");
		close(s);
		region_destroy(tcp_region);
		return;
	}
	if(event_add(&tcp_data->event, &timeout) != 0) {
		log_msg(LOG_ERR, "cannot add tcp to event base");
		close(s);
		region_destroy(tcp_region);
		return;
	}
	if(tcp_active_list) {
		tcp_active_list->prev = tcp_data;
		tcp_data->next = tcp_active_list;
	}
	tcp_active_list = tcp_data;

	/*
	 * Keep track of the total number of TCP handlers installed so
	 * we can stop accepting connections when the maximum number
	 * of simultaneous TCP connections is reached.
	 *
	 * If tcp-reject-overflow is enabled, however, then we do not
	 * change the handler event type; we keep it as-is and accept
	 * overflow TCP connections only so that we can forcibly kill
	 * them off.
	 */
	++data->nsd->current_tcp_count;
	if (!data->nsd->options->tcp_reject_overflow &&
	     data->nsd->current_tcp_count == data->nsd->maximum_tcp_count)
	{
		configure_handler_event_types(0);
	}
}

static void
send_children_command(struct nsd* nsd, sig_atomic_t command, int timeout)
{
	size_t i;
	assert(nsd->server_kind == NSD_SERVER_MAIN && nsd->this_child == 0);
	for (i = 0; i < nsd->child_count; ++i) {
		if (nsd->children[i].pid > 0 && nsd->children[i].child_fd != -1) {
			if (write(nsd->children[i].child_fd,
				&command,
				sizeof(command)) == -1)
			{
				if(errno != EAGAIN && errno != EINTR)
					log_msg(LOG_ERR, "problems sending command %d to server %d: %s",
					(int) command,
					(int) nsd->children[i].pid,
					strerror(errno));
			} else if (timeout > 0) {
				(void)block_read(NULL,
					nsd->children[i].child_fd,
					&command, sizeof(command), timeout);
			}
			fsync(nsd->children[i].child_fd);
			close(nsd->children[i].child_fd);
			nsd->children[i].child_fd = -1;
		}
	}
}

static void
send_children_quit(struct nsd* nsd)
{
	DEBUG(DEBUG_IPC, 1, (LOG_INFO, "send children quit"));
	send_children_command(nsd, NSD_QUIT, 0);
}

static void
send_children_quit_and_wait(struct nsd* nsd)
{
	DEBUG(DEBUG_IPC, 1, (LOG_INFO, "send children quit and wait"));
	send_children_command(nsd, NSD_QUIT_CHILD, 3);
}

#ifdef BIND8_STATS
static void
set_children_stats(struct nsd* nsd)
{
	size_t i;
	assert(nsd->server_kind == NSD_SERVER_MAIN && nsd->this_child == 0);
	DEBUG(DEBUG_IPC, 1, (LOG_INFO, "parent set stats to send to children"));
	for (i = 0; i < nsd->child_count; ++i) {
		nsd->children[i].need_to_send_STATS = 1;
		nsd->children[i].handler->event_types |= NETIO_EVENT_WRITE;
	}
}
#endif /* BIND8_STATS */

static void
configure_handler_event_types(short event_types)
{
	size_t i;

	for (i = 0; i < tcp_accept_handler_count; ++i) {
		struct event* handler = &tcp_accept_handlers[i].event;
		if(event_types) {
			/* reassign */
			int fd = handler->ev_fd;
			struct event_base* base = handler->ev_base;
			if(tcp_accept_handlers[i].event_added)
				event_del(handler);
			memset(handler, 0, sizeof(*handler));
			event_set(handler, fd, event_types,
				handle_tcp_accept, &tcp_accept_handlers[i]);
			if(event_base_set(base, handler) != 0)
				log_msg(LOG_ERR, "conhand: cannot event_base");
			if(event_add(handler, NULL) != 0)
				log_msg(LOG_ERR, "conhand: cannot event_add");
			tcp_accept_handlers[i].event_added = 1;
		} else {
			/* remove */
			if(tcp_accept_handlers[i].event_added) {
				event_del(handler);
				tcp_accept_handlers[i].event_added = 0;
			}
		}
	}
}
