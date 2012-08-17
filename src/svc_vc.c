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

#include <config.h>

/*
 * svc_vc.c, Server side for Connection Oriented based RPC.
 *
 * Actually implements two flavors of transporter -
 * a tcp rendezvouser (a listner and connection establisher)
 * and a record/tcp stream.
 */
#include <sys/cdefs.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h> /* before rpc.h */
#endif
#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/rpc.h>
#include <rpc/svc.h>

#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_vrec.h>

#include <getpeereid.h>

#define XDR_VREC 0

extern struct svc_params __svc_params[1];

static bool rendezvous_request(SVCXPRT *, struct rpc_msg *);
static enum xprt_stat rendezvous_stat(SVCXPRT *);
static void svc_vc_destroy(SVCXPRT *);
static void __svc_vc_dodestroy (SVCXPRT *);
static int read_vc(void *, void *, int);
static int write_vc(void *, void *, int);
static size_t readv_vc(void *xprtp, struct iovec *iov, int iovcnt,
                       u_int flags);
static size_t writev_vc(void *xprtp, struct iovec *iov, int iovcnt,
                        u_int flags);
static enum xprt_stat svc_vc_stat(SVCXPRT *);
static bool svc_vc_recv(SVCXPRT *, struct rpc_msg *);
static bool svc_vc_getargs(SVCXPRT *, xdrproc_t, void *);
static bool svc_vc_getargs2(SVCXPRT *, xdrproc_t, void *, void *);
static bool svc_vc_freeargs(SVCXPRT *, xdrproc_t, void *);
static bool svc_vc_reply(SVCXPRT *, struct rpc_msg *);
static void svc_vc_rendezvous_ops(SVCXPRT *);
static void svc_vc_ops(SVCXPRT *);
static void svc_vc_override_ops(SVCXPRT *xprt, SVCXPRT *newxprt);
static bool svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in);
static bool svc_vc_rendezvous_control (SVCXPRT *xprt, const u_int rq,
                                         void *in);
bool __svc_clean_idle2(int timeout, bool cleanblock);

extern pthread_mutex_t svc_ctr_lock;

/*
 * If event processing on xprt is not currently blocked, set to
 * blocked. Returns TRUE if blocking state was changed, FALSE otherwise.
 *
 * The shared {CLIENT,SVCXPRT} pair is fd-locked on entry.
 */
bool
cond_block_events_svc(SVCXPRT *xprt)
{
    if (xprt->xp_p4) {
        CLIENT *cl = (CLIENT *) xprt->xp_p4;
        struct cx_data *cx = (struct cx_data *) cl->cl_private;
        if ((cx->cx_duplex.flags & CT_FLAG_DUPLEX) &&
            (! (cx->cx_duplex.flags & CT_FLAG_EVENTS_BLOCKED))) {
            cx->cx_duplex.flags |= CT_FLAG_EVENTS_BLOCKED;
            (void) svc_rqst_block_events(xprt, SVC_RQST_FLAG_NONE);
            return (TRUE);
        }
    }
    return (FALSE);
}

/* Restore event processing on xprt.  The shared {CLIENT,SVCXPRT}
 * pair is fd-locked on entry.. */
void
cond_unblock_events_svc(SVCXPRT *xprt)
{
    if (xprt->xp_p4) {
        CLIENT *cl = (CLIENT *) xprt->xp_p4;
        struct cx_data *cx = (struct cx_data *) cl->cl_private;
        if (cx->cx_duplex.flags & CT_FLAG_EVENTS_BLOCKED) {
            cx->cx_duplex.flags &= ~CT_FLAG_EVENTS_BLOCKED;
            (void) svc_rqst_unblock_events(xprt, SVC_RQST_FLAG_NONE);
        }
    }
}

static void map_ipv4_to_ipv6(sin, sin6)
    struct sockaddr_in *sin;
    struct sockaddr_in6 *sin6;
{
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = sin->sin_port;
    sin6->sin6_addr.s6_addr32[0] = 0;
    sin6->sin6_addr.s6_addr32[1] = 0;
    sin6->sin6_addr.s6_addr32[2] = htonl(0xffff);
    sin6->sin6_addr.s6_addr32[3] = *(uint32_t *) & sin->sin_addr; /* XXX strict */
}

/*
 * Usage:
 * xprt = svc_vc_create(sock, send_buf_size, recv_buf_size);
 *
 * Creates, registers, and returns a (rpc) tcp based transporter.
 * Once *xprt is initialized, it is registered as a transporter
 * see (svc.h, xprt_register).  This routine returns
 * a NULL if a problem occurred.
 *
 * The filedescriptor passed in is expected to refer to a bound, but
 * not yet connected socket.
 *
 * Since streams do buffered io similar to stdio, the caller can specify
 * how big the send and receive buffers are via the second and third parms;
 * 0 => use the system default.
 *
 * Added svc_vc_create2 with flags argument, has the behavior of the original
 * function if flags are SVC_VC_FLAG_NONE (0).
 *
 */
SVCXPRT *
svc_vc_create2(int fd, u_int sendsize, u_int recvsize, u_int flags)
{
    SVCXPRT *xprt;
    struct cf_rendezvous *r = NULL;
    struct __rpc_sockinfo si;
    struct sockaddr_storage sslocal;
    struct sockaddr *salocal;
    struct sockaddr_in *salocal_in;
    struct sockaddr_in6 *salocal_in6;
    socklen_t slen;

    r = mem_alloc(sizeof(*r));
    if (r == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_create: out of memory");
        goto cleanup_svc_vc_create;
    }
    if (!__rpc_fd2sockinfo(fd, &si))
        return NULL;
    r->sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsize);
    r->recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsize);
    r->maxrec = __svc_maxrec;
    xprt = mem_zalloc(sizeof(SVCXPRT));
    if (xprt == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_create: out of memory");
        goto cleanup_svc_vc_create;
    }
    xprt->xp_flags = SVC_XPRT_FLAG_NONE;
    xprt->xp_p1 = r;
    xprt->xp_verf = _null_auth;
    svc_vc_rendezvous_ops(xprt);
    xprt->xp_fd = fd;
    svc_rqst_init_xprt(xprt);

    /* caller should know what it's doing */
    if (flags & SVC_VC_CREATE_FLAG_LISTEN)
        listen(fd, SOMAXCONN);

    slen = sizeof (struct sockaddr_storage);
    if (getsockname(fd, (struct sockaddr *)(void *)&sslocal, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_create: could not retrieve local addr");
        goto cleanup_svc_vc_create;
    }
#if 0
    xprt->xp_port = (u_short)-1; /* It is the rendezvouser */
#else
    /* XXX following breaks strict aliasing? */
    salocal = (struct sockaddr *) &sslocal;
    switch (salocal->sa_family) {
    case AF_INET:
        salocal_in = (struct sockaddr_in *) salocal;
        xprt->xp_port = ntohs(salocal_in->sin_port);
        break;
    case AF_INET6:
        salocal_in6 = (struct sockaddr_in6 *) salocal;
        xprt->xp_port = ntohs(salocal_in6->sin6_port);
        break;
    }
#endif
    if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &sslocal, sizeof(sslocal))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc_create: no mem for local addr");
        goto cleanup_svc_vc_create;
    }

    /* conditional xprt_register */
    if ((! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)) &&
        (! (flags & SVC_VC_CREATE_FLAG_XPRT_NOREG)))
        xprt_register(xprt);

    return (xprt);

cleanup_svc_vc_create:
    if (r != NULL)
        mem_free(r, sizeof(*r));

    return (NULL);
}

SVCXPRT *
svc_vc_create(int fd, u_int sendsize, u_int recvsize)
{
    return (svc_vc_create2(fd, sendsize, recvsize, SVC_VC_CREATE_FLAG_NONE));
}

/*
 * Like svtcp_create(), except the routine takes any *open* UNIX file
 * descriptor as its first input.
 */
SVCXPRT *
svc_fd_create(int fd, u_int sendsize, u_int recvsize)
{
    struct sockaddr_storage ss;
    socklen_t slen;
    SVCXPRT *xprt;

    assert(fd != -1);

    xprt = makefd_xprt(fd, sendsize, recvsize);
    if (! xprt)
        return NULL;

    /* conditional xprt_register */
    if (! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS))
        xprt_register(xprt);

    slen = sizeof (struct sockaddr_storage);
    if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: could not retrieve local addr");
        goto freedata;
    }
    if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &ss, sizeof(ss))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: no mem for local addr");
        goto freedata;
    }

    slen = sizeof (struct sockaddr_storage);
    if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: could not retrieve remote addr");
        goto freedata;
    }
    if (!__rpc_set_netbuf(&xprt->xp_rtaddr, &ss, sizeof(ss))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: no mem for local addr");
        goto freedata;
    }

    /* Set xp_raddr for compatibility */
    __xprt_set_raddr(xprt, &ss);

    return (xprt);

freedata:
    if (xprt->xp_ltaddr.buf != NULL)
        mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);

    return (NULL);
}

/*
 * Like sv_fd_create(), except export flags for additional control.  Add
 * special handling for AF_INET and AFS_INET6.  Possibly not needed,
 * because no longer called in Ganesha.
 */
SVCXPRT *
svc_fd_create2(int fd, u_int sendsize, u_int recvsize, u_int flags)
{
    struct sockaddr_storage ss;
    struct sockaddr_in6 sin6;
    struct netbuf *addr;
    socklen_t slen;
    SVCXPRT *xprt;
    int af;

    assert(fd != -1);

    xprt = makefd_xprt(fd, sendsize, recvsize);
    if (xprt == NULL)
        return (NULL);

    slen = sizeof (struct sockaddr_storage);
    if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: could not retrieve local addr");
        goto freedata;
    }
    if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &ss, sizeof(ss))) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: no mem for local addr");
        goto freedata;
    }

    slen = sizeof (struct sockaddr_storage);
    if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: could not retrieve remote addr");
        goto freedata;
    }
    af = ss.ss_family;

    /* XXX Ganesha concepts, and apparently no longer used, check */
    if (flags & SVC_VCCR_MAP6_V1) {
        if (af == AF_INET) {
            map_ipv4_to_ipv6((struct sockaddr_in *)&ss, &sin6);
            addr = __rpc_set_netbuf(&xprt->xp_rtaddr, &ss, sizeof(ss));
        }
        else
            addr = __rpc_set_netbuf(&xprt->xp_rtaddr, &sin6, sizeof(ss));
    } else
        addr = __rpc_set_netbuf(&xprt->xp_rtaddr, &ss, sizeof(ss));
    if (!addr) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_fd_create: no mem for local addr");
        goto freedata;
    }

    /* XXX Ganesha concepts, check */
    if (flags & SVC_VCCR_RADDR) {
        switch (af) {
        case AF_INET:
            if (! (flags & SVC_VCCR_RADDR_INET))
                goto out;
            break;
        case AF_INET6:
            if (! (flags & SVC_VCCR_RADDR_INET6))
                goto out;
            break;
        case AF_LOCAL:
            if (! (flags & SVC_VCCR_RADDR_LOCAL))
                goto out;
            break;
        default:
            break;
        }
        /* Set xp_raddr for compatibility */
        __xprt_set_raddr(xprt, &ss);
    }
out:
    /* conditional xprt_register */
    if ((! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)) &&
        (! (flags & SVC_VC_CREATE_FLAG_XPRT_NOREG)))
        xprt_register(xprt);

    return (xprt);

freedata:
    if (xprt->xp_ltaddr.buf != NULL)
        mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);

    return (NULL);
}


SVCXPRT *
makefd_xprt(int fd, u_int sendsz, u_int recvsz)
{
    SVCXPRT *xprt;
    struct cf_conn *cd;
    const char *netid;
    struct __rpc_sockinfo si;

    assert(fd != -1);

    if (! svc_vc_new_conn_ok()) {
            __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                    "%s: makefd_xprt: max_connections exceeded\n",
                    __func__);
                xprt = NULL;
                goto done;
    }

    xprt = mem_zalloc(sizeof(SVCXPRT));
    if (xprt == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_vc: makefd_xprt: out of memory");
        goto done;
    }
    memset(xprt, 0, sizeof *xprt);
    mutex_init(&xprt->xp_lock, NULL);
    cd = mem_alloc(sizeof(struct cf_conn));
    if (cd == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "svc_tcp: makefd_xprt: out of memory");
        mem_free(xprt, sizeof(SVCXPRT));
        xprt = NULL;
        goto done;
    }
    cd->strm_stat = XPRT_IDLE;

    /* the SVCXPRT created in svc_vc_create accepts new connections
     * in its xp_recv op, the rendezvous_request method, but xprt is
     * a call channel */
    svc_vc_ops(xprt);

#if XDR_VREC
    /* parallel send/recv */
    xdr_vrec_create(&(cd->xdrs_in),
                    XDR_VREC_IN, xprt, readv_vc, NULL, recvsz,
                    VREC_FLAG_NONE);

    xdr_vrec_create(&(cd->xdrs_out),
                    XDR_VREC_OUT, xprt, NULL, writev_vc, sendsz,
                    VREC_FLAG_NONE);
#else
    /* XXX */
    xdrrec_create(&(cd->xdrs_in), sendsz, recvsz, xprt,
                  read_vc, write_vc);
    xdrrec_create(&(cd->xdrs_out), sendsz, recvsz, xprt,
                  read_vc, write_vc);
#endif

    xprt->xp_p1 = cd;
    xprt->xp_verf.oa_base = cd->verf_body;
    xprt->xp_fd = fd;
    if (__rpc_fd2sockinfo(fd, &si) && __rpc_sockinfo2netid(&si, &netid))
        xprt->xp_netid = rpc_strdup(netid);

    /* Make reachable.  Registration deferred.  */
    svc_rqst_init_xprt(xprt);
done:
    return (xprt);
}

/*ARGSUSED*/
static bool
rendezvous_request(SVCXPRT *xprt, struct rpc_msg *msg)
{
    int sock;
    struct cf_rendezvous *r;
    struct cf_conn *cd;
    struct sockaddr_storage addr;
    socklen_t len;
    struct __rpc_sockinfo si;
    SVCXPRT *newxprt;

    assert(xprt != NULL);
    assert(msg != NULL);

    r = (struct cf_rendezvous *)xprt->xp_p1;
again:
    len = sizeof addr;
    if ((sock = accept(xprt->xp_fd, (struct sockaddr *)(void *)&addr,
                       &len)) < 0) {
        if (errno == EINTR)
            goto again;
        /*
         * Clean out the most idle file descriptor when we're
         * running out.
         */
        if (errno == EMFILE || errno == ENFILE) {
            switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
            case SVC_EVENT_EPOLL:
                /* XXX we did implement a plug-out strategy for this--check
                 * whether svc_clean_idle2 should be called */
                break;
#endif
            default:
                /* XXX formerly select/fd_set case, now placeholder
                 * for new event systems, reworked select, etc. */
                abort(); /* XXX */
                break;
            } /* switch */
            goto again;
        }
        return (FALSE);
    }
    /*
     * make a new transport (re-uses xprt)
     */
    newxprt = makefd_xprt(sock, r->sendsize, r->recvsize);
    if (! newxprt)
        return (FALSE);

    /*
     * propagate special ops
     */
    svc_vc_override_ops(xprt, newxprt);

    /* move xprt_register() out of makefd_xprt */
    (void) svc_rqst_xprt_register(xprt, newxprt);

    if (!__rpc_set_netbuf(&newxprt->xp_rtaddr, &addr, len)) {
        abort();
        return (FALSE);
    }

    __xprt_set_raddr(newxprt, &addr);

    /* XXX fvdl - is this useful? (Yes.  Matt) */
    if (__rpc_fd2sockinfo(sock, &si) && si.si_proto == IPPROTO_TCP) {
        len = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &len, sizeof (len));
    }

    cd = (struct cf_conn *)newxprt->xp_p1;

    cd->recvsize = r->recvsize;
    cd->sendsize = r->sendsize;
    cd->maxrec = r->maxrec;

#if 0 /* XXX vrec wont support atm (and it needs serious work anyway) */
    if (cd->maxrec != 0) {
        flags = fcntl(sock, F_GETFL, 0);
        if (flags  == -1)
            return (FALSE);
        if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1)
            return (FALSE);
        if (cd->recvsize > cd->maxrec)
            cd->recvsize = cd->maxrec;
        cd->nonblock = TRUE;
        __xdrrec_setnonblock(&cd->xdrs, cd->maxrec);
    } else
        cd->nonblock = FALSE;
#else
    cd->nonblock = FALSE;
#endif

    gettimeofday(&cd->last_recv_time, NULL);

    /* if parent has xp_rdvs, use it */
    if (xprt->xp_ops2->xp_rdvs)
        xprt->xp_ops2->xp_rdvs(xprt, newxprt, SVC_RQST_FLAG_NONE, NULL);

    return (FALSE); /* there is never an rpc msg to be processed */
}

/*ARGSUSED*/
static enum xprt_stat
rendezvous_stat(SVCXPRT *xprt)
{

    return (XPRT_IDLE);
}

static void
svc_vc_destroy(SVCXPRT *xprt)
{
    assert(xprt != NULL);
    (void) svc_rqst_xprt_unregister(xprt, SVC_RQST_FLAG_NONE);
    __svc_vc_dodestroy(xprt);
}

static void
__svc_vc_dodestroy(SVCXPRT *xprt)
{
    struct cf_conn *cd;
    struct cf_rendezvous *r;

    cd = (struct cf_conn *)xprt->xp_p1;

    /* Omit close in cases such as donation of the connection
     * to a client transport handle */
    if ((xprt->xp_fd != RPC_ANYFD) &&
        (!(xprt->xp_flags & SVC_XPRT_FLAG_DONTCLOSE)))
        (void)close(xprt->xp_fd);

    if (xprt->xp_port != 0) {
        /* a rendezvouser socket */
        r = (struct cf_rendezvous *)xprt->xp_p1;
        mem_free(r, sizeof (struct cf_rendezvous));
        xprt->xp_port = 0;
    } else {
        /* an actual connection socket */
        XDR_DESTROY(&(cd->xdrs_in));
        XDR_DESTROY(&(cd->xdrs_out));
        mem_free(cd, sizeof(struct cf_conn));
    }
    if (xprt->xp_auth != NULL) {
        SVCAUTH_DESTROY(xprt->xp_auth);
        xprt->xp_auth = NULL;
    }
    if (xprt->xp_rtaddr.buf)
        mem_free(xprt->xp_rtaddr.buf, xprt->xp_rtaddr.maxlen);
    if (xprt->xp_ltaddr.buf)
        mem_free(xprt->xp_ltaddr.buf, xprt->xp_ltaddr.maxlen);
    if (xprt->xp_tp)
        mem_free(xprt->xp_tp, 0);
    if (xprt->xp_netid)
        mem_free(xprt->xp_netid, 0);

    svc_vc_dec_nconns();
    svc_rqst_finalize_xprt(xprt);
    rpc_dplx_unref_xprt(xprt);

    /* assert: caller has unregistered xprt */
    /* duplex */
    if (xprt->xp_p4) {
        CLIENT *cl = (CLIENT *) xprt->xp_p4;
        SetDestroyed(cl);
    }

    /* call free hook */
    if (xprt->xp_ops2->xp_free_xprt)
        xprt->xp_ops2->xp_free_xprt(xprt);

    mem_free(xprt, sizeof(SVCXPRT));
}

extern mutex_t ops_lock;

/*ARGSUSED*/
static bool
svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in)
{
    switch (rq) {
    case SVCGET_XP_FLAGS:
        *(u_int *)in = xprt->xp_flags;
        break;
    case SVCSET_XP_FLAGS:
        xprt->xp_flags = *(u_int *)in;
        break;
    case SVCGET_XP_RECV:
        mutex_lock(&ops_lock);
        *(xp_recv_t *)in = xprt->xp_ops->xp_recv;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_RECV:
        mutex_lock(&ops_lock);
        xprt->xp_ops->xp_recv = *(xp_recv_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_GETREQ:
        mutex_lock(&ops_lock);
        *(xp_getreq_t *)in = xprt->xp_ops2->xp_getreq;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_GETREQ:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_getreq = *(xp_getreq_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        *(xp_dispatch_t *)in = xprt->xp_ops2->xp_dispatch;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_dispatch = *(xp_dispatch_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_RDVS:
        mutex_lock(&ops_lock);
        *(xp_rdvs_t *)in = xprt->xp_ops2->xp_rdvs;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_RDVS:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_rdvs = *(xp_rdvs_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_FREE_XPRT:
        mutex_lock(&ops_lock);
        *(xp_free_xprt_t *)in = xprt->xp_ops2->xp_free_xprt;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_FREE_XPRT:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_free_xprt = *(xp_free_xprt_t)in;
        mutex_unlock(&ops_lock);
        break;
    default:
        return (FALSE);
    }
    return (TRUE);
}

static bool
svc_vc_rendezvous_control(SVCXPRT *xprt, const u_int rq, void *in)
{
    struct cf_rendezvous *cfp;

    cfp = (struct cf_rendezvous *)xprt->xp_p1;
    if (cfp == NULL)
        return (FALSE);
    switch (rq) {
    case SVCGET_CONNMAXREC:
        *(int *)in = cfp->maxrec;
        break;
    case SVCSET_CONNMAXREC:
        cfp->maxrec = *(int *)in;
        break;
    case SVCGET_XP_RECV:
        *(xp_recv_t *)in = xprt->xp_ops->xp_recv;
        break;
    case SVCSET_XP_RECV:
        xprt->xp_ops->xp_recv = *(xp_recv_t)in;
        break;
    case SVCGET_XP_GETREQ:
        mutex_lock(&ops_lock);
        *(xp_getreq_t *)in = xprt->xp_ops2->xp_getreq;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_GETREQ:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_getreq = *(xp_getreq_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        *(xp_dispatch_t *)in = xprt->xp_ops2->xp_dispatch;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_DISPATCH:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_dispatch = *(xp_dispatch_t)in;
        mutex_unlock(&ops_lock);
        break;
    case SVCGET_XP_RDVS:
        mutex_lock(&ops_lock);
        *(xp_rdvs_t *)in = xprt->xp_ops2->xp_rdvs;
        mutex_unlock(&ops_lock);
        break;
    case SVCSET_XP_RDVS:
        mutex_lock(&ops_lock);
        xprt->xp_ops2->xp_rdvs = *(xp_rdvs_t)in;
        mutex_unlock(&ops_lock);
        break;
    default:
        return (FALSE);
    }
    return (TRUE);
}

/*
 * reads data from the tcp or udp connection.
 * any error is fatal and the connection is closed.
 * (And a read of zero bytes is a half closed stream => error.)
 * All read operations timeout after 35 seconds.  A timeout is
 * fatal for the connection.
 */
static int
read_vc(void *xprtp, void *buf, int len)
{
    SVCXPRT *xprt;
    int sock;
    int milliseconds = 35 * 1000; /* XXX shouldn't this be configurable? */
    struct pollfd pollfd;
    struct cf_conn *cfp;

    xprt = (SVCXPRT *)xprtp;
    assert(xprt != NULL);

    sock = xprt->xp_fd;

    cfp = (struct cf_conn *)xprt->xp_p1;

    if (cfp->nonblock) {
        len = read(sock, buf, (size_t)len);
        if (len < 0) {
            if (errno == EAGAIN)
                len = 0;
            else
                goto fatal_err;
        }
        if (len != 0)
            gettimeofday(&cfp->last_recv_time, NULL);
        return len;
    }

    /* XXX svc_dplx side of poll -- I think we want to
     * consider making this hot-threaded as well (Matt) */

    do {
        pollfd.fd = sock;
        pollfd.events = POLLIN;
        pollfd.revents = 0;
        switch (poll(&pollfd, 1, milliseconds)) {
        case -1:
            if (errno == EINTR)
                continue;
            /*FALLTHROUGH*/
        case 0:
            goto fatal_err;

        default:
            break;
        }
    } while ((pollfd.revents & POLLIN) == 0);

    if ((len = read(sock, buf, (size_t)len)) > 0) {
        gettimeofday(&cfp->last_recv_time, NULL);
        return (len);
    }

fatal_err:
    ((struct cf_conn *)(xprt->xp_p1))->strm_stat = XPRT_DIED;
    return (-1);
}

/*
 * writes data to the tcp connection.
 * Any error is fatal and the connection is closed.
 */
static int
write_vc(void *xprtp, void *buf, int len)
{
    SVCXPRT *xprt;
    int i, cnt;
    struct cf_conn *cd;
    struct timeval tv0, tv1;

    xprt = (SVCXPRT *)xprtp;
    assert(xprt != NULL);

    cd = (struct cf_conn *)xprt->xp_p1;

    if (cd->nonblock)
        gettimeofday(&tv0, NULL);

    for (cnt = len; cnt > 0; cnt -= i, buf += i) {
        i = write(xprt->xp_fd, buf, (size_t)cnt);
        if (i  < 0) {
            if (errno != EAGAIN || !cd->nonblock) {
                cd->strm_stat = XPRT_DIED;
                return (-1);
            }
            if (cd->nonblock && i != cnt) {
                /*
                 * For non-blocking connections, do not
                 * take more than 2 seconds writing the
                 * data out.
                 *
                 * XXX 2 is an arbitrary amount.
                 */
                gettimeofday(&tv1, NULL);
                if (tv1.tv_sec - tv0.tv_sec >= 2) {
                    cd->strm_stat = XPRT_DIED;
                    return (-1);
                }
            }
        }
    }

    return (len);
}

/* vector versions */

/*
 * reads data from the tcp or udp connection.
 * any error is fatal and the connection is closed.
 * (And a read of zero bytes is a half closed stream => error.)
 * All read operations timeout after 35 seconds.  A timeout is
 * fatal for the connection.
 */
static size_t
readv_vc(void *xprtp, struct iovec *iov, int iovcnt, u_int flags)
{
    SVCXPRT *xprt;
    int milliseconds = 35 * 1000; /* XXX shouldn't this be configurable? */
    struct pollfd pollfd;
    struct cf_conn *cd;
    size_t nbytes = -1;

    xprt = (SVCXPRT *)xprtp;
    assert(xprt != NULL);

    cd = (struct cf_conn *)xprt->xp_p1;

    do {
        pollfd.fd = xprt->xp_fd;
        pollfd.events = POLLIN;
        pollfd.revents = 0;
        switch (poll(&pollfd, 1, milliseconds)) {
        case -1:
            if (errno == EINTR)
                continue;
            /*FALLTHROUGH*/
        case 0:
            goto fatal_err;
        default:
            break;
        }
    } while ((pollfd.revents & POLLIN) == 0);

    if ((nbytes = readv(xprt->xp_fd, iov, iovcnt)) > 0) {
        gettimeofday(&cd->last_recv_time, NULL);
        goto out;
    }

fatal_err:
    ((struct cf_conn *)(xprt->xp_p1))->strm_stat = XPRT_DIED;
out:
    return (nbytes);
}

/*
 * writes data to the tcp connection.
 * Any error is fatal and the connection is closed.
 */
static size_t
writev_vc(void *xprtp, struct iovec *iov, int iovcnt, u_int flags)
{
    SVCXPRT *xprt;
    struct cf_conn *cd;
    size_t nbytes;

    xprt = (SVCXPRT *)xprtp;
    assert(xprt != NULL);

    cd = (struct cf_conn *)xprt->xp_p1;

    nbytes = writev(xprt->xp_fd, iov, iovcnt);
    if (nbytes  < 0) {
        cd->strm_stat = XPRT_DIED;
        return (-1);
    }

    return (nbytes);
}

static enum xprt_stat
svc_vc_stat(SVCXPRT *xprt)
{
    struct cf_conn *cd = (struct cf_conn *)(xprt->xp_p1);

    if (cd->strm_stat == XPRT_DIED)
        return (XPRT_DIED);
#if XDR_VREC
    /* SVC_STAT() only cares about the recv queue */
    if (! xdr_vrec_eof(&(cd->xdrs_in)))
#else
    if (! xdrrec_eof(&(cd->xdrs_in)))
#endif
        return (XPRT_MOREREQS);

    return (XPRT_IDLE);
}

static bool
svc_vc_recv(SVCXPRT *xprt, struct rpc_msg *msg)
{
    struct cf_conn *cd;
    XDR *xdrs;

    cd = (struct cf_conn *)(xprt->xp_p1);

    xdrs = &(cd->xdrs_in); /* recv queue */

    /* XXX assert(!cd->nonblock) */
    if (cd->nonblock) {
        if (!__xdrrec_getrec(xdrs, &cd->strm_stat, TRUE))
            return FALSE;
    }

    xdrs->x_op = XDR_DECODE;

    /*
     * No need skip records with nonblocking connections
     */
    if (cd->nonblock == FALSE)
#if XDR_VREC
        (void) xdr_vrec_skiprecord(xdrs);
#else
        (void) xdrrec_skiprecord(xdrs);
#endif

    if (xdr_dplx_msg(xdrs, msg)) {
        /* XXX actually using cd->x_id !MT-SAFE */
        cd->x_id = msg->rm_xid;
        return (TRUE);
    }

    cd->strm_stat = XPRT_DIED;
    return (FALSE);
}

static bool
svc_vc_getargs(SVCXPRT *xprt, xdrproc_t xdr_args, void *args_ptr)
{
    bool rslt = TRUE;

    assert(xprt != NULL);
    /* args_ptr may be NULL */

    /* XXXX TODO: bidirectional unification */

    if (! SVCAUTH_UNWRAP(xprt->xp_auth,
                         &(((struct cf_conn *)(xprt->xp_p1))->xdrs_in),
                         xdr_args, args_ptr)) {
#if 0 /* XXX bidrectional unification (there will be only one queue pair) */
        CLIENT *cl;
        cl = (CLIENT *) xprt->xp_p4;
        if (cl) {
            struct cx_data *cx = (struct cx_data *) cl->cl_private;
            struct ct_data *ct = CT_DATA(cx);
            if (cx->cx_duplex.flags & CT_FLAG_DUPLEX) {
                if (! SVCAUTH_UNWRAP(xprt->xp_auth,
                                     &(ct->ct_xdrs),
                                     xdr_args, args_ptr)) {
                    rslt = FALSE;
                }
            }
        }
#endif /* 0 */
        rslt = FALSE;
    }

    /* XXX Upstream TI-RPC lacks this call, but -does- call svc_dg_freeargs
     * in svc_dg_getargs if SVCAUTH_UNWRAP fails. */
    if (! rslt)
        svc_vc_freeargs(xprt, xdr_args, args_ptr);

    return (rslt);
}

static bool
svc_vc_getargs2(SVCXPRT *xprt, xdrproc_t xdr_args, void *args_ptr,
                void *u_data)
{
    bool rslt = TRUE;
    struct cf_conn *cd = (struct cf_conn *) xprt->xp_p1;
    XDR *xdrs = &cd->xdrs_in; /* recv queue */

    /* threads u_data for advanced decoders*/
    xdrs->x_public = u_data;

    /* XXX TODO: duplex unification (xdrs)  */

    if (! SVCAUTH_UNWRAP(xprt->xp_auth,
                         &(((struct cf_conn *)(xprt->xp_p1))->xdrs_in),
                         xdr_args, args_ptr)) {
#if 0 /* XXX bidrectional unification (there will be only one queue pair) */
        CLIENT *cl;
        cl = (CLIENT *) xprt->xp_p4;
        if (cl) {
            struct cx_data *cx = (struct cx_data *) cl->cl_private;
            struct ct_data *ct = CT_DATA(cx);
            if (cx->cx_duplex.flags & CT_FLAG_DUPLEX) {
                if (! SVCAUTH_UNWRAP(xprt->xp_auth,
                                     &(ct->ct_xdrs),
                                     xdr_args, args_ptr)) {
                    rslt = FALSE;
                }
            }
        }
#endif /* 0 */
        rslt = FALSE;
    }

    /* XXX Upstream TI-RPC lacks this call, but -does- call svc_dg_freeargs
     * in svc_dg_getargs if SVCAUTH_UNWRAP fails. */
    if (! rslt)
        svc_vc_freeargs(xprt, xdr_args, args_ptr);

    return (rslt);
}

static bool
svc_vc_freeargs(SVCXPRT *xprt, xdrproc_t xdr_args, void *args_ptr)
{
    XDR *xdrs;

    assert(xprt != NULL);
    /* args_ptr may be NULL */

    xdrs = &(((struct cf_conn *)(xprt->xp_p1))->xdrs_in);

    xdrs->x_op = XDR_FREE;
    return ((*xdr_args)(xdrs, args_ptr));
}

static bool
svc_vc_reply(SVCXPRT *xprt, struct rpc_msg *msg)
{
    XDR *xdrs;
    struct cf_conn *cd;
    xdrproc_t xdr_results;
    caddr_t xdr_location;
    bool rstat, has_args;
#if 0
    CLIENT *cl; /* XXX duplex */
    struct ct_data *ct;
#endif

    assert(xprt != NULL);
    assert(msg != NULL);

    cd = (struct cf_conn *)(xprt->xp_p1);
    xdrs = &(cd->xdrs_out); /* send queue */

#if 0 /* XXX duplex debugging */
    if (xprt->xp_p4) {
        cl = (CLIENT *) xprt->xp_p4;
        ct = CT_DATA((struct cx_data *) cl->cl_private);
    }
#endif

    if (msg->rm_reply.rp_stat == MSG_ACCEPTED &&
        msg->rm_reply.rp_acpt.ar_stat == SUCCESS) {
        has_args = TRUE;
        xdr_results = msg->acpted_rply.ar_results.proc;
        xdr_location = msg->acpted_rply.ar_results.where;

        msg->acpted_rply.ar_results.proc = (xdrproc_t)xdr_void;
        msg->acpted_rply.ar_results.where = NULL;
    } else {
        has_args = FALSE;
        xdr_results = NULL;
        xdr_location = NULL;
    }

    xdrs->x_op = XDR_ENCODE;

    /* MT-SAFE */
    if (! (msg->rm_flags & RPC_MSG_FLAG_MT_XID))
        msg->rm_xid = cd->x_id;

    rstat = FALSE;
    if (xdr_replymsg(xdrs, msg) &&
        (!has_args || (xprt->xp_auth &&
                       SVCAUTH_WRAP(xprt->xp_auth, xdrs, xdr_results,
                                    xdr_location)))) {
        rstat = TRUE;
    }
#if XDR_VREC
    (void)xdr_vrec_endofrecord(xdrs, TRUE);
#else
    (void)xdrrec_endofrecord(xdrs, TRUE);
#endif
    return (rstat);
}

static void
svc_vc_ops(SVCXPRT *xprt)
{
    static struct xp_ops ops;
    static struct xp_ops2 ops2;

/* VARIABLES PROTECTED BY ops_lock: ops, ops2, xp_type */

    mutex_lock(&ops_lock);
    xprt->xp_type = XPRT_TCP;
    if (ops.xp_recv == NULL) {
        ops.xp_recv = svc_vc_recv;
        ops.xp_stat = svc_vc_stat;
        ops.xp_getargs = svc_vc_getargs;
        ops.xp_getargs2 = svc_vc_getargs2;
        ops.xp_reply = svc_vc_reply;
        ops.xp_freeargs = svc_vc_freeargs;
        ops.xp_destroy = svc_vc_destroy;
        ops2.xp_control = svc_vc_control;
        ops2.xp_getreq = svc_getreq_default;
        ops2.xp_dispatch = svc_dispatch_default;
        ops2.xp_rdvs = NULL; /* no default */
    }
    xprt->xp_ops = &ops;
    xprt->xp_ops2 = &ops2;
    mutex_unlock(&ops_lock);
}

static void
svc_vc_override_ops(SVCXPRT *xprt, SVCXPRT *newxprt)
{
    if (xprt->xp_ops2->xp_getreq)
        newxprt->xp_ops2->xp_getreq = xprt->xp_ops2->xp_getreq;
    if (xprt->xp_ops2->xp_dispatch)
        newxprt->xp_ops2->xp_dispatch = xprt->xp_ops2->xp_dispatch;
    if (xprt->xp_ops2->xp_rdvs)
        newxprt->xp_ops2->xp_rdvs = xprt->xp_ops2->xp_rdvs;
}

static void
svc_vc_rendezvous_ops(SVCXPRT *xprt)
{
    static struct xp_ops ops;
    static struct xp_ops2 ops2;
    extern mutex_t ops_lock;

    mutex_lock(&ops_lock);
    xprt->xp_type = XPRT_TCP_RENDEZVOUS;
    if (ops.xp_recv == NULL) {
        ops.xp_recv = rendezvous_request;
        ops.xp_stat = rendezvous_stat;
        ops.xp_getargs =
            (bool (*)(SVCXPRT *, xdrproc_t, void *))abort;
        ops.xp_reply =
            (bool (*)(SVCXPRT *, struct rpc_msg *))abort;
        ops.xp_freeargs =
            (bool (*)(SVCXPRT *, xdrproc_t, void *))abort,
            ops.xp_destroy = svc_vc_destroy;
        ops2.xp_control = svc_vc_rendezvous_control;
        ops2.xp_getreq = svc_getreq_default;
        ops2.xp_dispatch = svc_dispatch_default;
    }
    xprt->xp_ops = &ops;
    xprt->xp_ops2 = &ops2;
    mutex_unlock(&ops_lock);
}

/*
 * Get the effective UID of the sending process. Used by rpcbind, keyserv
 * and rpc.yppasswdd on AF_LOCAL.
 */
int
__rpc_get_local_uid(SVCXPRT *transp, uid_t *uid) {
    int sock, ret;
    gid_t egid;
    uid_t euid;
    struct sockaddr *sa;

    sock = transp->xp_fd;
    sa = (struct sockaddr *)transp->xp_rtaddr.buf;
    if (sa->sa_family == AF_LOCAL) {
        ret = getpeereid(sock, &euid, &egid);
        if (ret == 0)
            *uid = euid;
        return (ret);
    } else
        return (-1);
}

/*
 * Destroy xprts that have not have had any activity in 'timeout' seconds.
 * If 'cleanblock' is true, blocking connections (the default) are also
 * cleaned. If timeout is 0, the least active connection is picked.
 *
 * Though this is not a publicly documented interface, some versions of
 * rpcbind are known to call this function.  Do not alter or remove this
 * API without changing the library's sonum.
 */

bool
__svc_clean_idle(fd_set *fds, int timeout, bool cleanblock)
{
    return ( __svc_clean_idle2(timeout, cleanblock) );

} /* __svc_clean_idle */

/*
 * Like __svc_clean_idle but event-type independent.  For now no cleanfds.
 */

struct svc_clean_idle_arg
{
    SVCXPRT *least_active;
    struct timeval tv, tmax;
    int cleanblock, ncleaned, timeout;
};

static void svc_clean_idle2_func(SVCXPRT *xprt, void *arg)
{
    struct cf_conn *cd;
    struct timeval tdiff;
    struct svc_clean_idle_arg *acc = (struct svc_clean_idle_arg *) arg;

    if (TRUE) { /* flag in __svc_params->ev_u.epoll? */

        mutex_lock(&xprt->xp_lock);

        if (xprt == NULL || xprt->xp_ops == NULL ||
            xprt->xp_ops->xp_recv != svc_vc_recv)
            goto unlock;

        cd = (struct cf_conn *) xprt->xp_p1;
        if (!acc->cleanblock && !cd->nonblock)
            goto unlock;

        if (acc->timeout == 0) {
            timersub(&acc->tv, &cd->last_recv_time, &tdiff);
            if (timercmp(&tdiff, &acc->tmax, >)) {
                acc->tmax = tdiff;
                acc->least_active = xprt;
            }
            goto unlock;
        }
        if (acc->tv.tv_sec - cd->last_recv_time.tv_sec > acc->timeout) {
            /* XXX locking */
            mutex_unlock(&xprt->xp_lock);
            (void) svc_rqst_xprt_unregister(xprt, SVC_RQST_FLAG_NONE);
            SVC_DESTROY(xprt);
            acc->ncleaned++;
            goto out;
        }

    unlock:
        mutex_unlock(&xprt->xp_lock);
    } /* TRUE */
out:
    return;
}

bool
__svc_clean_idle2(int timeout, bool cleanblock)
{
    struct svc_clean_idle_arg acc;
    static mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

    if (mutex_trylock(&mtx) != 0)
        goto out;

    memset(&acc, 0, sizeof(struct svc_clean_idle_arg));
    gettimeofday(&acc.tv, NULL);
    acc.timeout = timeout;

    /* XXX refcounting, state? */
    svc_xprt_foreach(svc_clean_idle2_func, (void *) &acc);

    if (timeout == 0 && acc.least_active != NULL) {
        (void) svc_rqst_xprt_unregister(
            acc.least_active, SVC_RQST_FLAG_NONE);
        /* __xprt_unregister_unlocked(acc.least_active); */
        __svc_vc_dodestroy(acc.least_active);
        acc.ncleaned++;
    }

    mutex_unlock(&mtx);

out:
    return (acc.ncleaned > 0) ? TRUE : FALSE;

} /* __svc_clean_idle2 */

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 *
 * If flags & SVC_VC_CLNT_CREATE_DEDICATED, the supplied xprt will be
 * unregistered and disposed inline.
 */
CLIENT *
clnt_vc_create_from_svc(SVCXPRT *xprt,
                        const rpcprog_t prog,
                        const rpcvers_t vers,
                        const uint32_t flags)
{

    struct cf_conn *cd;
    CLIENT *cl;

    mutex_lock(&xprt->xp_lock);

    /* XXX return allocated client structure, or allocate one if none
     * is currently allocated (it can be destroyed) */
    if (xprt->xp_p4) {
        cl = (CLIENT *) xprt->xp_p4;
        goto unlock;
    }

    cd = (struct cf_conn *) xprt->xp_p1;

    /* Create a client transport handle.  The endpoint is already
     * connected. */
    cl = clnt_vc_create2(xprt->xp_fd,
                         &xprt->xp_rtaddr,
                         prog,
                         vers,
                         cd->recvsize,
                         cd->sendsize,
                         CLNT_CREATE_FLAG_SVCXPRT);
    if (! cl)
        goto fail; /* XXX should probably warn here */

    if (flags & SVC_VC_CREATE_FLAG_DPLX) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "%s:  disposing--calls SetDuplex\n", __func__);
        SetDuplex(cl, xprt);
    }

    /* Warn cleanup routines not to close xp_fd */
    xprt->xp_flags |= SVC_XPRT_FLAG_DONTCLOSE;

unlock:
    mutex_unlock(&xprt->xp_lock);

fail:
    /* for a dedicated channel, unregister and free xprt */
    if ((flags & SVC_VC_CREATE_FLAG_SPLX) &&
        (flags & SVC_VC_CREATE_FLAG_DISPOSE)) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "%s:  disposing--calls svc_vc_destroy_xprt\n",
                __func__);
        svc_vc_destroy(xprt);
    }

    return (cl);
}

/*
 * Create an RPC SVCXPRT handle from an active client transport
 * handle, i.e., to service RPC requests.
 *
 * If flags & SVC_VC_CREATE_CL_FLAG_DEDICATED, then cl is also
 * deallocated without closing cl->cl_private->ct_fd.
 */
SVCXPRT *
svc_vc_create_from_clnt(CLIENT *cl,
                        const u_int sendsz,
                        const u_int recvsz,
                        const uint32_t flags)
{

    int fd;
    socklen_t len;
    struct cf_conn *cd;
    struct cx_data *cx = (struct cx_data *) cl->cl_private;
    struct ct_data *ct = CT_DATA(cx);
    struct sockaddr_storage addr;
    struct __rpc_sockinfo si;
    sigset_t mask;
    SVCXPRT *xprt = NULL;

    fd = cx->cx_fd;
    thr_sigsetmask(SIG_SETMASK, (sigset_t *) 0, &mask);
    rpc_dplx_slc(cl, &mask);
    rpc_dplx_rlc(cl, &mask);

    /*
     * make a new transport
     */

    xprt = makefd_xprt(fd, sendsz, recvsz);
    if (! xprt)
        goto unlock;

    len = sizeof (struct sockaddr_storage);
    if (getpeername(fd, (struct sockaddr *)(void *)&addr, &len) < 0) {
        __warnx(TIRPC_DEBUG_FLAG_SVC_VC,
                "%s: could not retrieve remote addr",
                __func__);
        svc_vc_destroy_xprt(xprt);
        goto unlock;
    }

    if (!__rpc_set_netbuf(&xprt->xp_rtaddr, &addr, len)) {
        /* keeps connected state, duplex clnt */
        svc_vc_destroy_xprt(xprt);
        goto unlock;
    }

    __xprt_set_raddr(xprt, &addr);

    if (__rpc_fd2sockinfo(fd, &si) && si.si_proto == IPPROTO_TCP) {
        len = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len, sizeof (len));
    }

    cd = (struct cf_conn *) xprt->xp_p1;

    cd->sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int) sendsz);
    cd->recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int) recvsz);
    cd->maxrec = __svc_maxrec;

#if 0 /* XXX wont currently support */
    if (cd->maxrec != 0) {
        fflags = fcntl(fd, F_GETFL, 0);
        if (fflags  == -1)
            return (FALSE);
        if (fcntl(fd, F_SETFL, fflags | O_NONBLOCK) == -1)
            return (FALSE);
        if (cd->recvsize > cd->maxrec)
            cd->recvsize = cd->maxrec;
        cd->nonblock = TRUE;
        __xdrrec_setnonblock(&cd->xdrs, cd->maxrec);
    } else
        cd->nonblock = FALSE;
#else
    cd->nonblock = FALSE;
#endif

    gettimeofday(&cd->last_recv_time, NULL);

    /* conditional xprt_register */
    if ((! (__svc_params->flags & SVC_FLAG_NOREG_XPRTS)) &&
        (! (flags & SVC_VC_CREATE_FLAG_XPRT_NOREG)))
        xprt_register(xprt);

    /* XXX duplex */
    if (flags & SVC_VC_CREATE_FLAG_DPLX)
        SetDuplex(cl, xprt);

    /* If creating a dedicated channel collect the supplied client
     * without closing fd */
    if ((flags & SVC_VC_CREATE_FLAG_SPLX) &&
        (flags & SVC_VC_CREATE_FLAG_DISPOSE)) {
        ct->ct_closeit = FALSE; /* must not close */
        CLNT_DESTROY(cl); /* clean up immediately */
    }

unlock:
    rpc_dplx_ruc(cl, &mask);
    rpc_dplx_suc(cl, &mask);

    return (xprt);
}

/*
 * Destroy a transport handle.  Do not alter connected transport state.
 */
void svc_vc_destroy_xprt(SVCXPRT * xprt)
{
    struct cf_conn *cd = NULL;

    if(xprt == NULL)
        return;

    cd = (struct cf_conn *) xprt->xp_p1;
    if(cd == NULL)
        return;

    svc_rqst_finalize_xprt(xprt);

    XDR_DESTROY(&(cd->xdrs_in));
    XDR_DESTROY(&(cd->xdrs_out));

    mem_free(cd, sizeof(struct cf_conn));
    mem_free(xprt, sizeof(SVCXPRT));
}

/*
 * Construct a service transport, unassociated with any transport
 * connection.
 */
SVCXPRT *svc_vc_create_xprt(u_long sendsz, u_long recvsz)
{
    SVCXPRT *xprt;
    struct cf_conn *cd;

    xprt = (SVCXPRT *) mem_alloc(sizeof(SVCXPRT));
    if(xprt == NULL) {
        goto done;
    }

    cd = (struct cf_conn *) mem_alloc(sizeof(struct cf_conn));
    if(cd == NULL) {
        mem_free(xprt, sizeof(SVCXPRT));
        xprt = NULL;
        goto done;
    }

    svc_rqst_init_xprt(xprt);

    cd->strm_stat = XPRT_IDLE;

#if XDR_VREC
    /* parallel send/recv */
    xdr_vrec_create(&(cd->xdrs_in),
                    XDR_VREC_IN, xprt, readv_vc, NULL, recvsz,
                    VREC_FLAG_NONE);

    xdr_vrec_create(&(cd->xdrs_out),
                    XDR_VREC_OUT, xprt, NULL, writev_vc, sendsz,
                    VREC_FLAG_NONE);
#else
    /* XXX */
    xdrrec_create(&(cd->xdrs_in), sendsz, recvsz, xprt,
                  read_vc, write_vc);
    xdrrec_create(&(cd->xdrs_out), sendsz, recvsz, xprt,
                  read_vc, write_vc);
#endif

    xprt->xp_p1 = cd;
    xprt->xp_verf.oa_base = cd->verf_body;

done:
    return (xprt);
}
