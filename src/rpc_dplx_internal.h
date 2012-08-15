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

#ifndef RPC_DPLX_INTERNAL_H
#define RPC_DPLX_INTERNAL_H

struct rpc_dplx_rec; /* in clnt_internal.h (avoids circular dependency) */

struct rpc_dplx_rec_set
{
    /* XXX check (dplx correct?) */
    mutex_t clnt_fd_lock; /* global mtx we'll try to spam less than formerly */
    struct rbtree_x xt;
};

/* XXX perhaps better off as a flag bit */
#define rpc_flag_clear 0
#define rpc_lock_value 1

#define RPC_DPLX_FLAG_NONE          0x0000
#define RPC_DPLX_FLAG_SPIN_LOCKED   0x0001
#define RPC_DPLX_FLAG_LOCK          0x0002 /* take rec->mtx before signal */

#ifndef HAVE_STRLCAT
extern size_t strlcat(char *dst, const char *src, size_t siz);
#endif

#ifndef HAVE_STRLCPY
extern size_t strlcpy(char *dst, const char *src, size_t siz);
#endif

/* XXXX split send/recv! */

static inline int32_t rpc_dplx_ref(struct rpc_dplx_rec *rec, u_int flags)
{
    int32_t refcnt;

    if (! (flags & RPC_DPLX_FLAG_SPIN_LOCKED))
        spin_lock(&rec->sp);

    refcnt = ++(rec->refcnt);

    if (! (flags & RPC_DPLX_FLAG_SPIN_LOCKED))
        spin_unlock(&rec->sp);

    return(refcnt);
}

int32_t rpc_dplx_unref(struct rpc_dplx_rec *rec, u_int flags);

/* XXXX next 2 need transform */
void vc_fd_lock(int fd, sigset_t *mask);
void vc_fd_unlock(int fd, sigset_t *mask);


struct rpc_dplx_rec *rpc_dplx_lookup_rec(int fd);

static inline
void rpc_dplx_init_client(CLIENT *cl)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    if (! cx->cx_rec) {
        /* many clients (and xprts) shall point to rec */
        cx->cx_rec = rpc_dplx_lookup_rec(cx->cx_fd); /* ref+1 */
    }
}

static inline
void rpc_dplx_init_xprt(SVCXPRT *xprt)
{
    if (! xprt->xp_p5) {
        /* many xprts shall point to rec */
        xprt->xp_p5 = rpc_dplx_lookup_rec(xprt->xp_fd); /* ref+1 */
    }
}

#define AMTX 1

static inline
void vc_fd_lock_impl(struct rpc_dplx_rec *rec, sigset_t *mask,
                     const char *file, int line)
{
    sigset_t __attribute__((unused)) newmask;

#if AMTX
    mutex_lock(&rec->mtx);
#else
    sigfillset(&newmask);
    sigdelset(&newmask, SIGINT); /* XXXX debugger */
    thr_sigsetmask(SIG_SETMASK, &newmask, mask);

    mutex_lock(&rec->mtx);
    while (rec->lock_flag_value)
        cond_wait(&rec->cv, &rec->mtx);
    rec->lock_flag_value = rpc_lock_value;
    mutex_unlock(&rec->mtx);
#endif
    if (__pkg_params.debug_flags & TIRPC_DEBUG_FLAG_LOCK) {
        strlcpy(rec->locktrace.file, file, 32);
        rec->locktrace.line = line;
    }
}

static inline void vc_fd_unlock_impl(struct rpc_dplx_rec *rec, sigset_t *mask)
{
#if AMTX
    mutex_unlock(&rec->mtx);
#else
    mutex_lock(&rec->mtx);
    rec->lock_flag_value = rpc_flag_clear;
    cond_signal(&rec->cv);
    mutex_unlock(&rec->mtx);
    thr_sigsetmask(SIG_SETMASK, mask, (sigset_t *) NULL);
#endif
}

static inline void vc_fd_wait_impl(struct rpc_dplx_rec *rec, uint32_t wait_for)
{
    mutex_lock(&rec->mtx);
    while (rec->lock_flag_value != rpc_flag_clear)
        cond_wait(&rec->cv, &rec->mtx);
    mutex_unlock(&rec->mtx);
}

static inline void vc_fd_signal_impl(struct rpc_dplx_rec *rec, uint32_t flags)
{
    if (flags & RPC_DPLX_FLAG_LOCK)
        mutex_lock(&rec->mtx);
    cond_signal(&rec->cv);
    if (flags & RPC_DPLX_FLAG_LOCK)
        mutex_unlock(&rec->mtx);
}

#define vc_fd_lock_c(cl, mask) \
    vc_fd_lock_c_impl(cl, mask, __FILE__, __LINE__)

static inline void vc_fd_lock_c_impl(CLIENT *cl, sigset_t *mask,
                                     const char *file, int line)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    rpc_dplx_init_client(cl);
    vc_fd_lock_impl(cx->cx_rec, mask, file, line);
}

static inline void vc_fd_unlock_c(CLIENT *cl, sigset_t *mask)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    /* barring lock order violation, cl is lock-initialized */
    vc_fd_unlock_impl(cx->cx_rec, mask);
}

void vc_fd_wait(int fd, uint32_t wait_for);

static inline void vc_fd_wait_c(CLIENT *cl, uint32_t wait_for)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    rpc_dplx_init_client(cl);
    vc_fd_wait_impl(cx->cx_rec, wait_for);
}

void vc_fd_signal(int fd, uint32_t flags);

static inline void vc_fd_signal_c(CLIENT *cl, uint32_t flags)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    rpc_dplx_init_client(cl);
    vc_fd_signal_impl(cx->cx_rec, flags);
}

static inline void vc_fd_lock_x(SVCXPRT *xprt, sigset_t *mask,
                                const char *file, int line)
{ 
    vc_lock_init_xprt(xprt);
    vc_fd_lock_impl((struct rpc_dplx_rec *) xprt->xp_p5, mask, file, line);
}

static inline void vc_fd_unlock_x(SVCXPRT *xprt, sigset_t *mask)
{
    vc_lock_init_xprt(xprt);
    vc_fd_unlock_impl((struct rpc_dplx_rec *) xprt->xp_p5, mask);
}

static inline void rpc_dplx_unref_clnt(CLIENT *cl)
{
    int32_t refcnt __attribute__((unused)) = 0;
    struct cx_data *cx = (struct cx_data *) cl->cl_private;

    if (cx->cx_rec) {
        refcnt = rpc_dplx_unref(cx->cx_rec, RPC_DPLX_FLAG_NONE);
        cx->cx_rec = NULL;
    }
}

static inline void rpc_dplx_unref_xprt(SVCXPRT *xprt)
{
    int32_t refcnt __attribute__((unused)) = 0;

    if (xprt->xp_p5) {
        refcnt = rpc_dplx_unref((struct rpc_dplx_rec *) xprt->xp_p5,
                                RPC_DPLX_FLAG_NONE);
        xprt->xp_p5 = NULL;
    }
}

void vc_lock_shutdown();

#endif /* RPC_DPLX_INTERNAL_H */
