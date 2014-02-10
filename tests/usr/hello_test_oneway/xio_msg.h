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
#ifndef MSG_API_H
#define MSG_API_H

#include "libxio.h"

struct msg_params {
		uint8_t		*g_hdr;
		uint8_t		*g_data;
		struct xio_mr	*g_data_mr;
		int		g_shmid;
		int		pad;
};

struct msg_pool {
	/* pool of msgs */
	struct xio_msg				**array;
	/* LIFO */
	struct xio_msg				**stack;

	struct xio_msg				**stack_ptr;
	struct xio_msg				**stack_end;
	void					*header;
	void					*data;

	struct xio_mr				*mr;
	/* max number of elements */
	size_t					max;
	int					shmid;
	int					pad;
};


/*---------------------------------------------------------------------------*/
/* msg_api_init								     */
/*---------------------------------------------------------------------------*/
int msg_api_init(struct msg_params *msg_params,
		 size_t hdrlen,
		 size_t datalen, int is_server);

/*---------------------------------------------------------------------------*/
/* msg_api_free								     */
/*---------------------------------------------------------------------------*/
void msg_api_free(struct msg_params *msg_params);

/*---------------------------------------------------------------------------*/
/* msg_alloc								     */
/*---------------------------------------------------------------------------*/
struct xio_msg *msg_alloc(size_t out_hdrlen, size_t out_datalen,
		size_t in_hdrlen, size_t in_datalen);

/*---------------------------------------------------------------------------*/
/* msg_write								     */
/*---------------------------------------------------------------------------*/
void msg_write(struct msg_params *msg_params,
	       struct xio_msg *msg,
	       void *hdr, size_t hdrlen,
	       void *data, size_t datalen);

/*---------------------------------------------------------------------------*/
/* msg_pool_alloc							     */
/*---------------------------------------------------------------------------*/
struct msg_pool *msg_pool_alloc(int max,
		size_t out_hdrlen, size_t out_datalen,
		size_t in_hdrlen, size_t in_datalen);


/*---------------------------------------------------------------------------*/
/* msg_pool_get								     */
/*---------------------------------------------------------------------------*/
struct xio_msg *msg_pool_get(struct msg_pool *pool);


/*---------------------------------------------------------------------------*/
/* msg_pool_put								     */
/*---------------------------------------------------------------------------*/
void msg_pool_put(struct msg_pool *pool, struct xio_msg *msg);


/*---------------------------------------------------------------------------*/
/* msg_pool_free							     */
/*---------------------------------------------------------------------------*/
void msg_pool_free(struct msg_pool *pool);



#endif /* #define MSG_API_H */
