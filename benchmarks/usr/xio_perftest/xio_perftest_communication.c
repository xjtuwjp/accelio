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
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "libxio.h"
#include "xio_perftest_parameters.h"
#include "xio_perftest_communication.h"


#define CONFIG_PORT	20610

/* configuration session data */
struct control_context {
	struct xio_session	*session;
	struct xio_server	*server;
	struct xio_context	*ctx;
	struct xio_connection	*conn;
	struct xio_msg		msg;
	struct xio_msg		*reply;
	int			disconnect;
	int			failed;
};


/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
		struct xio_session_event_data *event_data,
		void *cb_user_context)
{
	struct perf_comm *comm = cb_user_context;

	switch (event_data->event) {
	case XIO_SESSION_CONNECTION_ERROR_EVENT:
	case XIO_SESSION_REJECT_EVENT:
	case XIO_SESSION_CONNECTION_DISCONNECTED_EVENT:
		comm->control_ctx->failed = 1;
		break;
	case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
		xio_connection_destroy(event_data->conn);
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		xio_context_stop_loop(comm->control_ctx->ctx, 0);  /* exit */
		xio_session_destroy(session);
		break;
	default:
		break;
	};

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_message								     */
/*---------------------------------------------------------------------------*/
static int on_message(struct xio_session *session,
		      struct xio_msg *msg,
		      int more_in_batch,
		      void *cb_user_context)
{
	struct perf_comm *comm = cb_user_context;

	if (comm->control_ctx->reply)
		fprintf(stderr, "message overrun\n");

	comm->control_ctx->reply = msg;

	xio_context_stop_loop(comm->control_ctx->ctx, 0);  /* exit */

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_new_session							     */
/*---------------------------------------------------------------------------*/
static int on_new_session(struct xio_session *session,
			  struct xio_new_session_req *req,
			  void *cb_user_context)
{
	struct perf_comm *comm = cb_user_context;

	xio_accept(session, NULL, 0, NULL, 0);
	comm->control_ctx->conn = xio_get_connection(
					session, comm->control_ctx->ctx);

	xio_context_stop_loop(comm->control_ctx->ctx, 0);  /* exit */

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_session_established						     */
/*---------------------------------------------------------------------------*/
static int on_session_established(struct xio_session *session,
				  struct xio_new_session_rsp *rsp,
				  void *cb_user_context)
{
	struct perf_comm *comm = cb_user_context;

	xio_context_stop_loop(comm->control_ctx->ctx, 0);  /* exit */

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_msg_send_complete							     */
/*---------------------------------------------------------------------------*/
static int on_msg_send_complete(struct xio_session *session,
				struct xio_msg *rsp,
				void *conn_user_context)
{
	struct perf_comm *comm = conn_user_context;

	comm->control_ctx->reply  = NULL;
	if (comm->control_ctx->disconnect) {
		struct xio_connection *conn = xio_get_connection(
				session,
				comm->control_ctx->ctx);
		xio_disconnect(conn);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* callbacks								     */
/*---------------------------------------------------------------------------*/
static struct xio_session_ops	ses_ops = {
	.on_session_event		=  on_session_event,
	.on_msg				=  on_message,
	.on_new_session			=  on_new_session,
	.on_session_established		=  on_session_established,
	.on_msg_send_complete		=  on_msg_send_complete
};


/*---------------------------------------------------------------------------*/
/* create_comm_struct							     */
/*---------------------------------------------------------------------------*/
struct perf_comm *create_comm_struct(struct perf_parameters *user_param)
{
	struct perf_comm *comm;

	comm = calloc(1, sizeof(*comm));
	if (comm == NULL)
		return NULL;

	comm->control_ctx = calloc(1, sizeof(*comm->control_ctx));
	if (comm->control_ctx == NULL) {
		free(comm);
		return NULL;
	}
	comm->user_param = user_param;


	return comm;
}

/*---------------------------------------------------------------------------*/
/* on_response								     */
/*---------------------------------------------------------------------------*/
void destroy_comm_struct(struct perf_comm *comm)
{
	free(comm->control_ctx);
	free(comm);
}

/*---------------------------------------------------------------------------*/
/* establish_connection							     */
/*---------------------------------------------------------------------------*/
int establish_connection(struct perf_comm *comm)
{
	char	url[256];

	/* client session attributes */
	struct xio_session_attr attr = {
		&ses_ops,	/* callbacks structure */
		NULL,		/* no need to pass the server private data */
		0
	};

	/* create thread context for the client */
	comm->control_ctx->ctx = xio_context_create(NULL, 0);

	if (comm->user_param->machine_type == SERVER) {
		sprintf(url, "rdma://*:%d", CONFIG_PORT);

		/* bind a listener server to a portal/url */
		comm->control_ctx->server = xio_bind(comm->control_ctx->ctx,
						     &ses_ops, url,
						     NULL, 0, comm);
		if (!comm->control_ctx->server)
			fprintf(stderr, "failed to bind server\n");
	} else {
		sprintf(url, "rdma://%s:%d",
			comm->user_param->server_addr, CONFIG_PORT);

		/* create url to connect to */
		comm->control_ctx->session = xio_session_create(
				XIO_SESSION_CLIENT,
				&attr, url, 0, 0, comm);

		/* connect the session  */
		comm->control_ctx->conn = xio_connect(
				comm->control_ctx->session,
				comm->control_ctx->ctx, 0, NULL,
				comm);
	}

	/* the default xio supplied main loop */
	xio_context_run_loop(comm->control_ctx->ctx, XIO_INFINITE);

	return comm->control_ctx->failed;
}

/*---------------------------------------------------------------------------*/
/* ctx_xchg_data							     */
/*---------------------------------------------------------------------------*/
int ctx_xchg_data(struct perf_comm *comm,
		  void *my_data, void *rem_data, int size)
{
	if (comm->control_ctx->failed)
		return -1;

	if (comm->user_param->machine_type == CLIENT) {
		comm->control_ctx->msg.out.header.iov_base	= my_data;
		comm->control_ctx->msg.out.header.iov_len	= size;
		comm->control_ctx->msg.out.data_iovlen		= 0;
		comm->control_ctx->msg.in.header.iov_len	= 0;
		comm->control_ctx->msg.in.data_iovlen		= 0;

		xio_send_request(comm->control_ctx->conn,
				 &comm->control_ctx->msg);
		xio_context_run_loop(comm->control_ctx->ctx, XIO_INFINITE);

		if (comm->control_ctx->reply) {
			if (comm->control_ctx->reply->in.header.iov_len)
				memcpy(
				rem_data,
				comm->control_ctx->reply->in.header.iov_base,
				comm->control_ctx->reply->in.header.iov_len);

			xio_release_response(comm->control_ctx->reply);
			comm->control_ctx->reply = NULL;
		}
	} else {
		xio_context_run_loop(comm->control_ctx->ctx, XIO_INFINITE);

		if (comm->control_ctx->failed)
			goto cleanup;

		if (comm->control_ctx->reply &&
		    comm->control_ctx->reply->in.header.iov_len)
			memcpy(rem_data,
			       comm->control_ctx->reply->in.header.iov_base,
			       comm->control_ctx->reply->in.header.iov_len);

		comm->control_ctx->msg.out.header.iov_base	= my_data;
		comm->control_ctx->msg.out.header.iov_len	= size;
		comm->control_ctx->msg.out.data_iovlen		= 0;
		comm->control_ctx->msg.in.header.iov_len	= 0;
		comm->control_ctx->msg.in.data_iovlen		= 0;

		comm->control_ctx->msg.request = comm->control_ctx->reply;

		xio_send_response(&comm->control_ctx->msg);
	}

	return 0;

cleanup:
	if (comm->control_ctx->reply) {
		xio_release_msg(comm->control_ctx->reply);
		comm->control_ctx->reply = NULL;
	}
	return -1;
}

/*---------------------------------------------------------------------------*/
/* ctx_write_data							     */
/*---------------------------------------------------------------------------*/
int ctx_write_data(struct perf_comm *comm, void *data, int size)
{
	if (comm->control_ctx->failed)
		return -1;

	comm->control_ctx->msg.out.header.iov_base	= data;
	comm->control_ctx->msg.out.header.iov_len	= size;
	comm->control_ctx->msg.out.data_iovlen		= 0;
	comm->control_ctx->msg.in.header.iov_len	= 0;
	comm->control_ctx->msg.in.data_iovlen		= 0;

	xio_send_msg(comm->control_ctx->conn, &comm->control_ctx->msg);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* ctx_read_data							     */
/*---------------------------------------------------------------------------*/
int ctx_read_data(struct perf_comm *comm, void *data, int size, int *osize)
{
	if (comm->control_ctx->failed)
		goto cleanup;

	xio_context_run_loop(comm->control_ctx->ctx, XIO_INFINITE);

	if (comm->control_ctx->failed)
		goto cleanup;

	if (osize)
		*osize = comm->control_ctx->reply->in.header.iov_len;
	if (comm->control_ctx->reply->in.header.iov_len > size)
		goto cleanup;

	if (comm->control_ctx->reply->in.header.iov_len)
		memcpy(data,
		       comm->control_ctx->reply->in.header.iov_base,
		       comm->control_ctx->reply->in.header.iov_len);

	xio_release_msg(comm->control_ctx->reply);
	comm->control_ctx->reply = NULL;

	return 0;

cleanup:
	if (comm->control_ctx->reply) {
		xio_release_msg(comm->control_ctx->reply);
		comm->control_ctx->reply = NULL;
	}
	return -1;
}

/*---------------------------------------------------------------------------*/
/* ctx_hand_shake							     */
/*---------------------------------------------------------------------------*/
int ctx_hand_shake(struct perf_comm *comm)
{
	if (comm->control_ctx->failed)
		return -1;

	if (comm->user_param->machine_type == CLIENT) {
		if (ctx_write_data(comm, NULL, 0))
			return -1;
		if (ctx_read_data(comm, NULL, 0, NULL))
			return -1;
	} else {
		if (ctx_read_data(comm, NULL, 0, NULL))
			return -1;
		if (ctx_write_data(comm, NULL, 0))
			return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* ctx_close_connection							     */
/*---------------------------------------------------------------------------*/
int ctx_close_connection(struct perf_comm *comm)
{
	char done[16];

	if (!comm->control_ctx->failed) {
		comm->control_ctx->disconnect = 1;
		if (ctx_xchg_data(comm, "done", done , sizeof "done"))
			goto cleanup;

		if (!comm->control_ctx->failed) {
			if (comm->user_param->machine_type == CLIENT)
				xio_disconnect(comm->control_ctx->conn);

			xio_context_run_loop(comm->control_ctx->ctx,
					     XIO_INFINITE);
		}
	}

cleanup:
	if (comm->user_param->machine_type == SERVER)
		xio_unbind(comm->control_ctx->server);

	xio_context_destroy(comm->control_ctx->ctx);

	return 0;
}
