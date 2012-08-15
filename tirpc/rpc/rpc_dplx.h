/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RPC_DPLX_H
#define RPC_DPLX_H

#define rpc_dplx_send_lock(xprt, mask) \
    rpc_dplx_send_lock_impl(xprt, mask, __FILE__, __LINE__)

void rpc_dplx_send_lock_impl(SVCXPRT *xprt, sigset_t *mask,
                             const char *file, int line);

void rpc_dplx_send_unlock(SVCXPRT *xprt, sigset_t *mask);

#define rpc_dplx_recv_lock(xprt, mask) \
    rpc_dplx_recv_lock_impl(xprt, mask, __FILE__, __LINE__)

void rpc_dplx_recv_lock_impl(SVCXPRT *xprt, sigset_t *mask,
                             const char *file, int line);

void rpc_dplx_recv_unlock(SVCXPRT *xprt, sigset_t *mask);

#endif /* RPC_DPLX_H */
