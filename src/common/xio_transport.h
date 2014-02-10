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
#ifndef XIO_TRANSPORT_H
#define XIO_TRANSPORT_H

/*---------------------------------------------------------------------------*/
/* forward declarations	                                                     */
/*---------------------------------------------------------------------------*/
struct xio_task;
struct xio_task_pool;
struct xio_observer;
struct xio_observable;
struct xio_tasks_pool_ops;

/*---------------------------------------------------------------------------*/
/* enums								     */
/*---------------------------------------------------------------------------*/
enum xio_transport_event {
	XIO_TRANSPORT_NEW_CONNECTION,
	XIO_TRANSPORT_ESTABLISHED,
	XIO_TRANSPORT_DISCONNECTED,
	XIO_TRANSPORT_CLOSED,
	XIO_TRANSPORT_REFUSED,
	XIO_TRANSPORT_NEW_MESSAGE,
	XIO_TRANSPORT_SEND_COMPLETION,
	XIO_TRANSPORT_ASSIGN_IN_BUF,
	XIO_TRANSPORT_CANCEL_REQUEST,
	XIO_TRANSPORT_CANCEL_RESPONSE,
	XIO_TRANSPORT_MESSAGE_ERROR,
	XIO_TRANSPORT_ERROR,
};

enum xio_transport_opt {
	XIO_TRANSPORT_OPT_MSG_ATTR,
};

/*---------------------------------------------------------------------------*/
/* unions and structs	                                                     */
/*---------------------------------------------------------------------------*/
union xio_transport_event_data {
	struct {
		struct xio_task		*task;
		enum xio_wc_op		op;
		int			pad;
	} msg;
	struct {
		struct xio_task	 *task;
		int		 is_assigned;
		int		 pad;
	} assign_in_buf;
	struct {
		void		*ulp_msg;
		size_t		ulp_msg_sz;
		struct xio_task	*task;
		enum xio_status	result;
		int		pad;
	} cancel;
	struct {
		struct xio_transport_base	*child_trans_hndl;
	} new_connection;
	struct {
		uint32_t	cid;
	} established;
	struct {
		struct xio_task	 *task;
		enum xio_status	reason;
		int		pad;
	} msg_error;
	struct {
		enum xio_status	reason;
	} error;
};

struct xio_transport_base {
	struct xio_context		*ctx;
	struct xio_observable		observable;
	uint32_t			is_client;  /* client or server */
	atomic_t			refcnt;
	char				*portal_uri;
	struct sockaddr_storage		peer_addr;
	enum   xio_proto		proto;
	int				pad;
};

struct xio_transport_msg_validators_cls {
	int	(*is_valid_in_req)(struct xio_msg *msg);
	int	(*is_valid_out_msg)(struct xio_msg *msg);
};

struct xio_tasks_pool_ops {
	void	(*pool_get_params)(struct xio_transport_base *transport_hndl,
				int *pool_len, int *pool_dd_sz,
				int *task_dd_size);
	int	(*pool_alloc)(struct xio_transport_base *trans_hndl,
				int max, void *pool_dd_data);
	int	(*pool_free)(struct xio_transport_base *trans_hndl,
				void *pool_dd_data);
	int	(*pool_init_item)(struct xio_transport_base *trans_hndl,
				void *pool_dd_data, struct xio_task *task);
	int	(*pool_run)(struct xio_transport_base *trans_hndl);

	int	(*pre_put)(struct xio_transport_base *trans_hndl,
			struct xio_task *task);
	int	(*post_get)(struct xio_transport_base *trans_hndl,
			struct xio_task *task);
};

struct xio_tasks_pool_cls {
	void		*pool;
	struct xio_task * (*task_alloc)(void *pool);
	void		  (*task_free)(struct xio_task *task);

	struct xio_task	* (*task_lookup)(void *pool, int task_id);
};

struct xio_transport {
	const char				*name;

	struct xio_transport_msg_validators_cls	validators_cls;

	/* transport initialization */
	int	(*init)(struct xio_transport *self);
	void	(*release)(struct xio_transport *self);

	/* running thread (context) is going down */
	int	(*context_shutdown)(struct xio_transport_base *trans_hndl,
				    struct xio_context *ctx);

	/* observers */
	void	(*reg_observer)(struct xio_transport_base *trans_hndl,
				struct xio_observer *observer);
	void	(*unreg_observer)(struct xio_transport_base *trans_hndl,
				   struct xio_observer *observer);

	/* task pools managment */
	void	(*get_pools_setup_ops)(struct xio_transport_base *trans_hndl,
				struct xio_tasks_pool_ops **initial_pool_ops,
				struct xio_tasks_pool_ops **primary_pool_ops);

	void	(*set_pools_cls)(struct xio_transport_base *trans_hndl,
				struct xio_tasks_pool_cls *initial_pool_cls,
				struct xio_tasks_pool_cls *primary_pool_cls);

	/* connection */
	struct xio_transport_base *(*open)(struct xio_transport *self,
				struct xio_context *ctx,
				struct xio_observer *observer);

	int	(*connect)(struct xio_transport_base *trans_hndl,
			   const char *portal_uri,
			   const char *out_if);

	int	(*listen)(struct xio_transport_base *trans_hndl,
			  const char *portal_uri, uint16_t *src_port,
			  int backlog);

	int	(*accept)(struct xio_transport_base *trans_hndl);

	int	(*poll)(struct xio_transport_base *trans_hndl,
			long min_nr, long nr,
			struct timespec *timeout);

	int	(*reject)(struct xio_transport_base *trans_hndl);

	void	(*close)(struct xio_transport_base *trans_hndl);

	int	(*send)(struct xio_transport_base *trans_hndl,
			struct xio_task *task);

	int	(*set_opt)(void *xio_obj,
			   int optname, const void *optval, int optlen);

	int	(*get_opt)(void *xio_obj,
			   int optname, void *optval, int *optlen);

	int	(*cancel_req)(struct xio_transport_base *trans_hndl,
			      struct xio_msg *req, uint64_t stag,
			      void *ulp_msg, size_t ulp_msg_len);

	int	(*cancel_rsp)(struct xio_transport_base *trans_hndl,
			      struct xio_task *task, enum xio_status result,
			      void *ulp_msg, size_t ulp_msg_len);

	struct list_head transports_list_entry;
};

/*---------------------------------------------------------------------------*/
/* xio_transport_reg_observer	                                             */
/*---------------------------------------------------------------------------*/
static inline void xio_transport_reg_observer(
		struct xio_transport_base *trans_hndl,
		struct xio_observer *observer)
{
	xio_observable_reg_observer(&trans_hndl->observable, observer);
}

/*---------------------------------------------------------------------------*/
/* xio_transport_remove_observer	                                     */
/*---------------------------------------------------------------------------*/
static inline void xio_transport_unreg_observer(
		struct xio_transport_base *trans_hndl,
		struct xio_observer *observer)
{
	xio_observable_unreg_observer(&trans_hndl->observable, observer);
}

/*---------------------------------------------------------------------------*/
/* xio_reg_transport			                                     */
/*---------------------------------------------------------------------------*/
int xio_reg_transport(struct xio_transport *transport);

/*---------------------------------------------------------------------------*/
/* xio_unreg_transport							     */
/*---------------------------------------------------------------------------*/
void xio_unreg_transport(struct xio_transport *transport);

/*---------------------------------------------------------------------------*/
/* xio_get_transport							     */
/*---------------------------------------------------------------------------*/
struct xio_transport *xio_get_transport(const char *name);

int xio_rdma_cancel_req(struct xio_transport_base *transport,
			struct xio_msg *req, uint64_t stag,
			void *ulp_msg, size_t ulp_msg_sz);

int xio_rdma_cancel_rsp(struct xio_transport_base *transport,
			struct xio_task *task, enum xio_status result,
			void *ulp_msg, size_t ulp_msg_sz);

#endif /*XIO_TRANSPORT_H */

