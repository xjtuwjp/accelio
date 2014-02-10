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
#include "xio_tls.h"
#include "xio_sessions_store.h"
#include "xio_conns_store.h"

int page_size;

void xio_rdma_transport_constructor(void);
void xio_rdma_transport_destructor(void);

static pthread_once_t ctor_key_once = PTHREAD_ONCE_INIT;
static pthread_once_t dtor_key_once = PTHREAD_ONCE_INIT;
static struct kref ini_kref;

/*---------------------------------------------------------------------------*/
/* xio_dtor								     */
/*---------------------------------------------------------------------------*/
static void xio_dtor()
{
	xio_rdma_transport_destructor();
	xio_thread_data_destruct();
	ctor_key_once = PTHREAD_ONCE_INIT;
}

/*---------------------------------------------------------------------------*/
/* xio_dref								     */
/*---------------------------------------------------------------------------*/
static void xio_dref(struct kref *ref)
{
	pthread_once(&dtor_key_once, xio_dtor);
}

/*---------------------------------------------------------------------------*/
/* xio_dtor								     */
/*---------------------------------------------------------------------------*/
static void xio_ctor()
{
	page_size = sysconf(_SC_PAGESIZE);
	kref_init(&ini_kref);
	xio_thread_data_construct();
	sessions_store_construct();
	conns_store_construct();
	xio_rdma_transport_constructor();
	dtor_key_once = PTHREAD_ONCE_INIT;
}

/*---------------------------------------------------------------------------*/
/* xio_constructor like module init					     */
/*---------------------------------------------------------------------------*/
__attribute__((constructor)) void xio_init(void)
{
	if (ctor_key_once != PTHREAD_ONCE_INIT)
		kref_get(&ini_kref);

	pthread_once(&ctor_key_once, xio_ctor);
}

__attribute__((destructor))   void xio_shutdown(void)
{
	if (ctor_key_once == PTHREAD_ONCE_INIT)
		return;

	kref_put(&ini_kref, xio_dref);
}

