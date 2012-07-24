/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * clnt_tcp.c, Implements a TCP/IP based, client side RPC.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * TCP based RPC supports 'batched calls'.
 * A sequence of calls may be batched-up in a send buffer.  The rpc call
 * return immediately to the client even though the call was not necessarily
 * sent.  The batching occurs if the results' xdr routine is NULL (0) AND
 * the rpc timeout value is zero (see clnt.h, rpc).
 *
 * Clients should NOT casually batch calls that in fact return results; that is,
 * the server side should be aware that a call is batched and not produce any
 * return message.  Batched calls that produce many result messages can
 * deadlock (netlock) the client and the server....
 *
 * Now go hang yourself.
 */
#include <config.h>
#include <pthread.h>

#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/rpc.h>
#include "rpc_com.h"

#include "clnt_internal.h"
#include "vc_lock.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>

#define CMGROUP_MAX    16
#define SCM_CREDS      0x03            /* process creds (struct cmsgcred) */

/*
 * Credentials structure, used to verify the identity of a peer
 * process that has sent us a message. This is allocated by the
 * peer process but filled in by the kernel. This prevents the
 * peer from lying about its identity. (Note that cmcred_groups[0]
 * is the effective GID.)
 */
struct cmsgcred {
    pid_t   cmcred_pid;             /* PID of sending process */
    uid_t   cmcred_uid;             /* real UID of sending process */
    uid_t   cmcred_euid;            /* effective UID of sending process */
    gid_t   cmcred_gid;             /* real GID of sending process */
    short   cmcred_ngroups;         /* number or groups */
    gid_t   cmcred_groups[CMGROUP_MAX];     /* groups */
};

struct cmessage {
    struct cmsghdr cmsg;
    struct cmsgcred cmcred;
};

static enum clnt_stat clnt_vc_call(CLIENT *, rpcproc_t, xdrproc_t, void *,
                                   xdrproc_t, void *, struct timeval);
static void clnt_vc_geterr(CLIENT *, struct rpc_err *);
static bool clnt_vc_freeres(CLIENT *, xdrproc_t, void *);
static void clnt_vc_abort(CLIENT *);
static bool clnt_vc_control(CLIENT *, u_int, void *);
static void clnt_vc_destroy(CLIENT *);
static struct clnt_ops *clnt_vc_ops(void);
static bool time_not_ok(struct timeval *);
static int read_vc(void *, void *, int);
static int write_vc(void *, void *, int);

#include "clnt_internal.h"

/*
 *      This machinery implements per-fd locks for MT-safety.  It is not
 *      sufficient to do per-CLIENT handle locks for MT-safety because a
 *      user may create more than one CLIENT handle with the same fd behind
 *      it.  Therfore, we allocate an array of flags (vc_fd_locks), protected
 *      by the clnt_fd_lock mutex, and an array (vc_cv) of condition variables
 *      similarly protected.  Vc_fd_lock[fd] == 1 => a call is active on some
 *      CLIENT handle created for that fd.  (Historical interest only.)
 *
 *      The current implementation holds locks across the entire RPC and reply.
 *      Yes, this is silly, and as soon as this code is proven to work, this
 *      should be the first thing fixed.  One step at a time.  (Fixing this.)
 *
 *      The ONC RPC record marking (RM) standard (RFC 5531, s. 11) does not
 *      provide for mixed call and reply fragment reassembly, so writes to the
 *      bytestream with MUST be record-wise atomic.  It appears that an
 *      implementation may interleave call and reply messages on distinct
 *      conversations.  This is not incompatible with holding a transport
 *      exclusive locked across a full call and reply, bud does require new
 *      control tranfers and delayed decoding support in the transport.  For
 *      duplex channels, full coordination is required between client and
 *      server tranpsorts sharing an underlying bytestream (Matt).
 */

static const char clnt_vc_errstr[] = "%s : %s";
static const char clnt_vc_str[] = "clnt_vc_create";
static const char clnt_read_vc_str[] = "read_vc";
static const char __no_mem_str[] = "out of memory";

/*
 * If event processing on the xprt associated with cl is not
 * currently blocked, do so.
 *
 * Returns TRUE if blocking state was changed, FALSE otherwise.
 *
 * Locked on entry.
 */
bool
cond_block_events_client(CLIENT *cl)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    if ((cx->cx_duplex.flags & CT_FLAG_DUPLEX) &&
        (! (cx->cx_duplex.flags & CT_FLAG_EVENTS_BLOCKED))) {
        SVCXPRT *xprt = cx->cx_duplex.xprt;
        assert(xprt);
        cx->cx_duplex.flags |= CT_FLAG_EVENTS_BLOCKED;
        (void) svc_rqst_block_events(xprt, SVC_RQST_FLAG_NONE);
        return (TRUE);
    }
    return (FALSE);
}

/* Restore event processing on the xprt associated with cl.
 * Locked. */
void
cond_unblock_events_client(CLIENT *cl)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    if (cx->cx_duplex.flags & CT_FLAG_EVENTS_BLOCKED) {
        SVCXPRT *xprt = cx->cx_duplex.xprt;
        assert(xprt);
        cx->cx_duplex.flags &= ~CT_FLAG_EVENTS_BLOCKED;
        (void) svc_rqst_unblock_events(xprt, SVC_RQST_FLAG_NONE);
    }
}

/*
 * Create a client handle for a connection.
 * Default options are set, which the user can change using clnt_control()'s.
 * The rpc/vc package does buffering similar to stdio, so the client
 * must pick send and receive buffer sizes, 0 => use the default.
 * NB: fd is copied into a private area.
 * NB: The rpch->cl_auth is set null authentication. Caller may wish to
 * set this something more useful.
 *
 * fd should be an open socket
 */
CLIENT *
clnt_vc_create(int fd,      /* open file descriptor */
               const struct netbuf *raddr, /* servers address */
               const rpcprog_t prog,    /* program number */
               const rpcvers_t vers,    /* version number */
               u_int sendsz,     /* buffer recv size */
               u_int recvsz     /* buffer send size */)
{
    return (clnt_vc_create2(fd, raddr, prog, vers, sendsz, recvsz,
                            CLNT_CREATE_FLAG_CONNECT));
}

CLIENT *
clnt_vc_create2(int fd,       /* open file descriptor */
                const struct netbuf *raddr, /* servers address */
                const rpcprog_t prog,     /* program number */
                const rpcvers_t vers,     /* version number */
                u_int sendsz,      /* buffer recv size */
                u_int recvsz,      /* buffer send size */
                u_int flags)
{
    CLIENT *cl;   /* client handle */
    struct cx_data *cx = NULL;
    struct ct_data *ct = NULL;
    struct timeval now;
    struct rpc_msg call_msg;
    static u_int32_t disrupt;
    sigset_t mask;
    sigset_t newmask;
    struct sockaddr_storage ss;
    socklen_t slen;
    struct __rpc_sockinfo si;

    if (disrupt == 0)
        disrupt = (u_int32_t)(long)raddr;

    cl = (CLIENT *)mem_alloc(sizeof (*cl));
    cx = alloc_cx_data(CX_VC_DATA, 0, 0);
    if ((cl == NULL) || (cx == NULL)) {
        (void) syslog(LOG_ERR, clnt_vc_errstr,
                      clnt_vc_str, __no_mem_str);
        rpc_createerr.cf_stat = RPC_SYSTEMERROR;
        rpc_createerr.cf_error.re_errno = errno;
        goto err;
    }
    ct = CT_DATA(cx);
    ct->ct_addr.buf = NULL;
    sigfillset(&newmask);
    thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

    /*
     * XXX - fvdl connecting while holding a mutex?
     */
    if (flags & CLNT_CREATE_FLAG_CONNECT) {
        slen = sizeof ss;
        if (getpeername(fd, (struct sockaddr *)&ss, &slen) < 0) {
            if (errno != ENOTCONN) {
                rpc_createerr.cf_stat = RPC_SYSTEMERROR;
                rpc_createerr.cf_error.re_errno = errno;
                thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
                goto err;
            }
            if (connect(fd, (struct sockaddr *)raddr->buf, raddr->len) < 0){
                rpc_createerr.cf_stat = RPC_SYSTEMERROR;
                rpc_createerr.cf_error.re_errno = errno;
                thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
                goto err;
            }
        }
    } /* FLAG_CONNECT */

    if (!__rpc_fd2sockinfo(fd, &si))
        goto err;
    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);

    ct->ct_closeit = FALSE;

    /*
     * Set up private data struct
     */
    cx->cx_fd = fd;
    ct->ct_wait.tv_usec = 0;
    ct->ct_waitset = FALSE;
    ct->ct_addr.buf = mem_alloc(raddr->maxlen);
    if (ct->ct_addr.buf == NULL)
        goto err;
    memcpy(ct->ct_addr.buf, raddr->buf, raddr->len);
    ct->ct_addr.len = raddr->len;
    ct->ct_addr.maxlen = raddr->maxlen;
    cl->cl_netid = NULL;
    cl->cl_tp = NULL;

    /*
     * Initialize call message
     */
    (void)gettimeofday(&now, NULL);
    call_msg.rm_xid = ((u_int32_t)++disrupt) ^ __RPC_GETXID(&now);
    call_msg.rm_direction = CALL;
    call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
    call_msg.rm_call.cb_prog = (u_int32_t)prog;
    call_msg.rm_call.cb_vers = (u_int32_t)vers;

    /*
     * pre-serialize the static part of the call msg and stash it away
     */
    xdrmem_create(&(ct->ct_xdrs), ct->ct_u.ct_mcallc,
                  MCALL_MSG_SIZE, XDR_ENCODE);
    if (! xdr_callhdr(&(ct->ct_xdrs), &call_msg)) {
        if (ct->ct_closeit) {
            (void)close(fd);
        }
        goto err;
    }
    ct->ct_mpos = XDR_GETPOS(&(ct->ct_xdrs));
    XDR_DESTROY(&(ct->ct_xdrs));

    /*
     * Create a client handle which uses xdrrec for serialization
     * and authnone for authentication.
     */
    cl->cl_ops = clnt_vc_ops();
    cl->cl_private = cx;

    /*
     * Register lock channel (sync)
     */
    vc_lock_init_cl(cl);

    /*
     * Setup auth
     */
    cl->cl_auth = authnone_create();

    sendsz = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsz);
    recvsz = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsz);

    /*
     * Create XDRS.  TODO:  duplex unification
     */
    xdrrec_create(&(ct->ct_xdrs), sendsz, recvsz,
                  cx, read_vc, write_vc);
    return (cl);

err:
    if (cl) {
        if (cx) {
            if (ct->ct_addr.len)
                mem_free(ct->ct_addr.buf,
                         ct->ct_addr.len);
            free_cx_data(cx);
        }
        if (cl)
            mem_free(cl, sizeof (CLIENT));
    }
    return ((CLIENT *)NULL);
}

#define vc_call_return(r) do { result=(r); goto out; } while (0);

static enum clnt_stat
clnt_vc_call(CLIENT *cl,
             rpcproc_t proc,
             xdrproc_t xdr_args,
             void *args_ptr,
             xdrproc_t xdr_results,
             void *results_ptr,
             struct timeval timeout)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    struct ct_data *ct = CT_DATA(cx);
    enum clnt_stat result = RPC_SUCCESS;
    bool shipnow, ev_blocked, duplex;
    SVCXPRT *duplex_xprt = NULL;
    XDR *xdrs = &(ct->ct_xdrs);
    rpc_ctx_t *ctx = NULL;
    int refreshes = 2;
    sigset_t mask;

    assert(cl != NULL);
    thr_sigsetmask(SIG_SETMASK, (sigset_t *) 0, &mask); /* XXX */
    vc_fd_lock_c(cl, &mask);

    /* XXX presently, we take any svcxprt out of EPOLL during calls,
     * this is harmless, but it may be less efficient than using the
     * svc event loop for call wakeups. */
    ev_blocked = cond_block_events_client(cl);

    /* Create a call context.  A lot of TI-RPC decisions need to be
     * looked at, including:
     *
     * 1. the client has a serialized call.  This looks harmless, so long
     * as the xid is adjusted.
     *
     * 2. the last xid used is now saved in the shared crec structure, it
     * will be incremented by rpc_call_create (successive calls).  There's no
     * more reason to use the old time-dependent xid logic.  It should be
     * preferable to count atomically from 1.
     *
     * 3. the client has an XDR structure, which contains the initialied
     * xdrrec stream.  Since there is only one physical byte stream, it
     * would potentially be worse to do anything else?  The main issue which
     * will arise is the need to transition the stream between calls--which
     * may require adjustment to xdrrec code.  But on review it seems to
     * follow that one xdrrec stream would be parameterized by different call
     * contexts.  We'll keep the call parameters, control transfer machinery,
     * etc, in an rpc_ctx_t, to permit this.
     */
    ctx = alloc_rpc_call_ctx(cl, proc, xdr_args, args_ptr, xdr_results,
                             results_ptr, timeout);

    /* two basic strategies are possible here--this stage
     * assumes the cost of updating the epoll (or select)
     * registration of the connected transport is preferable
     * to muxing reply events with call events through the/a
     * request dispatcher (in the common case).
     *
     * the CT_FLAG_EPOLL_ACTIVE is intended to indicate the
     * inverse strategy, which would place the current thread
     * on a waitq here (in the common case). */
    duplex = cx->cx_duplex.flags & CT_FLAG_DUPLEX;
    if (duplex)
        duplex_xprt = cx->cx_duplex.xprt;

    if (!ct->ct_waitset) {
        /* If time is not within limits, we ignore it. */
        if (time_not_ok(&timeout) == FALSE)
            ct->ct_wait = timeout;
    }

    shipnow =
        (xdr_results == NULL && timeout.tv_sec == 0
         && timeout.tv_usec == 0) ? FALSE : TRUE;

call_again:
    xdrs->x_op = XDR_ENCODE;
    xdrs->x_public = (void *) ctx; /* transiently thread call ctx */

    ctx->error.re_status = RPC_SUCCESS;
    ct->ct_u.ct_mcalli = ntohl(ctx->xid);

    if ((! XDR_PUTBYTES(xdrs, ct->ct_u.ct_mcallc, ct->ct_mpos)) ||
        (! XDR_PUTINT32(xdrs, (int32_t *)&proc)) ||
        (! AUTH_MARSHALL(cl->cl_auth, xdrs)) ||
        (! AUTH_WRAP(cl->cl_auth, xdrs, xdr_args, args_ptr))) {
        if (ctx->error.re_status == RPC_SUCCESS)
            ctx->error.re_status = RPC_CANTENCODEARGS;
        (void)xdrrec_endofrecord(xdrs, TRUE);
        vc_call_return(ctx->error.re_status);
    }

    if (! xdrrec_endofrecord(xdrs, shipnow))
        vc_call_return(ctx->error.re_status = RPC_CANTSEND);

    if (! shipnow)
        vc_call_return(RPC_SUCCESS);

    /*
     * Hack to provide rpc-based message passing
     */
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
        vc_call_return(ctx->error.re_status = RPC_TIMEDOUT);

    /*
     * Keep receiving until we get a valid transaction id.
     *
     * XXX This behavior is incompatible with duplex operation.  We'll
     * remove it shortly.
     */
    xdrs->x_op = XDR_DECODE;
    while (TRUE) {
        ctx->msg->acpted_rply.ar_verf = _null_auth;
        ctx->msg->acpted_rply.ar_results.where = NULL;
        ctx->msg->acpted_rply.ar_results.proc = (xdrproc_t)xdr_void;
        if (! xdrrec_skiprecord(xdrs)) {
            __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                    "%s: error at skiprecord", __func__);
            vc_call_return(ctx->error.re_status);
        }
        /* now decode and validate the response header */
        if (! xdr_dplx_msg_decode_start(xdrs, ctx->msg)) {
            __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                    "%s: error at xdr_dplx_msg_start", __func__);
            vc_call_return(ctx->error.re_status);
        }
        if (! xdr_dplx_msg_decode_continue(xdrs, ctx->msg)) {
            __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                    "%s: error at xdr_dplx_msg_continue", __func__);
            vc_call_return(ctx->error.re_status);
        }

        __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                "%s: successful xdr_dplx_msg (direction==%d)\n",
                __func__, ctx->msg->rm_direction);
        /* switch on direction */
        switch (ctx->msg->rm_direction) {
        case REPLY:
            if (ctx->msg->rm_xid == ctx->xid)
                goto replied;
            break;
        case CALL:
            /* XXX queue or dispatch.  on return from xp_dispatch,
             * duplex_msg points to a (potentially new, junk) rpc_msg
             * object owned by this call path */
            if (duplex) {
                struct cf_conn *cd;
                assert(duplex_xprt);
                cd = (struct cf_conn *) duplex_xprt->xp_p1;
                /* XXX Ugh.  Here's another mt-unsafe copy of xid. */
                cd->x_id = ctx->msg->rm_xid;
                __warnx(TIRPC_DEBUG_FLAG_CLNT_VC,
                        "%s: call intercepted, dispatching (x_id == %d)\n",
                        __func__, cd->x_id);
                duplex_xprt->xp_ops2->xp_dispatch(duplex_xprt, &ctx->msg);
            }
            break;
        default:
            break;
        }
    } /* while (TRUE) */

    /*
     * process header
     */
replied:
    _seterr_reply(ctx->msg, &(ctx->error));
    if (ctx->error.re_status == RPC_SUCCESS) {
        if (! AUTH_VALIDATE(cl->cl_auth, &(ctx->msg->acpted_rply.ar_verf))) {
            ctx->error.re_status = RPC_AUTHERROR;
            ctx->error.re_why = AUTH_INVALIDRESP;
        } else if (! AUTH_UNWRAP(cl->cl_auth, xdrs,
                                 xdr_results, results_ptr)) {
            if (ctx->error.re_status == RPC_SUCCESS)
                ctx->error.re_status = RPC_CANTDECODERES;
        }
        /* free verifier ... */
        if (ctx->msg->acpted_rply.ar_verf.oa_base != NULL) {
            xdrs->x_op = XDR_FREE;
            (void)xdr_opaque_auth(xdrs, &(ctx->msg->acpted_rply.ar_verf));
        }
    }  /* end successful completion */
    else {
        /* maybe our credentials need to be refreshed ... */
        if (refreshes-- && AUTH_REFRESH(cl->cl_auth, &(ctx->msg))) {
            rpc_ctx_next_xid(ctx, RPC_CTX_FLAG_LOCKED);
            goto call_again;
        }
    }  /* end of unsuccessful completion */
    vc_call_return(ctx->error.re_status);

out:
    if (ev_blocked)
        cond_unblock_events_client(cl);

    if (ctx)
        free_rpc_call_ctx(ctx, RPC_CTX_FLAG_LOCKED);

    vc_fd_unlock_c(cl, &mask);

    return (result);
}

static void
clnt_vc_geterr(cl, errp)
    CLIENT *cl;
    struct rpc_err *errp;
{
    struct ct_data *ct = CT_DATA((struct cx_data *) cl->cl_private);

    assert(cl != NULL);
    assert(errp != NULL);

    if (ct->ct_xdrs.x_public) {
        rpc_ctx_t *ctx = (rpc_ctx_t *) ct->ct_xdrs.x_public;
        *errp = ctx->error;
    } else {
        /* XXX we don't want (overhead of) an unsafe last-error value */
        struct rpc_err err;
        memset(&err, 0, sizeof(struct rpc_err));
        *errp = err;
    }
}

static bool
clnt_vc_freeres(CLIENT *cl, xdrproc_t xdr_res, void *res_ptr)
{
    struct ct_data *ct;
    XDR *xdrs;
    bool dummy;
    sigset_t mask, newmask;

    assert(cl != NULL);

    ct = CT_DATA((struct cx_data *)cl->cl_private);
    xdrs = &(ct->ct_xdrs);

    /* Handle our own signal mask here, the signal section is
     * larger than the wait (not 100% clear why) */
    sigfillset(&newmask);
    thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

    vc_fd_wait_c(cl, rpc_flag_clear);

    xdrs->x_op = XDR_FREE;
    dummy = (*xdr_res)(xdrs, res_ptr);

    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
    vc_fd_signal_c(cl, VC_LOCK_FLAG_NONE);

    return dummy;
}

/*ARGSUSED*/
static void
clnt_vc_abort(CLIENT *cl)
{
}

static bool
clnt_vc_control(CLIENT *cl, u_int request, void *info)
{
    struct cx_data *cx = (struct cx_data *)cl->cl_private;
    struct ct_data *ct = CT_DATA(cx);
    void *infop = info;
    sigset_t mask;

    assert(cl);
    thr_sigsetmask(SIG_SETMASK, (sigset_t *) 0, &mask); /* XXX */
    vc_fd_lock_c(cl, &mask);

    switch (request) {
    case CLSET_FD_CLOSE:
        ct->ct_closeit = TRUE;
        vc_fd_unlock_c(cl, &mask);
        return (TRUE);
    case CLSET_FD_NCLOSE:
        ct->ct_closeit = FALSE;
        vc_fd_unlock_c(cl, &mask);
        return (TRUE);
    default:
        break;
    }

    /* for other requests which use info */
    if (info == NULL) {
        vc_fd_unlock_c(cl, &mask);
        return (FALSE);
    }
    switch (request) {
    case CLSET_TIMEOUT:
        if (time_not_ok((struct timeval *)info)) {
            vc_fd_unlock_c(cl, &mask);
            return (FALSE);
        }
        ct->ct_wait = *(struct timeval *)infop;
        ct->ct_waitset = TRUE;
        break;
    case CLGET_TIMEOUT:
        *(struct timeval *)infop = ct->ct_wait;
        break;
    case CLGET_SERVER_ADDR:
        (void) memcpy(info, ct->ct_addr.buf, (size_t)ct->ct_addr.len);
        break;
    case CLGET_FD:
        *(int *)info = cx->cx_fd;
        break;
    case CLGET_SVC_ADDR:
        /* The caller should not free this memory area */
        *(struct netbuf *)info = ct->ct_addr;
        break;
    case CLSET_SVC_ADDR:  /* set to new address */
        vc_fd_unlock_c(cl, &mask);
        return (FALSE);
    case CLGET_XID:
        /*
         * use the knowledge that xid is the
         * first element in the call structure
         * This will get the xid of the PREVIOUS call
         */
        *(u_int32_t *)info =
            ntohl(*(u_int32_t *)(void *)&ct->ct_u.ct_mcalli);
        break;
    case CLSET_XID:
        /* This will set the xid of the NEXT call */
        *(u_int32_t *)(void *)&ct->ct_u.ct_mcalli =
            htonl(*((u_int32_t *)info) + 1);
        /* increment by 1 as clnt_vc_call() decrements once */
        break;
    case CLGET_VERS:
        /*
         * This RELIES on the information that, in the call body,
         * the version number field is the fifth field from the
         * begining of the RPC header. MUST be changed if the
         * call_struct is changed
         */
    {
        u_int32_t *tmp =
            (u_int32_t *)(ct->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT);
        *(u_int32_t *)info = ntohl(*tmp);
    }
    break;

    case CLSET_VERS:
    {
        u_int32_t tmp = htonl(*(u_int32_t *)info);
        *(ct->ct_u.ct_mcallc + 4 * BYTES_PER_XDR_UNIT) = tmp;
    }
    break;

    case CLGET_PROG:
        /*
         * This RELIES on the information that, in the call body,
         * the program number field is the fourth field from the
         * begining of the RPC header. MUST be changed if the
         * call_struct is changed
         */
    {
        u_int32_t *tmp =
            (u_int32_t *)(ct->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT);
        *(u_int32_t *)info = ntohl(*tmp);
    }
    break;

    case CLSET_PROG:
    {
        u_int32_t tmp = htonl(*(u_int32_t *)info);
        *(ct->ct_u.ct_mcallc + 3 * BYTES_PER_XDR_UNIT) = tmp;
    }
    break;

    default:
        vc_fd_unlock_c(cl, &mask);
        return (FALSE);
    }

    vc_fd_unlock_c(cl, &mask);
    return (TRUE);
}


static void
clnt_vc_destroy(CLIENT *cl)
{
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    sigset_t mask, newmask;

    /* Handle our own signal mask here, the signal section is
     * larger than the wait (not 100% clear why) */
    sigfillset(&newmask);
    thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
    vc_fd_wait_c(cl, rpc_flag_clear);

    if (CT_DATA(cx)->ct_closeit && cx->cx_fd != -1)
        (void)close(cx->cx_fd);
    XDR_DESTROY(&(CT_DATA(cx)->ct_xdrs));
    if (CT_DATA(cx)->ct_addr.buf)
        mem_free(CT_DATA(cx)->ct_addr.buf, 0); /* XXX */

    vc_fd_signal_c(cl, VC_LOCK_FLAG_NONE); /* XXX moved before free */
    vc_lock_unref_clnt(cl);

    free_cx_data(cx);

    if (cl->cl_netid && cl->cl_netid[0])
        mem_free(cl->cl_netid, strlen(cl->cl_netid) +1);
    if (cl->cl_tp && cl->cl_tp[0])
        mem_free(cl->cl_tp, strlen(cl->cl_tp) +1);
    mem_free(cl, sizeof(CLIENT));
    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
}

/*
 * Interface between xdr serializer and tcp connection.
 * Behaves like the system calls, read & write, but keeps some error state
 * around for the rpc level.
 */
static int
read_vc(void *ctp, void *buf, int len)
{
    struct cx_data *cx = (struct cx_data *)ctp;
    struct ct_data *ct = CT_DATA(cx);
    rpc_ctx_t *ctx = NULL;
    struct pollfd fd;
    int milliseconds = (int)((ct->ct_wait.tv_sec * 1000) +
                             (ct->ct_wait.tv_usec / 1000));

    if (len == 0)
        return (0);

    /* Though not previously used by TI-RPC, this is an ONC-compliant
     * use of x_public */
    ctx = (rpc_ctx_t *) ct->ct_xdrs.x_public;

    /* if ct->ct_duplex.ct_flags & CT_FLAG_DUPLEX, in the current
     * strategy (cf. clnt_vc_call and the duplex-aware getreq
     * implementation), we assert that the current thread may safely
     * block in poll (though we are not constrained to blocking
     * semantics) */

    fd.fd = cx->cx_fd;
    fd.events = POLLIN;
    for (;;) {
        switch (poll(&fd, 1, milliseconds)) {
        case 0:
            ctx->error.re_status = RPC_TIMEDOUT;
            return (-1);

        case -1:
            if (errno == EINTR)
                continue;
            ctx->error.re_status = RPC_CANTRECV;
            ctx->error.re_errno = errno;
            return (-1);
        }
        break;
    }

    len = read(cx->cx_fd, buf, (size_t)len);

    switch (len) {
    case 0:
        /* premature eof */
        ctx->error.re_errno = ECONNRESET;
        ctx->error.re_status = RPC_CANTRECV;
        len = -1;  /* it's really an error */
        break;

    case -1:
        ctx->error.re_errno = errno;
        ctx->error.re_status = RPC_CANTRECV;
        break;
    }
    return (len);
}

static int
write_vc(void *ctp, void *buf, int len)
{
    struct cx_data *cx = (struct cx_data *)ctp;
    struct ct_data *ct = CT_DATA(cx);
    rpc_ctx_t *ctx = (rpc_ctx_t *) ct->ct_xdrs.x_public;

    int i = 0, cnt;

    for (cnt = len; cnt > 0; cnt -= i, buf += i) {
        if ((i = write(cx->cx_fd, buf, (size_t)cnt)) == -1) {
            ctx->error.re_errno = errno;
            ctx->error.re_status = RPC_CANTSEND;
            return (-1);
        }
    }
    return (len);
}

static void *
clnt_vc_xdrs(CLIENT *cl)
{
    struct ct_data *ct = (struct ct_data *) cl->cl_private;
    return ((void *) & ct->ct_xdrs);
}

static struct clnt_ops *
clnt_vc_ops(void)
{
    static struct clnt_ops ops;
    extern mutex_t  ops_lock;
    sigset_t mask, newmask;

    /* VARIABLES PROTECTED BY ops_lock: ops */

    sigfillset(&newmask);
    thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
    mutex_lock(&ops_lock);
    if (ops.cl_call == NULL) {
        ops.cl_call = clnt_vc_call;
        ops.cl_xdrs = clnt_vc_xdrs;
        ops.cl_abort = clnt_vc_abort;
        ops.cl_geterr = clnt_vc_geterr;
        ops.cl_freeres = clnt_vc_freeres;
        ops.cl_destroy = clnt_vc_destroy;
        ops.cl_control = clnt_vc_control;
    }
    mutex_unlock(&ops_lock);
    thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
    return (&ops);
}

/*
 * Make sure that the time is not garbage.   -1 value is disallowed.
 * Note this is different from time_not_ok in clnt_dg.c
 */
static bool
time_not_ok(struct timeval *t)
{
    return (t->tv_sec <= -1 || t->tv_sec > 100000000 ||
            t->tv_usec <= -1 || t->tv_usec > 1000000);
}
