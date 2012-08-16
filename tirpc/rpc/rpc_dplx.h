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

/* SVCXPRT variants */

/* slx: send lock xprt */
#define rpc_dplx_slx(xprt, mask) \
    rpc_dplx_slxi(xprt, mask, __FILE__, __LINE__)

/* slxi: send lock xprt impl */
void rpc_dplx_slxi(SVCXPRT *xprt, sigset_t *mask, const char *file, int line);

/* sux: send unlock xprt */
void rpc_dplx_sux(SVCXPRT *xprt, sigset_t *mask);

/* rlx: recv lock xprt */
#define rpc_dplx_rlx(xprt, mask) \
    rpc_dplx_rlxi(xprt, mask, __FILE__, __LINE__)

/* rlxi: recv lock xprt impl */
void rpc_dplx_rlxi(SVCXPRT *xprt, sigset_t *mask, const char *file, int line);

/* rux: recv unlock xprt */
void rpc_dplx_rux(SVCXPRT *xprt, sigset_t *mask);

/* CLIENT variants */

/* slc: send lock clnt */
#define rpc_dplx_slc(clnt, mask) \
    rpc_dplx_slci(clnt, mask, __FILE__, __LINE__)

/* slci: send lock clnt impl */
void rpc_dplx_slci(CLIENT *clnt, sigset_t *mask, const char *file, int line);

/* suc: send unlock clnt */
void rpc_dplx_suc(CLIENT *clnt, sigset_t *mask);

/* rlc: recv lock clnt */
#define rpc_dplx_rlc(clnt, mask) \
    rpc_dplx_rlci(clnt, mask, __FILE__, __LINE__)

/* rlci: recv lock clnt impl */
void rpc_dplx_rlci(CLIENT *clnt, sigset_t *mask, const char *file, int line);

/* ruc: recv unlock clnt */
void rpc_dplx_ruc(CLIENT *client, sigset_t *mask);

/* fd variants--these interfaces should be used only when NO OTHER
 * APPROACH COULD WORK.  Please. */

/* slf: send lock fd */
#define rpc_dplx_slf(fd, mask) \
    rpc_dplx_slfi(fd, mask, __FILE__, __LINE__)

/* slfi: send lock fd impl */
void rpc_dplx_slfi(int fd, sigset_t *mask, const char *file, int line);

/* suf: send unlock fd */
void rpc_dplx_suf(int fd, sigset_t *mask);

/* rlf: recv lock fd */
#define rpc_dplx_rlf(fd, mask) \
    rpc_dplx_rlfi(fd, mask, __FILE__, __LINE__)

/* rlfi: recv lock fd impl */
void rpc_dplx_rlfi(int fd, sigset_t *mask, const char *file, int line);

/* ruf: recv unlock fd */
void rpc_dplx_ruf(int fd, sigset_t *mask);

#endif /* RPC_DPLX_H */
