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

#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/types.h>
#include <rpc/rpc.h>

#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */

#include "rpc_com.h"
#include <rpc/svc.h>
#include <misc/rbtree_x.h>
#include "clnt_internal.h"
#include "rpc_dplx_internal.h"

/* public */

void rpc_dplx_send_lock_impl(SVCXPRT *xprt, sigset_t *mask,
                             const char *file,
                             int line)
{ 
    vc_fd_send_lock(xprt, mask, file, line);
}

void svc_dplx_unlock_x(SVCXPRT *xprt, sigset_t *mask)
{
    vc_fd_unlock_x(xprt, mask);
}

/* private */

#define RPC_DPLX_PARTITIONS 17

static bool initialized = FALSE;

static struct rpc_dplx_rec_set rpc_dplx_rec_set = {
    PTHREAD_MUTEX_INITIALIZER /* clnt_fd_lock */,
    { 0, NULL } /* xt */
};

static inline int
rpc_dplx_cmpf(const struct opr_rbtree_node *lhs,
              const struct opr_rbtree_node *rhs)
{
    struct rpc_dplx_rec *lk, *rk;

    lk = opr_containerof(lhs, struct rpc_dplx_rec, node_k);
    rk = opr_containerof(rhs, struct rpc_dplx_rec, node_k);

    if (lk->fd_k < rk->fd_k)
        return (-1);

    if (lk->fd_k == rk->fd_k)
        return (0);

    return (1);
}

void rpc_dplx_init()
{
    int code = 0;

    mutex_lock(&vc_fd_rec_set.clnt_fd_lock);

    if (initialized)
        goto unlock;

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(&vc_fd_rec_set.xt, vc_fd_cmpf /* NULL (inline) */,
                     VC_LOCK_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx(TIRPC_DEBUG_FLAG_LOCK,
                "vc_lock_init: rbtx_init failed");

    initialized = TRUE;

unlock:
    mutex_unlock(&vc_fd_rec_set.clnt_fd_lock);
}

#define cond_init_rpc_dplx() { \
        do { \
            if (! initialized) \
                rpc_dplx_init(); \
        } while (0); \
    }

/* CLNT/SVCXPRT structures keep a reference to their associated rpc_dplx_rec
 * structures in private data.  this way we can make the lock/unlock ops
 * inline, and the amortized cost of this change for locks is 0. */

static inline struct rpc_dplx_rec *
alloc_dplx_rec(void)
{
    struct rpc_dplx_rec *rec = mem_alloc(sizeof(struct rpc_dplx_rec));
    if (rec) {
        rec->refcnt = 0;
        rec->lock_flag_value = 0;
        spin_init(&rec->sp);
        mutex_init(&rec->mtx, NULL);
        cond_init(&rec->cv, 0, NULL);
    }
    return (rec);
}

static inline void
free_dplx_rec(struct rpc_dplx_rec *rec)
{
    mutex_destroy(&rec->mtx);
    cond_destroy(&rec->cv);
    mem_free(rec, sizeof(struct rpc_dplx_rec));
}

struct rpc_dplx_rec *
rpc_dplx_lookup_rec(int fd)
{
    struct rbtree_x_part *t;
    struct rpc_dplx_rec rk, *rec = NULL;
    struct opr_rbtree_node *nv;

    cond_init_rpc_dplx();

    rk.fd_k = fd;
    t = rbtx_partition_of_scalar(&(rpc_dplx_rec_set.xt), fd);

    /* find or install a vc_fd_rec at fd */
    rwlock_rdlock(&t->lock);
    nv = opr_rbtree_lookup(&t->t, &rk.node_k);

    /* XXX rework lock+insert case, so that new entries are inserted
     * locked, and t->lock critical section is reduced */

    if (! nv) {
        rwlock_unlock(&t->lock);
        rwlock_wrlock(&t->lock);
        nv = opr_rbtree_lookup(&t->t, &rk.node_k);
        if (! nv) {
            rec = alloc_dplx_rec();
            if (! rec) {
                __warnx(TIRPC_DEBUG_FLAG_LOCK,
                        "%s: failed allocating vc_fd_rec", __func__);
                goto unlock;
            }

            /* XXX allocation fail */
            rec->fd_k = fd;

            /* tracks outstanding calls */
            opr_rbtree_init(&crec->calls.t, call_xid_cmpf);
            rec->calls.xid = 0; /* next xid is 1 */

            if (opr_rbtree_insert(&t->t, &rec->node_k)) {
                /* cant happen */
                __warnx(TIRPC_DEBUG_FLAG_LOCK,
                        "%s: collision inserting in locked rbtree partition",
                        __func__);
                free_dplx_rec(rec);
            }
        }
    }
    else
        rec = opr_containerof(nv, struct rpc_dplx_rec, node_k);

    rpc_dplx_ref(rec, VC_LOCK_FLAG_NONE);

unlock:
    rwlock_unlock(&t->lock);

    return (rec);
}

void vc_fd_lock(int fd, sigset_t *mask)
{
    sigset_t newmask;
    struct rpc_dplx_rec *rec = rpc_dplx_lookup_rec(fd);

    assert(rec);

    sigfillset(&newmask);
    sigdelset(&newmask, SIGINT); /* XXXX debugger */
    thr_sigsetmask(SIG_SETMASK, &newmask, mask);

    mutex_lock(&rec->mtx);
    while (rec->lock_flag_value) {
        cond_wait(&rec->cv, &rec->mtx);
    }
    rec->lock_flag_value = rpc_lock_value;
    mutex_unlock(&rec->mtx);
}

void vc_fd_unlock(int fd, sigset_t *mask)
{
    struct vc_fd_rec *rec = rpc_dplx_lookup_rec(fd);

    assert(rec);

    mutex_lock(&rec->mtx);
    rec->lock_flag_value = rpc_flag_clear;
    mutex_unlock(&rec->mtx);
    thr_sigsetmask(SIG_SETMASK, mask, (sigset_t *) NULL);
    cond_signal(&rec->cv);
}

void vc_fd_wait(int fd, uint32_t wait_for)
{
    struct vc_fd_rec *rec = rpc_dplx_lookup_rec(fd);
    vc_fd_wait_impl(rec, wait_for);
}

void vc_fd_signal(int fd, uint32_t flags)
{
    struct vc_fd_rec *crec = vc_lookup_fd_rec(fd);
    vc_fd_signal_impl(crec, flags);
}

int32_t
rpc_dplx_unref(struct rp_dplx_rec *rec, u_int flags)
{
    struct rbtree_x_part *t;
    struct opr_rbtree_node *nv;
    int32_t refcnt;

    if (! (flags & VC_LOCK_FLAG_MTX_LOCKED))
        mutex_lock(&rec->mtx);

    refcnt = --(rec->refcnt);

    if (rec->refcnt == 0) {
        t = rbtx_partition_of_scalar(&rpc_dplx_rec_set.xt, rec->fd_k);
        mutex_unlock(&rec->mtx);
        rwlock_wrlock(&t->lock);
        nv = opr_rbtree_lookup(&t->t, &rec->node_k);
        if (nv) {
            rec = opr_containerof(nv, struct rpc_dplx_rec, node_k);
            mutex_lock(&crec->mtx);
            if (rec->refcnt == 0) {
                (void) opr_rbtree_remove(&t->t, &rec->node_k);
                mutex_unlock(&rec->mtx);
                free_dplx_rec(rec);
                rec = NULL;
            } else
                refcnt = rec->refcnt;
        }
        rwlock_unlock(&t->lock);
    }

    if (crec && (! (flags & VC_LOCK_FLAG_MTX_LOCKED)))
        mutex_unlock(&rec->mtx);

    return (refcnt);
}

void rpc_dplx_shutdown()
{
    struct rbtree_x_part *t = NULL;
    struct opr_rbtree_node *n;
    struct vc_fd_rec *rec = NULL;
    int p_ix;

    cond_init_rpc_dplx();

    /* concurrent, restartable iteration over t */
    p_ix = 0;
    while (p_ix < VC_LOCK_PARTITIONS) {
        t = &rpc_dplx_rec_set.xt.tree[p_ix];
        rwlock_rdlock(&t->lock); /* t RLOCKED */
        n = opr_rbtree_first(&t->t);
        while (n != NULL) {
            rec = opr_containerof(n, struct rpc_dplx_rec, node_k);
            opr_rbtree_remove(&t->t, &rec->node_k);
            free_dplx_rec(rec);
            n = opr_rbtree_first(&t->t);
        } /* curr partition */
        rwlock_unlock(&t->lock); /* t !LOCKED */
        rwlock_destroy(&t->lock);
        p_ix++;
    } /* VC_LOCK_PARTITIONS */

    /* free tree */
    mem_free(rpc_dplx_rec_set.xt.tree,
             VC_LOCK_PARTITIONS*sizeof(struct rbtree_x_part));

    /* set initialized = FALSE? */
}
