/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>
#include <sched.h>
#include <time.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libxio.h"
#include "xio_msg.h"


#define XIO_DEF_ADDRESS		"127.0.0.1"
#define XIO_DEF_PORT		2061
#define XIO_DEF_HEADER_SIZE	32
#define XIO_DEF_DATA_SIZE	32
#define PRINT_COUNTER		4000000
#define XIO_DEF_CPU		0
#define XIO_TEST_VERSION	"1.0.0"
#define XIO_READ_BUF_LEN	(1024*1024)

#define MAX_POOL_SIZE		2048
#define USECS_IN_SEC		1000000
#define NSECS_IN_USEC		1000
#define ONE_MB			(1 << 20)


struct xio_test_config {
	char		server_addr[32];
	uint16_t	server_port;
	uint16_t	cpu;
	uint32_t	hdr_len;
	uint32_t	data_len;
};


/*---------------------------------------------------------------------------*/
/* globals								     */
/*---------------------------------------------------------------------------*/
static struct msg_pool		*pool;
static struct xio_context	*ctx;
static struct xio_connection	*conn;
static uint64_t			print_counter;

static struct xio_test_config  test_config = {
	XIO_DEF_ADDRESS,
	XIO_DEF_PORT,
	XIO_DEF_CPU,
	XIO_DEF_HEADER_SIZE,
	XIO_DEF_DATA_SIZE
};

/**
 * Convert a timespec to a microsecond value.
 * @e NOTE: There is a loss of precision in the conversion.
 *
 * @param time_spec The timespec to convert.
 * @return The number of microseconds specified by the timespec.
 */
static inline
unsigned long long timespec_to_usecs(struct timespec *time_spec)
{
	unsigned long long retval = 0;

	retval  = time_spec->tv_sec * USECS_IN_SEC;
	retval += time_spec->tv_nsec / NSECS_IN_USEC;

	return retval;
}

static inline unsigned long long get_cpu_usecs()
{
	struct timespec ts = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &ts);

	return  timespec_to_usecs(&ts);
}

/*
 * Set CPU affinity to one core.
 */
void set_cpu_affinity(int cpu)
{
	cpu_set_t coremask;		/* core affinity mask */

	CPU_ZERO(&coremask);
	CPU_SET(cpu, &coremask);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &coremask) != 0)
		fprintf(stderr, "Unable to set affinity. %m\n");
}

/*---------------------------------------------------------------------------*/
/* get_ip								     */
/*---------------------------------------------------------------------------*/
static inline char *get_ip(const struct sockaddr *ip)
{
	if (ip->sa_family == AF_INET) {
		static char addr[INET_ADDRSTRLEN];
		struct sockaddr_in *v4 = (struct sockaddr_in *)ip;
		return (char *)inet_ntop(AF_INET, &(v4->sin_addr),
					 addr, INET_ADDRSTRLEN);
	}
	if (ip->sa_family == AF_INET6) {
		static char addr[INET6_ADDRSTRLEN];
		struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ip;
		return (char *)inet_ntop(AF_INET6, &(v6->sin6_addr),
					 addr, INET6_ADDRSTRLEN);
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* get_port								     */
/*---------------------------------------------------------------------------*/
static inline uint16_t get_port(const struct sockaddr *ip)
{
	if (ip->sa_family == AF_INET) {
		struct sockaddr_in *v4 = (struct sockaddr_in *)ip;
		return ntohs(v4->sin_port);
	}
	if (ip->sa_family == AF_INET6) {
		struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ip;
		return ntohs(v6->sin6_port);
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* get_time								     */
/*---------------------------------------------------------------------------*/
static void get_time(char *time, int len)
{
	struct timeval tv;
	struct tm      t;
	int	       n;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &t);
	/* Format the date and time,
	   down to a single second. */
	n = snprintf(time, len,
		  "%04d/%02d/%02d-%02d:%02d:%02d.%05ld",
		   t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		   t.tm_hour, t.tm_min, t.tm_sec, tv.tv_usec);
	time[n] = 0;
}

/*---------------------------------------------------------------------------*/
/* process_response							     */
/*---------------------------------------------------------------------------*/
static void process_response(struct xio_msg *rsp)
{
	static uint64_t cnt = 0;
	static int first_time = 1;
	static uint64_t start_time;
	static size_t	txlen, rxlen;


	if (first_time) {
		size_t	data_len = 0;
		int	i;


		for (i = 0; i < rsp->out.data_iovlen; i++)
			data_len += rsp->out.data_iov[i].iov_len;

		txlen = rsp->out.header.iov_len + data_len;

		data_len = 0;
		for (i = 0; i < rsp->in.data_iovlen; i++)
			data_len += rsp->in.data_iov[i].iov_len;

		rxlen = rsp->in.header.iov_len + data_len;

		start_time = get_cpu_usecs();
		first_time = 0;

		data_len = txlen > rxlen ? txlen : rxlen;
		data_len = data_len/1024;
		print_counter = (data_len ?
				 PRINT_COUNTER/data_len : PRINT_COUNTER);
	}

	if (++cnt == print_counter) {
		char		timeb[40];

		uint64_t delta = get_cpu_usecs() - start_time;
		uint64_t pps = (cnt*USECS_IN_SEC)/delta;

		double txbw = (1.0*pps*txlen/ONE_MB);
		double rxbw = (1.0*pps*rxlen/ONE_MB);

		printf("transactions per second: %"PRIu64", bandwidth: " \
		       "TX %.2f MB/s, RX: %.2f MB/s, length: TX: %zd B, RX: %zd B\n",
		       pps, txbw, rxbw, txlen, rxlen);
		get_time(timeb, 40);
		printf("**** [%s] - response complete [%"PRIu64"] %s - %s\n",
		       timeb, (rsp->request->sn + 1),
		       (char *)rsp->in.header.iov_base,
		       (char *)rsp->in.data_iov[0].iov_base);
		cnt = 0;
		start_time = get_cpu_usecs();
	}
}

/*---------------------------------------------------------------------------*/
/* process_re							     */
/*---------------------------------------------------------------------------*/
static void process_request(struct xio_msg *req)
{
	static int cnt = 0;

	if (req == NULL) {
		cnt = 0;
		return;
	}

	if (++cnt == print_counter) {
		printf("**** request complete [%"PRIu64"] %s - %s [%zd]\n",
		       (req->sn+1),
		       (char *)req->in.header.iov_base,
		       (char *)req->in.data_iov[0].iov_base,
		       req->in.data_iov[0].iov_len);
		cnt = 0;
	}
}

/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
		struct xio_session_event_data *event_data,
		void *cb_prv_data)
{
	printf("session event: %s. session:%p, connection:%p, reason: %s\n",
	       xio_session_event_str(event_data->event),
	       session, event_data->conn,
	       xio_strerror(event_data->reason));

	switch (event_data->event) {
	case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
		xio_connection_destroy(event_data->conn);
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		process_request(NULL);
		xio_session_destroy(session);
		break;
	default:
		break;
	};

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_new_session							     */
/*---------------------------------------------------------------------------*/
static int on_new_session(struct xio_session *session,
			struct xio_new_session_req *session_data,
			void *cb_prv_data)
{
	struct xio_msg	*req;
	int		i = 0;

	printf("**** [%p] on_new_session :%s:%d\n", session,
	       get_ip((struct sockaddr *)&session_data->src_addr),
	       get_port((struct sockaddr *)&session_data->src_addr));

	xio_accept(session, NULL, 0, NULL, 0);

	msg_pool_reset(pool);
	conn = xio_get_connection(session, ctx);

	printf("**** starting ...\n");
	while (1) {
		/* create transaction */
		req = msg_pool_get(pool);
		if (req == NULL)
			break;

		/* get pointers to internal buffers */
		req->in.header.iov_base = NULL;
		req->in.header.iov_len = 0;
		req->in.data_iovlen = 1;
		req->in.data_iov[0].iov_base = NULL;
		req->in.data_iov[0].iov_len  = ONE_MB;
		req->in.data_iov[0].mr = NULL;

		/* recycle the message and fill new request */
		msg_set(req, 1,
			test_config.hdr_len,
			test_config.data_len);

		/* try to send it */
		if (xio_send_request(conn, req) == -1) {
			printf("**** sent %d messages\n", i);
			if (xio_errno() != EAGAIN)
				printf("**** [%p] Error - xio_send_msg " \
				       "failed. %s\n",
					session,
					xio_strerror(xio_errno()));
			msg_pool_put(pool, req);
			return 0;
		}
		i++;
		if (i == 256)
			break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_request								     */
/*---------------------------------------------------------------------------*/
static int on_request(struct xio_session *session,
			struct xio_msg *req,
			int more_in_batch,
			void *cb_prv_data)
{
	struct xio_msg	*rsp;

	if (req->status)
		printf("**** request completed with error. [%s]\n",
		       xio_strerror(req->status));

	/* process request */
	process_request(req);

	/* alloc transaction */
	rsp	= msg_pool_get(pool);

	rsp->request		= req;
	rsp->more_in_batch	= 0;

	/* fill response */
	msg_set(rsp, 0,
		test_config.hdr_len,
		test_config.data_len);

	if (xio_send_response(rsp) == -1) {
		printf("**** [%p] Error - xio_send_msg failed. %s\n",
		       session, xio_strerror(xio_errno()));
		msg_pool_put(pool, rsp);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_response								     */
/*---------------------------------------------------------------------------*/
static int on_response(struct xio_session *session,
			struct xio_msg *rsp,
			int more_in_batch,
			void *cb_prv_data)
{
	process_response(rsp);

	if (rsp->status)
		printf("**** message completed with error. [%s]\n",
		       xio_strerror(rsp->status));

	/* message is no longer needed */
	xio_release_response(rsp);

	/* reset message */
	rsp->in.header.iov_base = NULL;
	rsp->in.header.iov_len = 0;
	rsp->in.data_iovlen = 1;
	rsp->in.data_iov[0].iov_base  = NULL;
	rsp->in.data_iov[0].iov_len  = ONE_MB;
	rsp->in.data_iov[0].mr = NULL;

	rsp->sn = 0;
	rsp->more_in_batch = 0;

	do {
		/* recycle the message and fill new request */
		msg_set(rsp, 1,
			test_config.hdr_len,
			test_config.data_len);

		/* try to send it */
		if (xio_send_request(conn, rsp) == -1) {
			if (xio_errno() != EAGAIN)
				printf("**** [%p] Error - xio_send_msg " \
				       "failed %s\n",
				       session,
				       xio_strerror(xio_errno()));
			msg_pool_put(pool, rsp);
			return 0;
		}
	} while (0);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_request								     */
/*---------------------------------------------------------------------------*/
static int on_message(struct xio_session *session,
			struct xio_msg *msg,
			int more_in_batch,
			void *cb_prv_data)
{
	switch (msg->type) {
	case XIO_MSG_TYPE_REQ:
		on_request(session, msg, more_in_batch, cb_prv_data);
		break;
	case XIO_MSG_TYPE_RSP:
		on_response(session, msg, more_in_batch, cb_prv_data);
		break;
	default:
		printf("unknown message type : %d\n", msg->type);
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_send_response_complete						     */
/*---------------------------------------------------------------------------*/
static int on_send_response_complete(struct xio_session *session,
			struct xio_msg *msg,
			void *cb_prv_data)
{
	/* can be safely freed */
	msg_pool_put(pool, msg);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_msg_error								     */
/*---------------------------------------------------------------------------*/
int on_msg_error(struct xio_session *session,
		enum xio_status error, struct xio_msg  *msg,
		void *cb_private_data)
{
	if (msg->type == XIO_MSG_TYPE_RSP)
		printf("**** [%p] message [%"PRIu64"] failed. reason: %s\n",
		       session, msg->request->sn, xio_strerror(error));
	else
		printf("**** [%p] message [%"PRIu64"] failed. reason: %s\n",
		       session, msg->sn, xio_strerror(error));

	msg_pool_put(pool, msg);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* callbacks								     */
/*---------------------------------------------------------------------------*/
struct xio_session_ops server_ops = {
	.on_session_event		=  on_session_event,
	.on_new_session			=  on_new_session,
	.on_msg_send_complete		=  on_send_response_complete,
	.on_msg				=  on_message,
	.on_msg_error			=  on_msg_error,
};

/*---------------------------------------------------------------------------*/
/* usage                                                                     */
/*---------------------------------------------------------------------------*/
static void usage(const char *argv0, int status)
{
	printf("Usage:\n");
	printf("  %s [OPTIONS]\t\t\tStart a server and wait for connection\n",
	       argv0);
	printf("\n");
	printf("Options:\n");

	printf("\t-c, --cpu=<cpu num> ");
	printf("\t\tBind the process to specific cpu (default 0)\n");

	printf("\t-p, --port=<port> ");
	printf("\t\tListen on port <port> (default %d)\n",
	       XIO_DEF_PORT);

	printf("\t-n, --header-len=<number> ");
	printf("\tSet the header length of the message to <number> bytes " \
			"(default %d)\n", XIO_DEF_HEADER_SIZE);

	printf("\t-w, --data-len=<length> ");
	printf("\tSet the data length of the message to <number> bytes " \
			"(default %d)\n", XIO_DEF_DATA_SIZE);

	printf("\t-v, --version ");
	printf("\t\t\tPrint the version and exit\n");

	printf("\t-h, --help ");
	printf("\t\t\tDisplay this help and exit\n");

	exit(status);
}

/*---------------------------------------------------------------------------*/
/* parse_cmdline							     */
/*---------------------------------------------------------------------------*/
int parse_cmdline(struct xio_test_config *test_config,
		int argc, char **argv)
{
	while (1) {
		int c;

		static struct option const long_options[] = {
			{ .name = "core",	.has_arg = 1, .val = 'c'},
			{ .name = "port",	.has_arg = 1, .val = 'p'},
			{ .name = "header-len",	.has_arg = 1, .val = 'n'},
			{ .name = "data-len",	.has_arg = 1, .val = 'w'},
			{ .name = "version",	.has_arg = 0, .val = 'v'},
			{ .name = "help",	.has_arg = 0, .val = 'h'},
			{0, 0, 0, 0},
		};

		static char *short_options = "c:p:n:w:svh";

		c = getopt_long(argc, argv, short_options,
				long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			test_config->cpu =
				(uint16_t)strtol(optarg, NULL, 0);
			break;
		case 'p':
			test_config->server_port =
				(uint16_t)strtol(optarg, NULL, 0);
			break;
		case 'n':
			test_config->hdr_len =
				(uint32_t)strtol(optarg, NULL, 0);
		break;
		case 'w':
			test_config->data_len =
				(uint32_t)strtol(optarg, NULL, 0);
			break;
		case 'v':
			printf("version: %s\n", XIO_TEST_VERSION);
			exit(0);
			break;
		case 'h':
			usage(argv[0], 0);
			break;
		default:
			fprintf(stderr, " invalid command or flag.\n");
			fprintf(stderr,
				" please check command line and run again.\n\n");
			usage(argv[0], -1);
			break;
		}
	}
	if (optind == argc - 1) {
		strcpy(test_config->server_addr, argv[optind]);
	} else if (optind < argc) {
		fprintf(stderr,
			" Invalid Command line.Please check command rerun\n");
		exit(-1);
	}

	return 0;
}

/*************************************************************
* Function: print_test_config
*-------------------------------------------------------------
* Description: print the test configuration
*************************************************************/
static void print_test_config(
		const struct xio_test_config *test_config_p)
{
	printf(" =============================================\n");
	printf(" Server Address		: %s\n", test_config_p->server_addr);
	printf(" Server Port		: %u\n", test_config_p->server_port);
	printf(" Header Length		: %u\n", test_config_p->hdr_len);
	printf(" Data Length		: %u\n", test_config_p->data_len);
	printf(" CPU Affinity		: %x\n", test_config_p->cpu);
	printf(" =============================================\n");
}

/*---------------------------------------------------------------------------*/
/* main									     */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	struct xio_server	*server;
	char			url[256];

	if (parse_cmdline(&test_config, argc, argv) != 0)
		return -1;


	print_test_config(&test_config);

	set_cpu_affinity(test_config.cpu);

	ctx	= xio_context_create(NULL, 0);

	if (msg_api_init(test_config.hdr_len, test_config.data_len, 1) != 0)
		return -1;

	pool = msg_pool_alloc(MAX_POOL_SIZE,
			      test_config.hdr_len, test_config.data_len,
			      0, 0);


	sprintf(url, "rdma://%s:%d", test_config.server_addr,
		test_config.server_port);

	server = xio_bind(ctx, &server_ops, url, NULL, 0, NULL);
	if (server) {
		printf("listen to %s\n", url);
		xio_context_run_loop(ctx, XIO_INFINITE);

		/* normal exit phase */
		fprintf(stdout, "exit signaled\n");

		/* free the server */
		xio_unbind(server);
	}

	if (pool)
		msg_pool_free(pool);
	pool = NULL;

	xio_context_destroy(ctx);

	return 0;
}

