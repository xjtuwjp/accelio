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
#include "xio_os.h"
#include "libxio.h"
#include "xio_common.h"
#include "xio_protocol.h"
#include "xio_observer.h"
#include "xio_task.h"
#include "xio_context.h"
#include "xio_transport.h"
#include "xio_sessions_store.h"
#include "xio_hash.h"
#include "xio_session.h"
#include "xio_conn.h"
#include "xio_connection.h"

#define XIO_ACTION_ACCEPT	1
#define XIO_ACTION_REDIRECT	2
#define XIO_ACTION_REJECT	3

#define MAX_PORTAL_LEN		192
#define MAX_RESOURCE_LEN	1024
#define SETUP_BUFFER_LEN	3840   /* 4096-256 */


/*---------------------------------------------------------------------------*/
/* forward declarations							     */
/*---------------------------------------------------------------------------*/
static int xio_on_req_recv(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_rsp_recv(struct xio_connection *connetion,
				  struct xio_task *task);
static int xio_on_ow_req_send_comp(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_rsp_send_comp(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_setup_req_recv(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_setup_rsp_send_comp(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_setup_rsp_recv(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_conn_event(void *observer, void *sender, int event,
			       void *event_data);
static int xio_on_conn_refused(struct xio_session *session,
				  struct xio_conn *conn,
				  union xio_conn_event_data *event_data);
static int xio_on_fin_req_recv(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_fin_rsp_recv(struct xio_connection *connection,
				  struct xio_task *task);
static int xio_on_fin_send_comp(struct xio_connection *connection,
				  struct xio_task *task);

/*---------------------------------------------------------------------------*/
/* xio_session_alloc_conn						     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_session_alloc_conn(
		struct xio_session *session,
		struct xio_context *ctx,
		uint32_t conn_idx,
		void	*conn_user_context
		)
{
	struct xio_connection		*connection;

	/* allocate and initialize connection */
	connection = xio_connection_init(session, ctx,
					 conn_idx, conn_user_context);
	if (connection == NULL) {
		ERROR_LOG("failed to initialize connection. " \
			  "seesion:%p, ctx:%p, conn_idx:%d\n",
			  session, ctx, conn_idx);
		return NULL;
	}
	/* add the connection  to the session's connections list */
	spin_lock(&session->conn_list_lock);
	list_add(&connection->connections_list_entry,
		 &session->connections_list);
	session->conns_nr++;
	spin_unlock(&session->conn_list_lock);

	return connection;
}

/*---------------------------------------------------------------------------*/
/* xio_session_free_conn						     */
/*---------------------------------------------------------------------------*/
int xio_session_free_conn(struct xio_connection *connection)
{
	int retval;

	spin_lock(&connection->session->conn_list_lock);
	connection->session->conns_nr--;
	list_del(&connection->connections_list_entry);
	spin_unlock(&connection->session->conn_list_lock);

	retval = xio_connection_close(connection);
	if (retval != 0) {
		ERROR_LOG("failed to close connection");
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_connection_set_conn						     */
/*---------------------------------------------------------------------------*/
static inline void xio_connection_set_conn(struct xio_connection *connection,
				    struct xio_conn *conn)
{
	if (connection->conn && connection->conn == conn)
		return;

	if (connection->conn)
		xio_conn_unreg_observer(connection->conn,
					&connection->session->observer);

	if (conn) {
		xio_conn_unreg_observer(conn,
					&connection->session->observer);
		xio_conn_reg_observer(conn,
				      &connection->session->observer,
				      connection->session->session_id);
	}

	connection->conn = conn;
}

/*---------------------------------------------------------------------------*/
/* xio_session_assign_conn						     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_session_assign_conn(
		struct xio_session *session,
		struct xio_conn *conn)
{
	struct xio_connection		*connection;

	spin_lock(&session->conn_list_lock);
	/* find free slot */
	list_for_each_entry(connection, &session->connections_list,
			    connections_list_entry) {
		if ((connection->ctx == conn->transport_hndl->ctx)  &&
		    ((connection->conn == NULL) ||
		     (connection->conn == conn))) {
			/* remove old observer if exist */
			spin_unlock(&session->conn_list_lock);
			xio_connection_set_conn(connection, conn);
			return connection;
		}
	}
	spin_unlock(&session->conn_list_lock);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_session_find_conn						     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_session_find_conn(
		struct xio_session *session,
		struct xio_conn *conn)
{
	struct xio_connection		*connection;

	list_for_each_entry(connection, &session->connections_list,
			    connections_list_entry) {
		if (connection->conn == conn)
			return connection;
	}

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_session_find_conn_by_ctx						     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_session_find_conn_by_ctx(
		struct xio_session *session,
		struct xio_context *ctx)
{
	struct xio_connection		*connection;

	list_for_each_entry(connection, &session->connections_list,
			    connections_list_entry) {
		if (connection->ctx == ctx)
			return connection;
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_find_session							     */
/*---------------------------------------------------------------------------*/
struct xio_session *xio_find_session(struct xio_task *task)
{
	struct xio_session_hdr	*tmp_hdr;
	struct xio_observer	*observer;
	struct xio_session	*session;
	uint32_t		dest_session_id;

	xio_mbuf_push(&task->mbuf);

	/* set start of the session header */
	tmp_hdr = xio_mbuf_set_session_hdr(&task->mbuf);

	xio_mbuf_pop(&task->mbuf);

	dest_session_id = ntohl(tmp_hdr->dest_session_id);

	observer = xio_conn_observer_lookup(task->conn, dest_session_id);
	if (observer != NULL)
		return observer->impl;

	/* fall back to store - this is should only happen when new connection
	 * message arrive to a portal on the server - just for the first
	 * message
	 */
	session = xio_sessions_store_lookup(dest_session_id);
	if (session == NULL)
		ERROR_LOG("failed to find session\n");

	return session;
}

/*---------------------------------------------------------------------------*/
/* xio_session_write_header						     */
/*---------------------------------------------------------------------------*/
int xio_session_write_header(struct xio_task *task,
			     struct xio_session_hdr *hdr)
{
	struct xio_session_hdr *tmp_hdr;

	/* set start of the session header */
	tmp_hdr = xio_mbuf_set_session_hdr(&task->mbuf);

	/* fill header */
	PACK_LVAL(hdr, tmp_hdr,  dest_session_id);
	PACK_LLVAL(hdr, tmp_hdr, serial_num);
	PACK_LVAL(hdr, tmp_hdr, flags);
	PACK_LVAL(hdr, tmp_hdr, receipt_result);

	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_session_hdr));

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_session_read_header						     */
/*---------------------------------------------------------------------------*/
static int xio_session_read_header(struct xio_task *task,
				   struct xio_session_hdr *hdr)
{
	struct xio_session_hdr *tmp_hdr;

	/* set start of the session header */
	tmp_hdr = xio_mbuf_set_session_hdr(&task->mbuf);

	/* fill request */
	UNPACK_LLVAL(tmp_hdr, hdr, serial_num);
	UNPACK_LVAL(tmp_hdr, hdr, dest_session_id);
	UNPACK_LVAL(tmp_hdr, hdr, flags);
	UNPACK_LVAL(tmp_hdr, hdr, receipt_result);

	xio_mbuf_inc(&task->mbuf, sizeof(struct xio_session_hdr));

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_session_release							     */
/*---------------------------------------------------------------------------*/
static void xio_session_release(struct xio_session *session)
{
	int i;

	/* unregister session from context */
	xio_sessions_store_remove(session->session_id);
	for (i = 0; i < session->services_array_len; i++)
		kfree(session->services_array[i]);
	for (i = 0; i < session->portals_array_len; i++)
		kfree(session->portals_array[i]);
	kfree(session->services_array);
	kfree(session->portals_array);
	kfree(session->user_context);
	kfree(session->uri);
	mutex_destroy(&session->lock);
	kfree(session);

	TRACE_LOG("session released\n");
}

/*---------------------------------------------------------------------------*/
/* xio_session_write_setup_req						     */
/*---------------------------------------------------------------------------*/
static struct xio_msg *xio_session_write_setup_req(
						struct xio_session *session)
{
	struct xio_msg		*msg;
	void			*buf;
	uint8_t			*ptr;
	uint16_t		len;


	/* allocate message */
	buf = kcalloc(SETUP_BUFFER_LEN + sizeof(struct xio_msg),
		      sizeof(uint8_t), GFP_KERNEL);
	if (buf == NULL) {
		ERROR_LOG("message allocation failed\n");
		xio_set_error(ENOMEM);
		return NULL;
	}

	/* fill the message */
	msg = buf;
	msg->out.header.iov_base = msg + 1;
	msg->out.header.iov_len = 0;
	msg->out.data_iovlen = 0;

	ptr = msg->out.header.iov_base;
	len = 0;

	/* serialize message on the buffer */
	len = xio_write_uint32(session->session_id , 0, ptr);
	ptr  = ptr + len;

	/* uri length */
	len = xio_write_uint16((uint16_t)session->uri_len , 0, ptr);
	ptr  = ptr + len;

	/* private length */
	len = xio_write_uint16((uint16_t)(session->user_context_len),
				  0, ptr);
	ptr  = ptr + len;

	if (session->uri_len) {
		len = xio_write_array((uint8_t *)session->uri,
					session->uri_len, 0, ptr);
		ptr  = ptr + len;
	}
	if (session->user_context_len) {
		len = xio_write_array(session->user_context,
					session->user_context_len,
					  0, ptr);
		ptr  = ptr + len;
	}
	msg->out.header.iov_len = ptr - (uint8_t *)msg->out.header.iov_base;

	if (msg->out.header.iov_len > SETUP_BUFFER_LEN)  {
		ERROR_LOG("primary task pool is empty\n");
		xio_set_error(XIO_E_MSG_SIZE);
		kfree(buf);
		return NULL;
	}

	return msg;
}

/*---------------------------------------------------------------------------*/
/* xio_on_setup_req_recv			                             */
/*---------------------------------------------------------------------------*/
static int xio_on_setup_req_recv(struct xio_connection *connection,
				  struct xio_task *task)
{
	struct xio_msg			*msg = &task->imsg;
	struct xio_new_session_req	req;
	uint8_t				*ptr;
	uint16_t			len;
	struct xio_session_hdr		hdr;
	struct xio_session		*session = connection->session;
	int				retval;
	struct xio_session_event_data  error_event = {
		.event = XIO_SESSION_ERROR_EVENT,
	};

	/* read session header */
	if (xio_session_read_header(task, &hdr) != 0) {
		ERROR_LOG("failed to read header\n");
		xio_set_error(XIO_E_MSG_INVALID);
		goto cleanup;
	}
	task->imsg.sn = hdr.serial_num;
	task->connection = connection;
	connection->session->setup_req = msg;

	/* read the header */
	ptr = msg->in.header.iov_base;

	memset(&req, 0, sizeof(req));

	/* session id */
	len = xio_read_uint32(&session->peer_session_id , 0, ptr);
	ptr  = ptr + len;

	/* uri length */
	len = xio_read_uint16(&req.uri_len, 0, ptr);
	ptr = ptr + len;

	/* private length */
	len = xio_read_uint16(&req.user_context_len, 0, ptr);
	ptr = ptr + len;

	if (req.uri_len) {
		req.uri = kcalloc(req.uri_len, sizeof(char), GFP_KERNEL);
		if (req.uri == NULL) {
			xio_set_error(ENOMEM);
			ERROR_LOG("uri allocation failed. len:%d\n",
				  req.uri_len);
			goto cleanup1;
		}

		len = xio_read_array((uint8_t *)req.uri,
					req.uri_len, 0, ptr);
		ptr = ptr + len;
	}
	if (req.user_context_len) {
		req.user_context = kcalloc(req.user_context_len,
					  sizeof(uint8_t), GFP_KERNEL);
		if (req.user_context == NULL) {
			xio_set_error(ENOMEM);
			ERROR_LOG("private data allocation failed. len:%d\n",
				  req.user_context_len);
			goto cleanup2;
		}
		len = xio_read_array(req.user_context, req.user_context_len,
				       0, ptr);
		ptr = ptr + len;
	}

	req.proto = xio_conn_get_proto(connection->conn);
	xio_conn_get_src_addr(connection->conn,
			      &req.src_addr, sizeof(req.src_addr));

	/* store the task in io queue*/
	xio_connection_queue_io_task(connection, task);

	/* notify the upper layer */
	if (connection->ses_ops.on_new_session) {
		connection->ses_ops.on_new_session(
				session, &req,
				connection->cb_user_context);
	} else {
		retval = xio_accept(session, NULL, 0, NULL, 0);
		if (retval) {
			ERROR_LOG("failed to auto accept session. session:%p\n",
				  session);
			goto cleanup2;
		}
	}

	kfree(req.uri);
	kfree(req.user_context);

	return 0;

cleanup2:
	kfree(req.user_context);

cleanup1:
	kfree(req.uri);

cleanup:
	if (session->ses_ops.on_session_event) {
		error_event.reason = xio_errno();
		session->ses_ops.on_session_event(
				session, &error_event,
				session->cb_user_context);
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_session_write_accept_rsp						     */
/*---------------------------------------------------------------------------*/
static struct xio_msg *xio_session_write_accept_rsp(
		struct xio_session *session,
		uint16_t action,
		const char **portals_array,
		uint16_t portals_array_len,
		void *user_context,
		uint16_t user_context_len)
{
	struct xio_msg	*msg;
	uint8_t			*buf;
	uint8_t			*ptr;
	uint16_t		len, i, str_len, tot_len;


	/* calclate length */
	tot_len = 3*sizeof(uint16_t) + sizeof(uint32_t);
	for (i = 0; i < portals_array_len; i++)
		tot_len += strlen(portals_array[i]) + sizeof(uint16_t);
	tot_len += user_context_len;

	if (tot_len > SETUP_BUFFER_LEN)  {
		ERROR_LOG("buffer is too small\n");
		xio_set_error(EMSGSIZE);
		return NULL;
	}

	/* allocate message */
	buf = kcalloc(SETUP_BUFFER_LEN + sizeof(struct xio_msg),
		     sizeof(uint8_t), GFP_KERNEL);
	if (buf == NULL) {
		ERROR_LOG("message allocation failed\n");
		xio_set_error(ENOMEM);
		return NULL;
	}

	/* fill the message */
	msg = (struct xio_msg *)buf;
	msg->out.header.iov_base = buf + sizeof(struct xio_msg);
	msg->out.header.iov_len = 0;


	ptr = msg->out.header.iov_base;
	len = 0;

	/* serialize message into the buffer */

	/* session_id */
	len = xio_write_uint32(session->session_id , 0, ptr);
	ptr  = ptr + len;

	/* action */
	len = xio_write_uint16(action, 0, ptr);
	ptr  = ptr + len;

	/* portals_array_len */
	len = xio_write_uint16(portals_array_len, 0, ptr);
	ptr  = ptr + len;

	/* user_context_len */
	len = xio_write_uint16(user_context_len, 0, ptr);
	ptr  = ptr + len;


	for (i = 0; i < portals_array_len; i++) {
		str_len = strlen(portals_array[i]);

		len = xio_write_uint16(str_len, 0, ptr);
		ptr  = ptr + len;

		len = xio_write_array((uint8_t *)portals_array[i],
					 str_len, 0, ptr);
		ptr  = ptr + len;
	}

	if (user_context_len) {
		len = xio_write_array(user_context,
					user_context_len,
					0, ptr);
		ptr  = ptr + len;
	}

	msg->out.header.iov_len = ptr - (uint8_t *)msg->out.header.iov_base;

	if (msg->out.header.iov_len != tot_len) {
		ERROR_LOG("calculated length %d != actual length %zd\n",
			  tot_len, msg->out.header.iov_len);
	}

	return msg;
}

/*---------------------------------------------------------------------------*/
/* xio_session_write_reject_rsp						     */
/*---------------------------------------------------------------------------*/
static struct xio_msg *xio_session_write_reject_rsp(
		struct xio_session *session,
		enum xio_status reason,
		void *user_context,
		uint16_t user_context_len)
{
	struct xio_msg	*msg;
	uint8_t			*buf;
	uint8_t			*ptr;
	uint16_t		len,  tot_len;
	uint16_t		action = XIO_ACTION_REJECT;


	/* calclate length */
	tot_len = 2*sizeof(uint16_t) + 2*sizeof(uint32_t);
	tot_len += user_context_len;

	if (tot_len > SETUP_BUFFER_LEN)  {
		ERROR_LOG("buffer is too small\n");
		xio_set_error(EMSGSIZE);
		return NULL;
	}

	/* allocate message */
	buf = kcalloc(SETUP_BUFFER_LEN + sizeof(struct xio_msg),
		      sizeof(uint8_t), GFP_KERNEL);
	if (buf == NULL) {
		ERROR_LOG("message allocation failed\n");
		xio_set_error(ENOMEM);
		return NULL;
	}

	/* fill the message */
	msg = (struct xio_msg *)buf;
	msg->out.header.iov_base = buf + sizeof(struct xio_msg);
	msg->out.header.iov_len = 0;


	ptr = msg->out.header.iov_base;
	len = 0;

	/* serialize message into the buffer */

	/* session_id */
	len = xio_write_uint32(session->session_id , 0, ptr);
	ptr  = ptr + len;

	/* action */
	len = xio_write_uint16(action, 0, ptr);
	ptr  = ptr + len;

	/* reason */
	len = xio_write_uint32(reason, 0, ptr);
	ptr  = ptr + len;


	/* user_context_len */
	len = xio_write_uint16(user_context_len, 0, ptr);
	ptr  = ptr + len;

	if (user_context_len) {
		len = xio_write_array(user_context,
					user_context_len,
					0, ptr);
		ptr  = ptr + len;
	}

	msg->out.header.iov_len = ptr - (uint8_t *)msg->out.header.iov_base;

	if (msg->out.header.iov_len != tot_len) {
		ERROR_LOG("calculated length %d != actual length %zd\n",
			  tot_len, msg->out.header.iov_len);
	}

	return msg;
}


/*---------------------------------------------------------------------------*/
/* xio_session_accept_connection					     */
/*---------------------------------------------------------------------------*/
static int xio_session_accept_connection(struct xio_session *session)
{
	struct xio_connection	*connection;
	struct xio_conn		*conn;
	int			retval;
	char			*portal;

	list_for_each_entry(connection, &session->connections_list,
			    connections_list_entry) {
		if (connection->conn == NULL) {
			if (connection->conn_idx == 0) {
				portal = session->portals_array[
						session->last_opened_portal++];
				if (session->last_opened_portal ==
				    session->portals_array_len)
					session->last_opened_portal = 0;
			} else {
				int pid = (connection->conn_idx %
					   session->portals_array_len);
				portal = session->portals_array[pid];
			}
			conn = xio_conn_open(connection->ctx, portal,
					     &session->observer,
					     session->session_id);

			if (conn == NULL) {
				ERROR_LOG("failed to open connection to %s\n",
					  portal);
				break;
			}
			INFO_LOG("reconnecting to %s\n", portal);
			retval = xio_conn_connect(conn, portal);
			if (retval != 0) {
				ERROR_LOG("connection connect failed\n");
				break;
			}
			connection = xio_session_assign_conn(session, conn);
			if (connection == NULL) {
				ERROR_LOG("failed to assign connection\n");
				return -1;
			}
		}
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_session_redirect_connection					     */
/*---------------------------------------------------------------------------*/
static int xio_session_redirect_connection(struct xio_session *session)
{
	struct xio_conn		*conn, *tmp_conn;
	int			retval;
	char			*service;

	service = session->services_array[session->last_opened_service++];
	if (session->last_opened_service == session->services_array_len)
		session->last_opened_service = 0;

	conn = xio_conn_open(session->lead_conn->ctx, service, NULL, 0);
	if (conn == NULL) {
		ERROR_LOG("failed to open connection to %s\n",
			  service);
		return -1;
	}
	/* initialize the redirected connection */
	tmp_conn = session->lead_conn->conn;
	session->redir_conn = session->lead_conn;
	xio_connection_set_conn(session->redir_conn, conn);

	TRACE_LOG("connection redirected to %s\n", service);
	retval = xio_conn_connect(conn, service);
	if (retval != 0) {
		ERROR_LOG("connection connect failed\n");
		goto cleanup;
	}

	/* prep the lead connection for close */
	session->lead_conn = xio_connection_init(session,
			session->lead_conn->ctx,
			session->lead_conn->conn_idx,
			session->lead_conn->cb_user_context);
	xio_connection_set_conn(session->lead_conn, tmp_conn);

	return 0;

cleanup:
	xio_conn_close(conn, &session->observer);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_accept								     */
/*---------------------------------------------------------------------------*/
int xio_accept(struct xio_session *session,
		const char **portals_array,
		size_t portals_array_len,
		void *user_context,
		size_t user_context_len)
{
	int			retval = 0;
	struct xio_msg		*msg;
	struct xio_connection	*connection;

	msg = xio_session_write_accept_rsp(session,
					   XIO_ACTION_ACCEPT,
					   portals_array,
					   portals_array_len,
					   user_context,
					   user_context_len);
	if (msg == NULL) {
		ERROR_LOG("setup request creation failed\n");
		return -1;
	}
	if (portals_array_len != 0) {
		/* server side state is changed to ACCEPT */
		session->state = XIO_SESSION_STATE_ACCEPTED;
		TRACE_LOG("session state is now ACCEPT. session:%p\n",
			  session);
	}

	msg->request	= session->setup_req;
	msg->type	= XIO_SESSION_SETUP_RSP;

	connection = list_first_entry(
				&session->connections_list,
				struct xio_connection,
				connections_list_entry);

	retval = xio_connection_send(connection, msg);
	if (retval != 0) {
		ERROR_LOG("failed to send message\n");
		return -1;

	};

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_redirect								     */
/*---------------------------------------------------------------------------*/
int xio_redirect(struct xio_session *session,
	       const char **portals_array,
	       size_t portals_array_len)
{
	int			retval = 0;
	struct xio_msg		*msg;
	struct xio_connection	*connection;

	if (portals_array_len == 0 || portals_array == NULL) {
		xio_set_error(EINVAL);
		ERROR_LOG("portals array for redirect is mandatory\n");
		return -1;
	}

	msg = xio_session_write_accept_rsp(session,
					   XIO_ACTION_REDIRECT,
					   portals_array,
					   portals_array_len,
					   NULL,
					   0);
	if (msg == NULL) {
		ERROR_LOG("setup request creation failed\n");
		return -1;
	}
	if (portals_array_len != 0) {
		/* server side state is changed to ACCEPT */
		session->state = XIO_SESSION_STATE_REDIRECTED;
		TRACE_LOG("session state is now REDIRECTED. session:%p\n",
			  session);
	}
	msg->request = session->setup_req;
	msg->type    = XIO_SESSION_SETUP_RSP;

	connection = list_first_entry(
				&session->connections_list,
				struct xio_connection,
				connections_list_entry);

	retval = xio_connection_send(connection, msg);
	if (retval != 0) {
		ERROR_LOG("failed to send message\n");
		return -1;

	};

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_reject								     */
/*---------------------------------------------------------------------------*/
int xio_reject(struct xio_session *session,
	       enum xio_status reason,
	       void *user_context,
	       size_t user_context_len)
{
	int			retval = 0;
	struct xio_msg		*msg;
	struct xio_connection	*connection;

	msg = xio_session_write_reject_rsp(session,
		reason,
		user_context,
		user_context_len);
	if (msg == NULL) {
		ERROR_LOG("setup request creation failed\n");
		return -1;
	}
	/* server side state is changed to REJECTED */
	session->state = XIO_SESSION_STATE_REJECTED;
	TRACE_LOG("session state is now REJECT. session:%p\n",
		  session);

	msg->request = session->setup_req;
	msg->type    = XIO_SESSION_SETUP_RSP;
	connection = list_first_entry(
				&session->connections_list,
				struct xio_connection,
				connections_list_entry);

	retval = xio_connection_send(connection, msg);
	if (retval != 0) {
		ERROR_LOG("failed to send message\n");
		return -1;

	};

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_connection_rejected			                             */
/*---------------------------------------------------------------------------*/
static int xio_on_connection_rejected(struct xio_session *session,
				      struct xio_connection *connection)
{
	struct xio_connection		*tmp_connection;

	/* notify the upper layer */
	struct xio_session_event_data  event = {
		.event = XIO_SESSION_REJECT_EVENT,
		.reason = session->reject_reason,
		.conn   = connection,
		.conn_user_context =
			(connection) ? connection->cb_user_context : NULL
	};
	if (session->ses_ops.on_session_event)
		session->ses_ops.on_session_event(
				session, &event,
				session->cb_user_context);

	/* also send disconnect to connections that do no have conn */
	list_for_each_entry_safe(connection, tmp_connection,
				 &session->connections_list,
				 connections_list_entry) {
		if (connection && !connection->conn) {
			event.conn	= connection;
			event.conn_user_context =
				connection->cb_user_context;
			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &event,
						session->cb_user_context);
		}
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_read_setup_rsp							     */
/*---------------------------------------------------------------------------*/
static int xio_read_setup_rsp(struct xio_connection *connection,
			      struct xio_task *task,
			      uint16_t *action)
{
	struct xio_msg			*msg = &task->imsg;
	struct xio_session_hdr		hdr;
	struct xio_session		*session = connection->session;
	struct xio_new_session_rsp	*rsp = &session->new_ses_rsp;
	uint8_t				*ptr;
	uint16_t			len;
	int				i = 0;
	uint16_t			str_len;

	/* read session header */
	if (xio_session_read_header(task, &hdr) != 0)
		return -1;
	task->imsg.sn = hdr.serial_num;

	/* free the outgoing message */
	kfree(task->sender_task->omsg);
	task->sender_task->omsg = NULL;

	/* read the message */
	ptr = msg->in.header.iov_base;

	/* read the payload */
	len = xio_read_uint32(&session->peer_session_id , 0, ptr);
	ptr  = ptr + len;

	len = xio_read_uint16(action, 0, ptr);
	ptr = ptr + len;

	switch (*action) {
	case XIO_ACTION_ACCEPT:
		len = xio_read_uint16(&session->portals_array_len, 0, ptr);
		ptr = ptr + len;

		len = xio_read_uint16(&rsp->user_context_len, 0, ptr);
		ptr = ptr + len;

		if (session->portals_array_len) {
			session->portals_array = kcalloc(
					session->portals_array_len,
				       sizeof(char *), GFP_KERNEL);
			if (session->portals_array == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}
			for (i = 0; i < session->portals_array_len; i++) {
				len = xio_read_uint16(&str_len, 0, ptr);
				ptr = ptr + len;

				session->portals_array[i] =
					kstrndup((char *)ptr, str_len,
						 GFP_KERNEL);
				session->portals_array[i][str_len] = 0;
				ptr = ptr + str_len;
			}

		} else {
			session->portals_array = NULL;
		}

		if (session->new_ses_rsp.user_context_len) {
			rsp->user_context = kcalloc(rsp->user_context_len,
					sizeof(uint8_t), GFP_KERNEL);
			if (rsp->user_context == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}

			len = xio_read_array(rsp->user_context,
					rsp->user_context_len, 0, ptr);
			ptr = ptr + len;
		} else {
			rsp->user_context = NULL;
		}
		break;
	case XIO_ACTION_REDIRECT:
		len = xio_read_uint16(&session->services_array_len, 0, ptr);
		ptr = ptr + len;

		len = xio_read_uint16(&rsp->user_context_len, 0, ptr);
		ptr = ptr + len;

		if (session->services_array_len) {
			session->services_array = kcalloc(
					session->services_array_len,
					sizeof(char *), GFP_KERNEL);
			if (session->services_array == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}

			for (i = 0; i < session->services_array_len; i++) {
				len = xio_read_uint16(&str_len, 0, ptr);
				ptr = ptr + len;

				session->services_array[i] =
					kstrndup((char *)ptr, str_len,
						 GFP_KERNEL);
				session->services_array[i][str_len] = 0;
				ptr = ptr + str_len;
			}

		} else {
			session->services_array = NULL;
		}
		break;

	case XIO_ACTION_REJECT:
		len = xio_read_uint32(&session->reject_reason , 0, ptr);
		ptr  = ptr + len;

		len = xio_read_uint16(&rsp->user_context_len, 0, ptr);
		ptr = ptr + len;

		if (session->new_ses_rsp.user_context_len) {
			rsp->user_context = kcalloc(rsp->user_context_len,
					sizeof(uint8_t), GFP_KERNEL);
			if (rsp->user_context == NULL) {
				ERROR_LOG("allocation failed\n");
				xio_set_error(ENOMEM);
				return -1;
			}

			len = xio_read_array(rsp->user_context,
					rsp->user_context_len, 0, ptr);
			ptr = ptr + len;
		} else {
			rsp->user_context = NULL;
		}
		break;
	default:
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_setup_rsp_recv			                             */
/*---------------------------------------------------------------------------*/
static int xio_on_setup_rsp_recv(struct xio_connection *connection,
				 struct xio_task *task)
{
	uint16_t			action = 0;
	struct xio_session		*session = connection->session;
	struct xio_new_session_rsp	*rsp = &session->new_ses_rsp;
	struct xio_connection		*tmp_connection;
	int				retval = 0;

	retval = xio_read_setup_rsp(connection, task, &action);

	/* the tx task is returend back to pool */
	xio_tasks_pool_put(task->sender_task);
	task->sender_task = NULL;

	xio_tasks_pool_put(task);
	DEBUG_LOG("task recycled\n");

	if (retval != 0) {
		ERROR_LOG("failed to read setup response\n");
		return -1;
	}

	switch (action) {
	case XIO_ACTION_ACCEPT:
		if (session->portals_array == NULL)  {
			kfree(rsp->user_context);
			rsp->user_context = NULL;

			session->state = XIO_SESSION_STATE_ONLINE;

			TRACE_LOG("session state is now ONLINE. session:%p\n",
				  session);

			tmp_connection =
				xio_session_assign_conn(session,
							connection->conn);
			if (connection == session->lead_conn)
				session->lead_conn = NULL;
			else
				session->redir_conn = NULL;

			/* now try to send */
			xio_connection_set_state(connection,
						 CONNECTION_STATE_ONLINE);
			xio_connection_xmit_msgs(connection);

			/* notify the upper layer */
			if (session->ses_ops.on_session_established)
				session->ses_ops.on_session_established(
						session, rsp,
						session->cb_user_context);
			return 0;
		} else { /* reconnect to peer other session */
			TRACE_LOG("session state is now ACCEPT. session:%p\n",
				  session);

			/* re - initialize the redirected connection */
			session->lead_conn = xio_connection_init(
					session,
					session->lead_conn->ctx,
					session->lead_conn->conn_idx,
					session->lead_conn->cb_user_context);

			xio_connection_set_conn(session->lead_conn,
						connection->conn);
			/* close the lead/redirected connection */
			xio_conn_close(connection->conn, &session->observer);
			connection->conn = NULL;

			session->state = XIO_SESSION_STATE_ACCEPTED;
			/* open new connections */
			retval = xio_session_accept_connection(session);
			if (retval != 0) {
				ERROR_LOG("failed to accept connection\n");
				return -1;
			}
			return 0;
		}
		break;
	case XIO_ACTION_REDIRECT:
		TRACE_LOG("session state is now REDIRECT. session:%p\n",
			  session);

		session->state = XIO_SESSION_STATE_REDIRECTED;

		/* open new connections */
		retval = xio_session_redirect_connection(session);
		if (retval != 0) {
			ERROR_LOG("failed to redirect connection\n");
			return -1;
		}

		/* close the lead connection */
		xio_conn_close(session->lead_conn->conn,
			       &session->observer);

		return 0;
		break;
	case XIO_ACTION_REJECT:

		kfree(rsp->user_context);
		rsp->user_context = NULL;

		tmp_connection =
			xio_session_assign_conn(session,
						connection->conn);

		/* close the old connection */
		xio_conn_close(connection->conn, &session->observer);

		tmp_connection->conn = NULL;
		connection = tmp_connection;

		session->state = XIO_SESSION_STATE_REJECTED;

		TRACE_LOG("session state is now REJECT. session:%p\n",
			  session);

		return xio_on_connection_rejected(session, connection);
		break;
	}

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_on_setup_rsp_send_comp			                             */
/*---------------------------------------------------------------------------*/
static int xio_on_setup_rsp_send_comp(
		struct xio_connection *connection,
		struct xio_task *task)
{
	/* recycle the task */
	xio_tasks_pool_put(task);

	/* time to set new callback */
	DEBUG_LOG("task recycled\n");

	if (connection->session->state == XIO_SESSION_STATE_CONNECT) {
		connection->session->state = XIO_SESSION_STATE_ONLINE;
		TRACE_LOG("session state changed to ONLINE. session:%p\n",
			  connection->session);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_fin_req_recv				                             */
/*---------------------------------------------------------------------------*/
static int xio_on_fin_req_recv(struct xio_connection *connection,
		struct xio_task *task)
{
	xio_ack_disconnect(connection, task);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_fin_rsp_recv				                             */
/*---------------------------------------------------------------------------*/
static int xio_on_fin_rsp_recv(struct xio_connection *connection,
		struct xio_task *task)
{
	xio_do_disconnect(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_fin_send_comp							     */
/*---------------------------------------------------------------------------*/
static int xio_on_fin_send_comp(
		struct xio_connection *connection,
		struct xio_task *task)
{
	xio_connection_release_fin(connection, task->omsg);

	/* now try to send */
	xio_connection_xmit_msgs(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_req_recv				                             */
/*---------------------------------------------------------------------------*/
static int xio_on_req_recv(struct xio_connection *connection,
		struct xio_task *task)
{
	struct xio_session_hdr	hdr;
	struct xio_msg		*msg = &task->imsg;

	/* server side after accept change the state upon first message */
	if (connection->session->state == XIO_SESSION_STATE_ACCEPTED)
		connection->session->state = XIO_SESSION_STATE_ONLINE;

	/* read session header */
	if (xio_session_read_header(task, &hdr) != 0)
		return -1;

	msg->sn = hdr.serial_num;
	msg->flags = hdr.flags;
	task->connection = connection;

	xio_connection_queue_io_task(connection, task);

	task->state = XIO_TASK_STATE_DELIVERED;

	/* add reference count to protect against release in callback */
	/* add ref to task avoiding race when user call release or send
	 * completion
	 */
	if (hdr.flags & XIO_MSG_FLAG_REQUEST_READ_RECEIPT)
		xio_task_addref(task);

	/* notify the upper layer */
	if (connection->ses_ops.on_msg)
		connection->ses_ops.on_msg(
				connection->session, msg,
				msg->more_in_batch,
				connection->cb_user_context);

	if (hdr.flags & XIO_MSG_FLAG_REQUEST_READ_RECEIPT) {
		if (task->state == XIO_TASK_STATE_DELIVERED) {
			xio_connection_send_read_receipt(connection, msg);
		} else {
			/* free the ref added in this function */
			xio_tasks_pool_put(task);
		}
	}

	/* now try to send */
	xio_connection_xmit_msgs(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_rsp_recv				                             */
/*---------------------------------------------------------------------------*/
static int xio_on_rsp_recv(struct xio_connection *connection,
			   struct xio_task *task)
{
	struct xio_session_hdr	hdr;
	struct xio_msg		*msg = &task->imsg;
	struct xio_msg		*omsg;
	struct xio_task		*sender_task = task->sender_task;

	/* read session header */
	if (xio_session_read_header(task, &hdr) != 0)
		return -1;

	msg->sn = hdr.serial_num;

	/* one way messages do not have sender task */
	omsg = sender_task->omsg;
	omsg->request = msg;

	task->connection = connection;

	/* store the task in io queue */
	xio_connection_queue_io_task(connection, task);

	if (task->tlv_type == XIO_ONE_WAY_RSP) {
		if (!(hdr.flags & XIO_MSG_RSP_FLAG_FIRST))
			ERROR_LOG("protocol requires first flag to be set. " \
				  "flags:0x%x\n", hdr.flags);
		omsg->sn	  = msg->sn; /* one way do have response */
		omsg->receipt_res = hdr.receipt_result;
		if (connection->ses_ops.on_msg_delivered)
			connection->ses_ops.on_msg_delivered(
				    connection->session,
				    omsg,
				    task->imsg.more_in_batch,
				    connection->cb_user_context);
	    sender_task->omsg = NULL;
	    xio_release_response_task(task);
	} else   {
		if (hdr.flags & XIO_MSG_RSP_FLAG_FIRST) {
			if (connection->ses_ops.on_msg_delivered) {
				omsg->receipt_res = hdr.receipt_result;
				connection->ses_ops.on_msg_delivered(
						connection->session,
						omsg,
						task->imsg.more_in_batch,
						connection->cb_user_context);
			}
			/* standalone receipt */
			if ((hdr.flags &
			    (XIO_MSG_RSP_FLAG_FIRST | XIO_MSG_RSP_FLAG_LAST)) ==
					XIO_MSG_RSP_FLAG_FIRST) {
				/* recycle the receipt */
				xio_tasks_pool_put(task);
			}
		}
		if (hdr.flags & XIO_MSG_RSP_FLAG_LAST) {
			if (connection->ses_ops.on_msg)
				connection->ses_ops.on_msg(
					connection->session,
					omsg,
					task->imsg.more_in_batch,
					connection->cb_user_context);
		}
	}

	/* now try to send */
	xio_connection_xmit_msgs(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_rsp_send_comp				                             */
/*---------------------------------------------------------------------------*/
static int xio_on_rsp_send_comp(
		struct xio_connection *connection,
		struct xio_task *task)
{
	/*
	 * completion of receipt
	 */
	if ((task->omsg_flags &
	    (XIO_MSG_RSP_FLAG_FIRST | XIO_MSG_RSP_FLAG_LAST)) ==
	     XIO_MSG_RSP_FLAG_FIRST) {
		xio_connection_release_read_receipt(connection, task->omsg);
		xio_release_response_task(task);
	} else {
		/* send completion notification only to responder to
		 * release responses
		 */
		if (connection->ses_ops.on_msg_send_complete) {
			connection->ses_ops.on_msg_send_complete(
					connection->session, task->omsg,
					connection->cb_user_context);
		}
		/* recycle the task */
		xio_tasks_pool_put(task);
	}

	/* now try to send */
	xio_connection_xmit_msgs(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_ow_req_send_comp				                     */
/*---------------------------------------------------------------------------*/
static int xio_on_ow_req_send_comp(
		struct xio_connection *connection,
		struct xio_task *task)
{
	/* recycle the task */
	if (!(task->omsg_flags & XIO_MSG_FLAG_REQUEST_READ_RECEIPT))
		xio_tasks_pool_put(task);

	/* now try to send */
	xio_connection_xmit_msgs(connection);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_conn_disconnected			                             */
/*---------------------------------------------------------------------------*/
static int xio_on_conn_disconnected(struct xio_session *session,
				      struct xio_conn *conn,
				      union xio_conn_event_data *event_data)
{
	struct xio_connection		*connection, *tmp_connection;
	struct xio_session_event_data	event;

	if (session->lead_conn && session->lead_conn->conn == conn)
		connection = session->lead_conn;
	else
		connection = xio_session_find_conn(session, conn);

	if (connection && connection->conn) {
		connection->state = CONNECTION_STATE_DISCONNECT;
		if (session->type == XIO_SESSION_REQ) {
			event.event = XIO_SESSION_CONNECTION_DISCONNECTED_EVENT;
			event.reason = XIO_E_SUCCESS;
			event.conn = connection;
			event.conn_user_context = connection->cb_user_context;

			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &event,
						session->cb_user_context);
		} else if (session->type == XIO_SESSION_REP) {
			xio_session_disconnect(session, connection);
		}
	} else {
		xio_conn_close(conn, &session->observer);
	}
	if (session->type == XIO_SESSION_REQ) {
		/* only on client */
		/* also send disconnect to connections that do no have conn */
		list_for_each_entry_safe(connection, tmp_connection,
					 &session->connections_list,
					 connections_list_entry) {
			if (connection && !connection->conn) {
				event.event =
				     XIO_SESSION_CONNECTION_DISCONNECTED_EVENT;
				event.reason =
					XIO_E_SUCCESS;
				event.conn = connection;
				event.conn_user_context =
					connection->cb_user_context;
				if (session->ses_ops.on_session_event)
					session->ses_ops.on_session_event(
						session, &event,
						session->cb_user_context);
			}
		}
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_session_notify_teardown						     */
/*---------------------------------------------------------------------------*/
static void xio_session_notify_teardown(struct xio_session *session, int reason)
{
	struct xio_session_event_data  event = {
		.event = XIO_SESSION_TEARDOWN_EVENT,
		.reason = reason
	};
	if (session->ses_ops.on_session_event)
		session->ses_ops.on_session_event(
				session, &event,
				session->cb_user_context);
}

/*---------------------------------------------------------------------------*/
/* xio_on_conn_closed							     */
/*---------------------------------------------------------------------------*/
static int xio_on_conn_closed(struct xio_session *session,
			      struct xio_conn *conn,
			      union xio_conn_event_data *event_data)
{
	struct xio_connection		*connection;
	int				teardown = 0;
	int				reason;

	INFO_LOG("session:%p - conn:%p close complete\n", session, conn);

	switch (session->state) {
	case XIO_SESSION_STATE_ACCEPTED:
		if (session->type == XIO_SESSION_REP)
			reason = XIO_E_SESSION_DISCONECTED;
		else
			reason = XIO_E_SESSION_REFUSED;
		break;
	default:
		reason = XIO_E_SESSION_DISCONECTED;
		break;
	}

	/* leading connection */
	if (session->lead_conn && session->lead_conn->conn == conn) {
		xio_connection_close(session->lead_conn);
		session->lead_conn = NULL;
		if (session->type == XIO_SESSION_REP) {
			/* do not notify teardown if no messages arrived
			 * on multithread - leave the session unteared
			 * until messages arrived or session timeout
			 */
			return 0;
		}
		TRACE_LOG("lead connection is closed\n");
		spin_lock(&session->conn_list_lock);
		teardown = (session->conns_nr == 0);
		spin_unlock(&session->conn_list_lock);
	} else if (session->redir_conn && session->redir_conn->conn == conn) {
		xio_connection_close(session->redir_conn);
		session->redir_conn = NULL;
		TRACE_LOG("redirected connection is closed\n");
		spin_lock(&session->conn_list_lock);
		teardown = (session->conns_nr == 0);
		spin_unlock(&session->conn_list_lock);
	} else {
		connection = xio_session_find_conn(session, conn);
		xio_connection_flush(connection);
		if (session->type == XIO_SESSION_REQ) {
			struct xio_session_event_data  event = {
				.event = XIO_SESSION_CONNECTION_CLOSED_EVENT,
				.reason = XIO_E_SUCCESS,
				.conn = connection,
				.conn_user_context = connection->cb_user_context
			};
			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &event,
						session->cb_user_context);
		}
		spin_lock(&session->conn_list_lock);
		teardown = (session->conns_nr == 1);
		spin_unlock(&session->conn_list_lock);
		xio_session_free_conn(connection);
	}
	if (teardown && !session->lead_conn && !session->redir_conn) {
			xio_session_notify_teardown(
				session,
				reason);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_conn_refused							     */
/*---------------------------------------------------------------------------*/
static int xio_on_conn_refused(struct xio_session *session,
				  struct xio_conn *conn,
				  union xio_conn_event_data *event_data)
{
	struct xio_connection *connection, *tmp_connection;
	struct xio_session_event_data ev_data = {
		.event	=	XIO_SESSION_CONNECTION_DISCONNECTED_EVENT,
		.reason =	XIO_E_SESSION_REFUSED
	};

	if ((session->state == XIO_SESSION_STATE_CONNECT) ||
	    (session->state == XIO_SESSION_STATE_REDIRECTED)) {
		list_for_each_entry_safe(connection, tmp_connection,
					 &session->connections_list,
					 connections_list_entry) {
			if (session->lead_conn == connection)
				session->lead_conn = NULL;
			else if (session->redir_conn == connection)
				session->redir_conn = NULL;

			ev_data.conn =  connection;
			ev_data.conn_user_context = connection->cb_user_context;

			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &ev_data,
						session->cb_user_context);
		}
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_conn_established						     */
/*---------------------------------------------------------------------------*/
static int xio_on_conn_established(struct xio_session *session,
		struct xio_conn *conn,
		union xio_conn_event_data *event_data)
{
	int				retval = 0;
	struct xio_connection		*connection;
	struct xio_session_event_data	ev_data = {
		.event	=	XIO_SESSION_ERROR_EVENT,
		.reason =	XIO_E_SESSION_REFUSED
	};

	switch (session->state) {
	case XIO_SESSION_STATE_CONNECT:
		{
			struct xio_msg *msg =
				xio_session_write_setup_req(session);
			if (msg == NULL) {
				ERROR_LOG("setup request creation failed\n");
				return -1;
			}
			session->state = XIO_SESSION_STATE_CONNECT;

			msg->type = XIO_SESSION_SETUP_REQ;
			retval = xio_connection_send(session->lead_conn, msg);
			if (retval) {
				TRACE_LOG("failed to send session "\
					  "setup request\n");
				ev_data.conn =  session->lead_conn;
				ev_data.conn_user_context =
					session->lead_conn->cb_user_context;
				if (session->ses_ops.on_session_event)
					session->ses_ops.on_session_event(
						session, &ev_data,
						session->cb_user_context);
			}
		}
		break;
	case XIO_SESSION_STATE_REDIRECTED:
		{
			struct xio_msg *msg =
				xio_session_write_setup_req(session);
			if (msg == NULL) {
				ERROR_LOG("setup request creation failed\n");
				return -1;
			}
			session->state = XIO_SESSION_STATE_CONNECT;

			msg->type      = XIO_SESSION_SETUP_REQ;

			retval = xio_connection_send(session->redir_conn, msg);
			if (retval) {
				TRACE_LOG("failed to send session " \
					  "setup request\n");
				ev_data.conn =  session->redir_conn;
				ev_data.conn_user_context =
					session->redir_conn->cb_user_context;
				if (session->ses_ops.on_session_event)
					session->ses_ops.on_session_event(
						session, &ev_data,
						session->cb_user_context);
			}
		}
		break;
	case XIO_SESSION_STATE_ACCEPTED:
		{
			int is_last = 1;

			connection = xio_session_find_conn(session, conn);
			if (connection == NULL) {
				ERROR_LOG("failed to find connection conn:%p\n",
					  conn);
				return -1;
			}

			/* set the new connection to online */
			xio_connection_set_state(connection,
						 CONNECTION_STATE_ONLINE);

			/* is this the last to accept */
			list_for_each_entry(connection,
					    &session->connections_list,
					    connections_list_entry) {
				if (connection->state !=
						CONNECTION_STATE_ONLINE) {
					is_last = 0;
					break;
				}
			}
			if (is_last) {
				session->state = XIO_SESSION_STATE_ONLINE;
				TRACE_LOG(
				  "session state is now ONLINE. session:%p\n",
				  session);

				if (session->ses_ops.on_session_established)
					session->ses_ops.on_session_established(
						session, &session->new_ses_rsp,
						session->cb_user_context);

				kfree(session->new_ses_rsp.user_context);

					/* now try to send */
				list_for_each_entry(connection,
						    &session->connections_list,
						    connections_list_entry) {
					TRACE_LOG(
					   "connection established: " \
					   "connection:%p, session:%p, conn:%p\n",
					   connection, connection->session,
					   connection->conn);
					xio_connection_xmit_msgs(connection);
				}
			}
		}
		break;
	case XIO_SESSION_STATE_ONLINE:
		connection = xio_session_find_conn(session, conn);
		if (connection == NULL)  {
			ERROR_LOG("failed to find connection\n");
			return -1;
		}
		DEBUG_LOG("connection established: " \
			  "connection:%p, session:%p, conn:%p\n",
			   connection, connection->session,
			   connection->conn);
		/* now try to send */
		xio_connection_set_state(connection, CONNECTION_STATE_ONLINE);
		xio_connection_xmit_msgs(connection);
	default:
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_conn_error							     */
/*---------------------------------------------------------------------------*/
static int xio_on_conn_error(struct xio_session *session,
			     struct xio_conn *conn,
			     union xio_conn_event_data *event_data)
{
	struct xio_session_event_data ev_data = {
		.event	= XIO_SESSION_CONNECTION_ERROR_EVENT,
		.reason = event_data->error.reason

	};
	struct xio_session  *the_session = session;
	struct xio_connection *connection =
				xio_session_find_conn(session, conn);

	ev_data.conn =  connection;
	ev_data.conn_user_context =
		(connection) ? connection->cb_user_context : NULL;

	if (session->ses_ops.on_session_event)
		session->ses_ops.on_session_event(
				the_session, &ev_data,
				session->cb_user_context);

	return 0;
}




/*---------------------------------------------------------------------------*/
/* xio_on_new_message							     */
/*---------------------------------------------------------------------------*/
static int xio_on_new_message(struct xio_session *session,
				  struct xio_conn *conn,
				  union xio_conn_event_data *event_data)
{
	struct xio_task	*task  = event_data->msg.task;
	struct xio_connection	*connection;
	int			retval = -1;
	int			tlv_type;

	if (session == NULL) {
		session = xio_find_session(task);
		if (session == NULL) {
			ERROR_LOG("failed to find session\n");
			xio_tasks_pool_put(task);
			return -1;
		}
	}
	connection = xio_session_find_conn(session, conn);
	if (connection == NULL) {
		/* leading connection is refused */
		if (session->lead_conn && session->lead_conn->conn == conn) {
			connection = session->lead_conn;
		} else if (session->redir_conn &&
			   session->redir_conn->conn == conn) {
			/* redirected connection is refused */
			connection = session->redir_conn;
		} else {
			/* on server assign the new connection */
			if (session->type == XIO_SESSION_REP) {
				connection = xio_server_create_accepted_conn(
							session, conn);
				/* new user to the connection */
				xio_conn_addref(conn);
			} else {
				connection =
					xio_session_assign_conn(session, conn);
			}
			if (connection == NULL) {
				ERROR_LOG("failed to find connection " \
					  "session:%p. conn:%p\n",
					  session,
					  conn);
				xio_tasks_pool_put(task);
				return -1;
			}
		}
	}

	tlv_type = task->tlv_type;
	switch (tlv_type) {
	case XIO_MSG_REQ:
	case XIO_ONE_WAY_REQ:
		retval = xio_on_req_recv(connection, task);
		break;
	case XIO_MSG_RSP:
	case XIO_ONE_WAY_RSP:
		retval = xio_on_rsp_recv(connection, task);
		break;
	case XIO_FIN_REQ:
		retval = xio_on_fin_req_recv(connection, task);
		break;
	case XIO_FIN_RSP:
		retval = xio_on_fin_rsp_recv(connection, task);
		break;
	case XIO_SESSION_SETUP_REQ:
		retval = xio_on_setup_req_recv(connection, task);
		break;
	case XIO_SESSION_SETUP_RSP:
		retval = xio_on_setup_rsp_recv(connection, task);
		break;
	default:
		retval = -1;
		break;
	}

	if (retval != 0)
		ERROR_LOG("receiving new message failed. type:0x%x\n",
			  tlv_type);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_send_completion						     */
/*---------------------------------------------------------------------------*/
static int xio_on_send_completion(struct xio_session *session,
				  struct xio_conn *conn,
				  union xio_conn_event_data *event_data)
{
	struct xio_task	*task  = event_data->msg.task;
	struct xio_connection	*connection;
	int			retval = -1;

	connection = task->connection;
	if (connection == NULL) {
		connection = xio_session_assign_conn(session, conn);
		if (connection == NULL) {
			ERROR_LOG("failed to find connection conn:%p. " \
					"dropping message type:0x%x\n",
					conn,
					task->tlv_type);
			xio_tasks_pool_put(task);
			return -1;
		}
	}

	switch (task->tlv_type) {
	case XIO_MSG_REQ:
	case XIO_SESSION_SETUP_REQ:
		retval = 0;
		break;
	case XIO_MSG_RSP:
	case XIO_ONE_WAY_RSP:
		retval = xio_on_rsp_send_comp(connection, task);
		break;
	case XIO_ONE_WAY_REQ:
		retval = xio_on_ow_req_send_comp(connection, task);
		break;
	case XIO_FIN_REQ:
	case XIO_FIN_RSP:
		retval = xio_on_fin_send_comp(connection, task);
		break;
	case XIO_SESSION_SETUP_RSP:
		retval = xio_on_setup_rsp_send_comp(connection, task);
		break;
	default:
		break;
	}

	if (retval != 0)
		ERROR_LOG("message send completion failed. type:0x%x\n",
			  task->tlv_type);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_assign_in_buf							     */
/*---------------------------------------------------------------------------*/
static int xio_on_assign_in_buf(struct xio_session *session,
				struct xio_conn *conn,
				union xio_conn_event_data *event_data)
{
	struct xio_task	*task  = event_data->assign_in_buf.task;
	struct xio_connection	*connection;

	if (session == NULL)
		session = xio_find_session(task);

	connection = xio_session_find_conn(session, conn);
	if (connection == NULL) {
		connection = xio_session_assign_conn(session, conn);
		if (connection == NULL) {
			ERROR_LOG("failed to find connection :%p. " \
				  "dropping message:%d\n", conn,
				  event_data->msg.op);
			return -1;
		}
	}

	if (connection->ses_ops.assign_data_in_buf) {
		connection->ses_ops.assign_data_in_buf(&task->imsg,
		connection->cb_user_context);
		event_data->assign_in_buf.is_assigned = 1;
		return 0;
	}
	event_data->assign_in_buf.is_assigned = 0;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_cancel_request						     */
/*---------------------------------------------------------------------------*/
static int xio_on_cancel_request(struct xio_session *sess,
				 struct xio_conn *conn,
				 union xio_conn_event_data *event_data)
{
	struct xio_session_cancel_hdr	hdr;
	struct xio_msg			req;
	struct xio_session_cancel_hdr	*tmp_hdr;
	struct xio_session		*session;
	struct xio_connection		*connection;
	struct xio_task			*task;
	struct xio_observer		*observer;


	tmp_hdr			 = event_data->cancel.ulp_msg;
	hdr.sn			 = ntohll(tmp_hdr->sn);
	hdr.responder_session_id = ntohl(tmp_hdr->responder_session_id);

	observer = xio_conn_observer_lookup(conn, hdr.responder_session_id);
	if (observer == NULL) {
		ERROR_LOG("failed to find session\n");
		return -1;
	}

	session = observer->impl;

	connection = xio_session_find_conn(session, conn);
	if (connection == NULL) {
		ERROR_LOG("failed to find session\n");
		return -1;
	}

	/* lookup for task in io list */
	task = xio_connection_find_io_task(connection, hdr.sn);
	if (task) {
		if (connection->ses_ops.on_cancel_request) {
			connection->ses_ops.on_cancel_request(
				connection->session,
				&task->imsg,
				connection->cb_user_context);
			return 0;
		} else {
			WARN_LOG("cancel is not supported on responder\n");
		}
	}

	TRACE_LOG("message to cancel not found %llu\n", hdr.sn);

	req.sn	= hdr.sn;
	xio_connection_send_cancel_response(connection, &req, NULL,
					    XIO_E_MSG_NOT_FOUND);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_on_cancel_response						     */
/*---------------------------------------------------------------------------*/
static int xio_on_cancel_response(struct xio_session *sess,
				  struct xio_conn *conn,
				  union xio_conn_event_data *event_data)
{
	struct xio_session_cancel_hdr	hdr;
	struct xio_session_cancel_hdr	*tmp_hdr;
	struct xio_observer		*observer;
	struct xio_session		*session;
	struct xio_connection		*connection;
	struct xio_msg			msg;
	struct xio_msg			*pmsg;


	if (event_data->cancel.task == NULL) {
		tmp_hdr			 = event_data->cancel.ulp_msg;
		hdr.sn			 = ntohll(tmp_hdr->sn);
		hdr.requester_session_id = ntohl(tmp_hdr->requester_session_id);

		observer = xio_conn_observer_lookup(conn,
						    hdr.requester_session_id);
		if (observer == NULL) {
			ERROR_LOG("failed to find session\n");
			return -1;
		}
		session = observer->impl;

		pmsg		= &msg;		/* fake a message */
		msg.sn		= hdr.sn;
		msg.status	= 0;
	} else {
		session		= event_data->cancel.task->session;
		pmsg		= event_data->cancel.task->omsg;
		hdr.sn		= pmsg->sn;
	}

	connection = xio_session_find_conn(session, conn);
	if (connection == NULL) {
		ERROR_LOG("failed to find session\n");
		return -1;
	}

	/* need to release the last reference since answer is not expected */
	if (event_data->cancel.result == XIO_E_MSG_CANCELED)
		xio_tasks_pool_put(event_data->cancel.task);

	if (connection->ses_ops.on_cancel)
		connection->ses_ops.on_cancel(
				session,
				pmsg,
				event_data->cancel.result,
				connection->cb_user_context);
	else
		ERROR_LOG("cancel is not supported\n");

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_on_conn_event				                             */
/*---------------------------------------------------------------------------*/
static int xio_on_conn_event(void *observer, void *sender, int event,
			void *event_data)
{
	struct xio_session	*session = observer;
	struct xio_conn	*conn	= sender;
	int			retval  = 0;


	switch (event) {
	case XIO_CONNECTION_NEW_MESSAGE:
/*
		INFO_LOG("session: [notification] - new message. " \
			 "session:%p, conn:%p\n", observer, sender);

*/		xio_on_new_message(session, conn, event_data);
		break;
	case XIO_CONNECTION_SEND_COMPLETION:
/*		INFO_LOG("session: [notification] - send_completion. " \
			 "session:%p, conn:%p\n", observer, sender);
*/
		xio_on_send_completion(session, conn, event_data);
		break;
	case XIO_CONNECTION_ASSIGN_IN_BUF:
/*		INFO_LOG("session: [notification] - assign in buf. " \
			 "session:%p, conn:%p\n", observer, sender);
*/
		xio_on_assign_in_buf(session, conn, event_data);
		break;
	case XIO_CONNECTION_CANCEL_REQUEST:
		INFO_LOG("session: [notification] - cancel request. " \
			 "session:%p, conn:%p\n", observer, sender);
		xio_on_cancel_request(session, conn, event_data);
		break;
	case XIO_CONNECTION_CANCEL_RESPONSE:
		INFO_LOG("session: [notification] - cancel response. " \
			 "session:%p, conn:%p\n", observer, sender);
		xio_on_cancel_response(session, conn, event_data);
		break;
	case XIO_CONNECTION_ESTABLISHED:
		INFO_LOG("session: [notification] - connection established. " \
			 "session:%p, conn:%p\n", observer, sender);
		xio_on_conn_established(session, conn, event_data);
		break;
	case XIO_CONNECTION_DISCONNECTED:
		INFO_LOG("session: [notification] - connection disconnected" \
			 " session:%p, conn:%p\n", observer, sender);
		xio_on_conn_disconnected(session, conn, event_data);
		break;
	case XIO_CONNECTION_CLOSED:
		INFO_LOG("session: [notification] - connection closed. " \
			 "session:%p, conn:%p\n", observer, sender);
		xio_on_conn_closed(session, conn, event_data);
		break;
	case XIO_CONNECTION_REFUSED:
		INFO_LOG("session: [notification] - connection refused. " \
			 "session:%p, conn:%p\n", observer, sender);
		xio_on_conn_refused(session, conn, event_data);
		break;
	case XIO_CONNECTION_ERROR:
		ERROR_LOG("session: [notification] - connection error. " \
			 "session:%p, conn:%p\n", observer, sender);
		xio_on_conn_error(session, conn, event_data);
		break;
	default:
		ERROR_LOG("session: [notification] - unexpected event. " \
			 "event:%d, session:%p, conn:%p\n",
			 event, observer, sender);
		xio_on_conn_error(session, conn, event_data);
		break;
	};

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_session_init			                                     */
/*---------------------------------------------------------------------------*/
struct xio_session *xio_session_init(
		enum xio_session_type type,
		struct xio_session_attr *attr,
		const char *uri,
		uint32_t initial_sn,
		uint32_t flags,
		void *cb_user_context)
{
	struct xio_session	*session = NULL;
	int			retval;
	int			uri_len = strlen(uri);


	/* extract portal from uri */
	/* create the session */
	session = kcalloc(1, sizeof(struct xio_session), GFP_KERNEL);
	if (session == NULL) {
		ERROR_LOG("failed to create session\n");
		xio_set_error(ENOMEM);
		return NULL;
	}

	XIO_OBSERVER_INIT(&session->observer, session, xio_on_conn_event);

	INIT_LIST_HEAD(&session->connections_list);

	session->user_context_len = attr->user_context_len;

	/* copy private data if exist */
	if (session->user_context_len) {
		session->user_context = kmalloc(attr->user_context_len,
						GFP_KERNEL);
		if (session->user_context == NULL) {
			xio_set_error(ENOMEM);
			goto cleanup;
		}
		memcpy(session->user_context, attr->user_context,
		       session->user_context_len);
	}
	mutex_init(&session->lock);
	spin_lock_init(&session->conn_list_lock);

	/* fill session data*/
	session->type			= type;
	session->cb_user_context	= cb_user_context;

	session->trans_sn		= initial_sn;
	session->state			= XIO_SESSION_STATE_INIT;
	session->session_flags		= flags;

	memcpy(&session->ses_ops, attr->ses_ops,
	       sizeof(*attr->ses_ops));


	session->uri_len = uri_len;
	session->uri = kstrdup(uri, GFP_KERNEL);
	if (session->uri == NULL) {
		xio_set_error(ENOMEM);
		goto cleanup2;
	}

	/* add the session to storage */
	retval = xio_sessions_store_add(session, &session->session_id);
	if (retval != 0) {
		ERROR_LOG("adding session to sessions store failed :%p\n",
			  session);
		goto cleanup3;
	}

	return session;

cleanup3:
	kfree(session->uri);
cleanup2:
	kfree(session->user_context);
cleanup:
	kfree(session);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_session_disconnect						     */
/*---------------------------------------------------------------------------*/
int xio_session_disconnect(struct xio_session *session,
			     struct xio_connection *connection)
{
	int teardown = 0;

	/* remove the connection from the session's connections list */
	if (connection->conn) {
		xio_conn_close(connection->conn, &session->observer);
	} else {
		if ((connection->state == CONNECTION_STATE_DISCONNECT) ||
		    (connection->state == CONNECTION_STATE_CLOSE)) {
			struct xio_session_event_data  event = {
				.event = XIO_SESSION_CONNECTION_CLOSED_EVENT,
				.reason = XIO_E_SUCCESS,
				.conn = connection,
				.conn_user_context = connection->cb_user_context

			};
			spin_lock(&session->conn_list_lock);
			teardown = (session->conns_nr == 1);
			spin_unlock(&session->conn_list_lock);
			xio_session_free_conn(connection);
			if (session->ses_ops.on_session_event)
				session->ses_ops.on_session_event(
						session, &event,
						session->cb_user_context);
		} else {
			teardown = list_empty(&session->connections_list);
		}

		if (teardown && !session->lead_conn && !session->redir_conn) {
			xio_session_notify_teardown(
						session,
						XIO_E_SESSION_DISCONECTED);
		}
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_session_close							     */
/*---------------------------------------------------------------------------*/
int xio_session_close(struct xio_session *session)
{
	if (session == NULL)
		return 0;

	session->state = XIO_SESSION_STATE_CLOSING;
	if (list_empty(&session->connections_list)) {
		xio_session_release(session);
	} else {
		xio_set_error(EINVAL);
		ERROR_LOG("xio_session_close failed: not empty\n");
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_session_open			                                     */
/*---------------------------------------------------------------------------*/
struct xio_session *xio_session_open(
		enum xio_session_type type,
		struct xio_session_attr *attr,
		const char *uri,
		uint32_t initial_sn,
		uint32_t flags,
		void *cb_user_context)
{
	struct xio_session	*session = NULL;

	/* input validation */
	if (attr == NULL || uri == NULL) {
		xio_set_error(EINVAL);
		ERROR_LOG("xio_session_open: invalid parameter\n");
		return NULL;
	}

	session = xio_session_init(type, attr, uri,
				     initial_sn, flags, cb_user_context);

	if (session == NULL) {
		ERROR_LOG("failed to open session\n");
		return NULL;
	}
	return session;
}

/*---------------------------------------------------------------------------*/
/* xio_connect								     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_connect(struct xio_session  *session,
				       struct xio_context  *ctx,
				       uint32_t conn_idx,
				       void *conn_user_context)
{
	struct xio_connection  *connection = NULL, *tmp_conn;
	int			 retval;

	if ((ctx == NULL) || (session == NULL)) {
		ERROR_LOG("invalid parameters ctx:%p, session:%p\n",
			  ctx, session);
		xio_set_error(EINVAL);
		return NULL;
	}

	mutex_lock(&session->lock);

	/* only one connection per context allowed */
	connection = xio_session_find_conn_by_ctx(session, ctx);
	if (connection != NULL) {
		ERROR_LOG("context:%p, already assigned connection:%p\n",
			  ctx, connection);
		goto cleanup;
	}
	if (session->state == XIO_SESSION_STATE_INIT) {
		char portal[64];
		struct xio_conn	*conn;
		/* extract portal from uri */
		if (xio_uri_get_portal(session->uri, portal,
				       sizeof(portal)) != 0) {
			xio_set_error(EADDRNOTAVAIL);
			ERROR_LOG("parsing uri failed. uri: %s\n",
				  session->uri);
			goto cleanup;
		}
		conn = xio_conn_open(ctx, portal, &session->observer,
						  session->session_id);
		if (conn == NULL) {
			ERROR_LOG("failed to create connection\n");
			goto cleanup;
		}
		/* get transport class routines */
		session->trans_cls = xio_conn_get_trans_cls(conn);
		retval = xio_conn_connect(conn, portal);
		if (retval != 0) {
			ERROR_LOG("connection connect failed\n");
			goto cleanup;
		}

		/* initialize the lead connection */
		session->lead_conn = xio_session_alloc_conn(
				session, ctx,
				conn_idx,
				conn_user_context);
		session->lead_conn->conn = conn;

		connection  = session->lead_conn;

		session->state = XIO_SESSION_STATE_CONNECT;
	} else if (session->state == XIO_SESSION_STATE_CONNECT) {
		connection  = xio_session_alloc_conn(session,
						     ctx, conn_idx,
						     conn_user_context);
	} else if (session->state == XIO_SESSION_STATE_ONLINE ||
		   session->state == XIO_SESSION_STATE_ACCEPTED) {
		struct xio_conn *conn;
		char *portal;
		if (conn_idx == 0) {
			portal = session->portals_array[
					session->last_opened_portal++];
			if (session->last_opened_portal ==
			    session->portals_array_len)
					session->last_opened_portal = 0;
		} else {
			int pid = (conn_idx % session->portals_array_len);
			portal = session->portals_array[pid];
		}
		connection  = xio_session_alloc_conn(session, ctx,
						     conn_idx,
						     conn_user_context);
		conn = xio_conn_open(ctx, portal, &session->observer,
				     session->session_id);
		if (conn == NULL) {
			ERROR_LOG("failed to open connection\n");
			goto cleanup;
		}
		tmp_conn = xio_session_assign_conn(session, conn);
		if (tmp_conn != connection) {
			ERROR_LOG("failed to open connection conn:%p, %p %p\n",
				  conn, tmp_conn, connection);
			goto cleanup;
		}
		DEBUG_LOG("reconnecting to %s, ctx:%p\n", portal, ctx);
		retval = xio_conn_connect(conn, portal);
		if (retval != 0) {
			ERROR_LOG("connection connect failed\n");
			goto cleanup;
		}
		connection = tmp_conn;
	}
	mutex_unlock(&session->lock);

	return connection;

cleanup:
	mutex_unlock(&session->lock);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_session_assign_ops						     */
/*---------------------------------------------------------------------------*/
void xio_session_assign_ops(struct xio_session *session,
		struct xio_session_ops *ops)
{
	memcpy(&session->ses_ops, ops, sizeof(*ops));
}

/*---------------------------------------------------------------------------*/
/* xio_session_event_str						     */
/*---------------------------------------------------------------------------*/
const char *xio_session_event_str(enum xio_session_event event)
{
	switch (event) {
	case XIO_SESSION_REJECT_EVENT:
		return "session reject";
	case XIO_SESSION_TEARDOWN_EVENT:
		return "session teardown";
	case XIO_SESSION_CONNECTION_CLOSED_EVENT:
		return "connection closed";
	case XIO_SESSION_CONNECTION_DISCONNECTED_EVENT:
		return "connection disconnected";
	case XIO_SESSION_CONNECTION_ERROR_EVENT:
		return "connection error";
	case XIO_SESSION_ERROR_EVENT:
		return "session error";
	};
	return "unknown session event";
}

/*---------------------------------------------------------------------------*/
/* xio_get_connection							     */
/*---------------------------------------------------------------------------*/
struct xio_connection *xio_get_connection(
		struct xio_session *session,
		struct xio_context *ctx)
{
	return  xio_session_find_conn_by_ctx(session, ctx);
}

/*---------------------------------------------------------------------------*/
/* xio_session_notify_cancel						     */
/*---------------------------------------------------------------------------*/
int xio_session_notify_cancel(struct xio_connection *connection,
			      struct xio_msg *req, enum xio_status result)
{
	/* notify the upper layer */
	if (connection->ses_ops.on_cancel)
		connection->ses_ops.on_cancel(
				connection->session, req,
				result,
				connection->cb_user_context);

	return 0;
}

