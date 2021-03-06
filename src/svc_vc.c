
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

#include <rpc/rpc.h>

#include "rpc_com.h"
#include "clnt_internal.h"

#include <getpeereid.h>

extern svc_params __svc_params[1];
extern rwlock_t svc_fd_lock;

static bool_t rendezvous_request(SVCXPRT *, struct rpc_msg *);
static enum xprt_stat rendezvous_stat(SVCXPRT *);
static void svc_vc_destroy(SVCXPRT *);
static void __svc_vc_dodestroy (SVCXPRT *);
static int read_vc(void *, void *, int);
static int write_vc(void *, void *, int);
static enum xprt_stat svc_vc_stat(SVCXPRT *);
static bool_t svc_vc_recv(SVCXPRT *, struct rpc_msg *);
static bool_t svc_vc_getargs(SVCXPRT *, xdrproc_t, void *);
static bool_t svc_vc_freeargs(SVCXPRT *, xdrproc_t, void *);
static bool_t svc_vc_reply(SVCXPRT *, struct rpc_msg *);
static void svc_vc_rendezvous_ops(SVCXPRT *);
static void svc_vc_ops(SVCXPRT *);
static bool_t svc_vc_control(SVCXPRT *xprt, const u_int rq, void *in);
static bool_t svc_vc_rendezvous_control (SVCXPRT *xprt, const u_int rq,
				   	     void *in);
void clnt_vc_destroy(CLIENT *);

static void map_ipv4_to_ipv6(sin, sin6)
struct sockaddr_in *sin;
struct sockaddr_in6 *sin6;
{
  sin6->sin6_family = AF_INET6;
  sin6->sin6_port = sin->sin_port;
  sin6->sin6_addr.s6_addr32[0] = 0;
  sin6->sin6_addr.s6_addr32[1] = 0;
  sin6->sin6_addr.s6_addr32[2] = htonl(0xffff);
  sin6->sin6_addr.s6_addr32[3] = *(uint32_t *) & sin->sin_addr;
}

/*
 * Usage:
 *	xprt = svc_vc_create(sock, send_buf_size, recv_buf_size);
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
 */
SVCXPRT *
svc_vc_create(fd, sendsize, recvsize)
	int fd;
	u_int sendsize;
	u_int recvsize;
{
	SVCXPRT *xprt;
	struct cf_rendezvous *r = NULL;
	struct __rpc_sockinfo si;
	struct sockaddr_storage sslocal;
	socklen_t slen;

	r = mem_alloc(sizeof(*r));
	if (r == NULL) {
		__warnx("svc_vc_create: out of memory");
		goto cleanup_svc_vc_create;
	}
	if (!__rpc_fd2sockinfo(fd, &si))
		return NULL;
	r->sendsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)sendsize);
	r->recvsize = __rpc_get_t_size(si.si_af, si.si_proto, (int)recvsize);
	r->maxrec = __svc_maxrec;
	xprt = mem_alloc(sizeof(SVCXPRT));
	if (xprt == NULL) {
		__warnx("svc_vc_create: out of memory");
		goto cleanup_svc_vc_create;
	}
	xprt->xp_flags = SVC_XPORT_FLAG_NONE;
	xprt->xp_tp = NULL;
	xprt->xp_p1 = r;
	xprt->xp_p2 = NULL;
	xprt->xp_p3 = NULL;
	xprt->xp_auth = NULL;
	xprt->xp_verf = _null_auth;
	svc_vc_rendezvous_ops(xprt);
	xprt->xp_port = (u_short)-1;	/* It is the rendezvouser */
	xprt->xp_fd = fd;

	slen = sizeof (struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&sslocal, &slen) < 0) {
		__warnx("svc_vc_create: could not retrieve local addr");
		goto cleanup_svc_vc_create;
	}

	if (!__rpc_set_netbuf(&xprt->xp_ltaddr, &sslocal, sizeof(sslocal))) {
		__warnx("svc_vc_create: no mem for local addr");
		goto cleanup_svc_vc_create;
	}
	xprt_register(xprt);
	return (xprt);
cleanup_svc_vc_create:
	if (r != NULL)
		mem_free(r, sizeof(*r));
	return (NULL);
}

/*
 * Like svtcp_create(), except the routine takes any *open* UNIX file
 * descriptor as its first input.
 */
SVCXPRT *
svc_fd_create(fd, sendsize, recvsize)
	int fd;
	u_int sendsize;
	u_int recvsize;
{
	struct sockaddr_storage ss;
	socklen_t slen;
	SVCXPRT *ret;

	assert(fd != -1);

	ret = makefd_xprt(fd, sendsize, recvsize);
	if (ret == NULL)
		return NULL;

	slen = sizeof (struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx("svc_fd_create: could not retrieve local addr");
		goto freedata;
	}
	if (!__rpc_set_netbuf(&ret->xp_ltaddr, &ss, sizeof(ss))) {
		__warnx("svc_fd_create: no mem for local addr");
		goto freedata;
	}

	slen = sizeof (struct sockaddr_storage);
	if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx("svc_fd_create: could not retrieve remote addr");
		goto freedata;
	}
	if (!__rpc_set_netbuf(&ret->xp_rtaddr, &ss, sizeof(ss))) {
		__warnx("svc_fd_create: no mem for local addr");
		goto freedata;
	}

	/* Set xp_raddr for compatibility */
	__xprt_set_raddr(ret, &ss);

	return ret;

freedata:
	if (ret->xp_ltaddr.buf != NULL)
		mem_free(ret->xp_ltaddr.buf, ret->xp_ltaddr.maxlen);

	return NULL;
}

/*
 * Like sv_fd_create(), except export flags for additional control.  Add
 * special handling for AF_INET and AFS_INET6.  Possibly not needed,
 * because no longer called in Ganesha.
 */
SVCXPRT *
svc_fd_create2(fd, sendsize, recvsize, flags)
	int fd;
	u_int sendsize;
	u_int recvsize;
	u_int flags;
{
	struct sockaddr_storage ss;
	struct sockaddr_in6 sin6;
	struct netbuf *addr;
	socklen_t slen;
	SVCXPRT *ret;
	int af;

	assert(fd != -1);

	ret = makefd_xprt(fd, sendsize, recvsize);
	if (ret == NULL)
		return NULL;

	slen = sizeof (struct sockaddr_storage);
	if (getsockname(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx("svc_fd_create: could not retrieve local addr");
		goto freedata;
	}
	if (!__rpc_set_netbuf(&ret->xp_ltaddr, &ss, sizeof(ss))) {
		__warnx("svc_fd_create: no mem for local addr");
		goto freedata;
	}

	slen = sizeof (struct sockaddr_storage);
	if (getpeername(fd, (struct sockaddr *)(void *)&ss, &slen) < 0) {
		__warnx("svc_fd_create: could not retrieve remote addr");
		goto freedata;
	}
	af = ss.ss_family;

	/* XXX Ganesha concepts, and apparently no longer used, check */
	if (flags & SVC_VCCR_MAP6_V1) {
	    if (af == AF_INET) {
		map_ipv4_to_ipv6((struct sockaddr_in *)&ss, &sin6);
		addr = __rpc_set_netbuf(&ret->xp_rtaddr, &ss, sizeof(ss));
	    }
	    else
		addr = __rpc_set_netbuf(&ret->xp_rtaddr, &sin6, sizeof(ss));
	} else
	    addr = __rpc_set_netbuf(&ret->xp_rtaddr, &ss, sizeof(ss));
	if (!addr) {
		__warnx("svc_fd_create: no mem for local addr");
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
	    __xprt_set_raddr(ret, &ss);
	}
out:
	return ret;

freedata:
	if (ret->xp_ltaddr.buf != NULL)
		mem_free(ret->xp_ltaddr.buf, ret->xp_ltaddr.maxlen);

	return NULL;
}

SVCXPRT *
makefd_xprt(fd, sendsize, recvsize)
	int fd;
	u_int sendsize;
	u_int recvsize;
{
	SVCXPRT *xprt;
	struct cf_conn *cd;
	const char *netid;
	struct __rpc_sockinfo si;
 
	assert(fd != -1);

        /* XXX an ordering inelegance, at best */
        if (! __svc_params->max_connections)
            __svc_params->max_connections = FD_SETSIZE;

        if (fd >= __svc_params->max_connections) {
                __warnx("svc_vc: makefd_xprt: fd too high\n");
                xprt = NULL;
                goto done;
        }

	xprt = mem_alloc(sizeof(SVCXPRT));
	if (xprt == NULL) {
		__warnx("svc_vc: makefd_xprt: out of memory");
		goto done;
	}
	memset(xprt, 0, sizeof *xprt);
	rwlock_init(&xprt->lock, NULL);
	cd = mem_alloc(sizeof(struct cf_conn));
	if (cd == NULL) {
		__warnx("svc_tcp: makefd_xprt: out of memory");
		mem_free(xprt, sizeof(SVCXPRT));
		xprt = NULL;
		goto done;
	}
	cd->strm_stat = XPRT_IDLE;
	xdrrec_create(&(cd->xdrs), sendsize, recvsize,
	    xprt, read_vc, write_vc);
	xprt->xp_p1 = cd;
	xprt->xp_auth = NULL;
	xprt->xp_verf.oa_base = cd->verf_body;
	/* the SVCXPRT created in svc_vc_create accepts new connections
	 * in its xp_recv op, the rendezvous_request method, but xprt is
	 * a call channel */
	svc_vc_ops(xprt);
	xprt->xp_port = 0;  /* this is a connection, not a rendezvouser */
	xprt->xp_fd = fd;
        if (__rpc_fd2sockinfo(fd, &si) && __rpc_sockinfo2netid(&si, &netid))
		xprt->xp_netid = strdup(netid);

	xprt_register(xprt);
done:
	return (xprt);
}

/*ARGSUSED*/
static bool_t
rendezvous_request(xprt, msg)
	SVCXPRT *xprt;
	struct rpc_msg *msg;
{
	int sock, flags;
	struct cf_rendezvous *r;
	struct cf_conn *cd;
	struct sockaddr_storage addr;
	socklen_t len;
	struct __rpc_sockinfo si;
	SVCXPRT *newxprt;
	fd_set cleanfds;

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
                        
                        break;
#endif
                    default:
			cleanfds = svc_fdset;
			__svc_clean_idle2(0, FALSE);
                        break;
                    } /* switch */
                    goto again;
		}
		return (FALSE);
	}
	/*
	 * make a new transporter (re-uses xprt)
	 */

	newxprt = makefd_xprt(sock, r->sendsize, r->recvsize);

	if (!__rpc_set_netbuf(&newxprt->xp_rtaddr, &addr, len))
		return (FALSE);

	__xprt_set_raddr(newxprt, &addr);

	if (__rpc_fd2sockinfo(sock, &si) && si.si_proto == IPPROTO_TCP) {
		len = 1;
		/* XXX fvdl - is this useful? */
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &len, sizeof (len));
	}

	cd = (struct cf_conn *)newxprt->xp_p1;

	cd->recvsize = r->recvsize;
	cd->sendsize = r->sendsize;
	cd->maxrec = r->maxrec;

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

	gettimeofday(&cd->last_recv_time, NULL);

	return (FALSE); /* there is never an rpc msg to be processed */
}

/*ARGSUSED*/
static enum xprt_stat
rendezvous_stat(xprt)
	SVCXPRT *xprt;
{

	return (XPRT_IDLE);
}

static void
svc_vc_destroy(xprt)
	SVCXPRT *xprt;
{
	assert(xprt != NULL);
	
	xprt_unregister(xprt);
	__svc_vc_dodestroy(xprt);
}

static void
__svc_vc_dodestroy(xprt)
	SVCXPRT *xprt;
{
	struct cf_conn *cd;
	struct cf_rendezvous *r;

	cd = (struct cf_conn *)xprt->xp_p1;

	/* Omit close in cases such as donation of the connection
	 * to a client transport handle */
	if ((xprt->xp_fd != RPC_ANYFD) &&
	    (!(xprt->xp_flags & SVC_XPORT_FLAG_DONTCLOSE)))
	    (void)close(xprt->xp_fd);

	if (xprt->xp_port != 0) {
		/* a rendezvouser socket */
		r = (struct cf_rendezvous *)xprt->xp_p1;
		mem_free(r, sizeof (struct cf_rendezvous));
		xprt->xp_port = 0;
	} else {
		/* an actual connection socket */
		XDR_DESTROY(&(cd->xdrs));
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
		free(xprt->xp_tp); /* XXX check why not mem_alloc/free */

	if (xprt->xp_netid)
		free(xprt->xp_netid); /* XXX check why not mem_alloc/free */

	mem_free(xprt, sizeof(SVCXPRT));
}

/*ARGSUSED*/
static bool_t
svc_vc_control(xprt, rq, in)
	SVCXPRT *xprt;
	const u_int rq;
	void *in;
{
	switch (rq) {
	case SVCGET_XP_FLAGS:
	    *(u_int *)in = xprt->xp_flags;
	    break;
	case SVCSET_XP_FLAGS:
	    xprt->xp_flags = *(u_int *)in;
	    break;
	case SVCGET_XP_RECV:
	    *(xp_recv_t *)in = xprt->xp_ops->xp_recv;
	    break;
	case SVCSET_XP_RECV:
	    xprt->xp_ops->xp_recv = *(xp_recv_t)in;
	    break;
	default:
	    return (FALSE);
	}
	return (TRUE);
}

static bool_t
svc_vc_rendezvous_control(xprt, rq, in)
	SVCXPRT *xprt;
	const u_int rq;
	void *in;
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
read_vc(xprtp, buf, len)
	void *xprtp;
	void *buf;
	int len;
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
write_vc(xprtp, buf, len)
	void *xprtp;
	void *buf;
	int len;
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

static enum xprt_stat
svc_vc_stat(xprt)
	SVCXPRT *xprt;
{
	struct cf_conn *cd;

	assert(xprt != NULL);

	cd = (struct cf_conn *)(xprt->xp_p1);

	if (cd->strm_stat == XPRT_DIED)
		return (XPRT_DIED);
	if (! xdrrec_eof(&(cd->xdrs)))
		return (XPRT_MOREREQS);
	return (XPRT_IDLE);
}

static bool_t
svc_vc_recv(xprt, msg)
	SVCXPRT *xprt;
	struct rpc_msg *msg;
{
	struct cf_conn *cd;
	XDR *xdrs;

	assert(xprt != NULL);
	assert(msg != NULL);

	cd = (struct cf_conn *)(xprt->xp_p1);
	xdrs = &(cd->xdrs);

	if (cd->nonblock) {
		if (!__xdrrec_getrec(xdrs, &cd->strm_stat, TRUE))
			return FALSE;
	}

	xdrs->x_op = XDR_DECODE;
	/*
	 * No need skip records with nonblocking connections
	 */
	if (cd->nonblock == FALSE)
		(void)xdrrec_skiprecord(xdrs);
	if (xdr_callmsg(xdrs, msg)) {
		cd->x_id = msg->rm_xid;
		return (TRUE);
	}
	cd->strm_stat = XPRT_DIED;
	return (FALSE);
}

static bool_t
svc_vc_getargs(xprt, xdr_args, args_ptr)
	SVCXPRT *xprt;
	xdrproc_t xdr_args;
	void *args_ptr;
{

	assert(xprt != NULL);
	/* args_ptr may be NULL */

	if (! SVCAUTH_UNWRAP(xprt->xp_auth,
			     &(((struct cf_conn *)(xprt->xp_p1))->xdrs),
			     xdr_args, args_ptr)) {
		return FALSE;  
	}
	return TRUE;
}

static bool_t
svc_vc_freeargs(xprt, xdr_args, args_ptr)
	SVCXPRT *xprt;
	xdrproc_t xdr_args;
	void *args_ptr;
{
	XDR *xdrs;

	assert(xprt != NULL);
	/* args_ptr may be NULL */

	xdrs = &(((struct cf_conn *)(xprt->xp_p1))->xdrs);

	xdrs->x_op = XDR_FREE;
	return ((*xdr_args)(xdrs, args_ptr));
}

static bool_t
svc_vc_reply(xprt, msg)
	SVCXPRT *xprt;
	struct rpc_msg *msg;
{
	struct cf_conn *cd;
	XDR *xdrs;
	bool_t rstat;

	xdrproc_t xdr_results;
	caddr_t xdr_location;
	bool_t has_args;

	assert(xprt != NULL);
	assert(msg != NULL);

	cd = (struct cf_conn *)(xprt->xp_p1);
	xdrs = &(cd->xdrs);

	if (msg->rm_reply.rp_stat == MSG_ACCEPTED &&
	    msg->rm_reply.rp_acpt.ar_stat == SUCCESS) {
		has_args = TRUE;
		xdr_results = msg->acpted_rply.ar_results.proc;
		xdr_location = msg->acpted_rply.ar_results.where;

		msg->acpted_rply.ar_results.proc = (xdrproc_t)xdr_void;
		msg->acpted_rply.ar_results.where = NULL;
	} else
		has_args = FALSE;

	xdrs->x_op = XDR_ENCODE;
	msg->rm_xid = cd->x_id;
	rstat = FALSE;
	if (xdr_replymsg(xdrs, msg) &&
	    (!has_args || (xprt->xp_auth &&
	     SVCAUTH_WRAP(xprt->xp_auth, xdrs, xdr_results, xdr_location)))) {
		rstat = TRUE;
	}
	(void)xdrrec_endofrecord(xdrs, TRUE);
	return (rstat);
}

static void
svc_vc_ops(xprt)
	SVCXPRT *xprt;
{
	static struct xp_ops ops;
	static struct xp_ops2 ops2;
	extern mutex_t ops_lock;

/* VARIABLES PROTECTED BY ops_lock: ops, ops2 */

	mutex_lock(&ops_lock);
	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_vc_recv;
		ops.xp_stat = svc_vc_stat;
		ops.xp_getargs = svc_vc_getargs;
		ops.xp_reply = svc_vc_reply;
		ops.xp_freeargs = svc_vc_freeargs;
		ops.xp_destroy = svc_vc_destroy;
		ops2.xp_control = svc_vc_control;
	}
	xprt->xp_ops = &ops;
	xprt->xp_ops2 = &ops2;
	mutex_unlock(&ops_lock);
}

static void
svc_vc_rendezvous_ops(xprt)
	SVCXPRT *xprt;
{
	static struct xp_ops ops;
	static struct xp_ops2 ops2;
	extern mutex_t ops_lock;

	mutex_lock(&ops_lock);
	if (ops.xp_recv == NULL) {
		ops.xp_recv = rendezvous_request;
		ops.xp_stat = rendezvous_stat;
		ops.xp_getargs =
		    (bool_t (*)(SVCXPRT *, xdrproc_t, void *))abort;
		ops.xp_reply =
		    (bool_t (*)(SVCXPRT *, struct rpc_msg *))abort;
		ops.xp_freeargs =
		    (bool_t (*)(SVCXPRT *, xdrproc_t, void *))abort,
		ops.xp_destroy = svc_vc_destroy;
		ops2.xp_control = svc_vc_rendezvous_control;
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

bool_t
__svc_clean_idle(fd_set *fds, int timeout, bool_t cleanblock)
{
	int i, ncleaned;
	SVCXPRT *xprt, *least_active;
	struct timeval tv, tdiff, tmax;
	struct cf_conn *cd;

	gettimeofday(&tv, NULL);
	tmax.tv_sec = tmax.tv_usec = 0;
	least_active = NULL;
	rwlock_wrlock(&svc_fd_lock);
	for (i = ncleaned = 0; i <= svc_maxfd; i++) {
		xprt = __svc_xports[i];
		if (FD_ISSET(i, fds)) {			
			if (xprt == NULL || xprt->xp_ops == NULL ||
			    xprt->xp_ops->xp_recv != svc_vc_recv)
				continue;
			cd = (struct cf_conn *)xprt->xp_p1;
			if (!cleanblock && !cd->nonblock)
				continue;
			if (timeout == 0) {
				timersub(&tv, &cd->last_recv_time, &tdiff);
				if (timercmp(&tdiff, &tmax, >)) {
					tmax = tdiff;
					least_active = xprt;
				}
				continue;
			}
			if (tv.tv_sec - cd->last_recv_time.tv_sec > timeout) {
				__xprt_unregister_unlocked(xprt);
				__svc_vc_dodestroy(xprt);
				ncleaned++;
			}
		} /* ! FD_ISSET */
	} /* loop */
	if (timeout == 0 && least_active != NULL) {
		__xprt_unregister_unlocked(least_active);
		__svc_vc_dodestroy(least_active);
		ncleaned++;
	}
	rwlock_unlock(&svc_fd_lock);
	return ncleaned > 0 ? TRUE : FALSE;
} /* __svc_clean_idle */

/*
 * Like __svc_clean_idle but event-type independent.  For now no cleanfds.
 */
bool_t
__svc_clean_idle2(int timeout, bool_t cleanblock)
{
	int i, ncleaned;
	SVCXPRT *xprt, *least_active;
	struct timeval tv, tdiff, tmax;
	struct cf_conn *cd;

	gettimeofday(&tv, NULL);
	tmax.tv_sec = tmax.tv_usec = 0;
	least_active = NULL;
	rwlock_wrlock(&svc_fd_lock);
	for (i = ncleaned = 0; i <= svc_maxfd; i++) {
		xprt = __svc_xports[i];
		if (TRUE) { /* flag in__svc_params->ev_u.epoll? */
			if (xprt == NULL || xprt->xp_ops == NULL ||
			    xprt->xp_ops->xp_recv != svc_vc_recv)
				continue;
			cd = (struct cf_conn *)xprt->xp_p1;
			if (!cleanblock && !cd->nonblock)
				continue;
			if (timeout == 0) {
				timersub(&tv, &cd->last_recv_time, &tdiff);
				if (timercmp(&tdiff, &tmax, >)) {
					tmax = tdiff;
					least_active = xprt;
				}
				continue;
			}
			if (tv.tv_sec - cd->last_recv_time.tv_sec > timeout) {
				__xprt_unregister_unlocked(xprt);
				__svc_vc_dodestroy(xprt);
				ncleaned++;
			}
		} /* TRUE */
	} /* loop */
	if (timeout == 0 && least_active != NULL) {
		__xprt_unregister_unlocked(least_active);
		__svc_vc_dodestroy(least_active);
		ncleaned++;
	}
	rwlock_unlock(&svc_fd_lock);
	return ncleaned > 0 ? TRUE : FALSE;
} /* __svc_clean_idle2 */

/*
 * Create an RPC client handle from an active service transport
 * handle, i.e., to issue calls on the channel.
 *
 * If flags & SVC_VC_CLNT_CREATE_DEDICATED, the supplied xprt will be
 * unregistered and disposed inline.
 */
CLIENT *
svc_vc_clnt_create(xprt, prog, vers, flags)
	SVCXPRT *xprt;
	const rpcprog_t prog;
	const rpcvers_t vers;
	const uint32_t flags;
{
	CLIENT *cl;
	struct cf_conn *cd;

	rwlock_wrlock (&xprt->lock);

	/* Once */
	if (xprt->xp_p4) {
	    cl = (CLIENT *) xprt->xp_p4;
            rwlock_unlock (&xprt->lock);
	    goto out;
	}

	/* Create a client transport handle.  The endpoint is already
	 * connected. */
	cd = (struct cf_conn *) xprt->xp_p1;
	cl  = clnt_vc_create2(xprt->xp_fd,
			      &xprt->xp_rtaddr,
			      prog,
			      vers,
			      cd->recvsize,
			      cd->sendsize,
			      CLNT_CREATE_FLAG_SVCXPRT);
	xprt->xp_p4 = cl;

	/* Warn cleanup routines not to close xp_fd */
	xprt->xp_flags |= SVC_XPORT_FLAG_DONTCLOSE;

        /* In this case, unregister and free xprt */
	if (flags & SVC_VC_CLNT_CREATE_DEDICATED)
            svc_vc_destroy(xprt);
out:
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
svc_vc_create_cl(cl, sendsize, recvsize, flags)
	CLIENT *cl;
	const u_int sendsize;
	const u_int recvsize;
	const uint32_t flags;
{

    int fd, fflags;
    socklen_t len;
    struct cf_conn *cd;
    struct ct_data *ct;
    struct sockaddr_storage addr;
    struct __rpc_sockinfo si;
    SVCXPRT *xprt;

    ct = (struct ct_data *) cl->cl_private;
    fd = ct->ct_fd;

    /*
     * make a new transport
     */

    xprt = makefd_xprt(fd, sendsize, recvsize);

    if (!__rpc_set_netbuf(&xprt->xp_rtaddr, &addr, len))
		return (FALSE);

    __xprt_set_raddr(xprt, &addr);
    
    if (__rpc_fd2sockinfo(fd, &si) && si.si_proto == IPPROTO_TCP) {
	len = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &len, sizeof (len));
    }

    cd = (struct cf_conn *) xprt->xp_p1;

    cd->recvsize = recvsize;
    cd->sendsize = sendsize;
    cd->maxrec = __svc_maxrec;

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

    gettimeofday(&cd->last_recv_time, NULL);

    /* If creating a dedicated channel collect the supplied client
     * without closing fd */
    if (flags & SVC_VC_CREATE_CL_FLAG_DEDICATED) {
	ct->ct_closeit = FALSE; /* must not close */
	/* clean up immediately */
	clnt_vc_destroy(cl);
    }

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
    
    XDR_DESTROY(&(cd->xdrs));
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

    cd->strm_stat = XPRT_IDLE;
    xdrrec_create(&(cd->xdrs), sendsz, recvsz, xprt, read_vc, write_vc);
    
    xprt->xp_p1 = cd;
    xprt->xp_verf.oa_base = cd->verf_body;

done:
    return (xprt);
 }

/*
 * Duplicate xprt from original to copy.
 */
void svc_vc_copy_xprt(SVCXPRT *xprt_copy, SVCXPRT *xprt_orig)
{
    struct cf_conn *cd_copy = (struct cf_conn *)(xprt_copy->xp_p1);
    struct cf_conn *cd_orig = (struct cf_conn *)(xprt_orig->xp_p1);

    memcpy(xprt_copy, xprt_orig, sizeof(SVCXPRT));
    xprt_copy->xp_p1 = cd_copy;
    xprt_copy->xp_verf.oa_base = cd_copy->verf_body;

    cd_copy->strm_stat = cd_orig->strm_stat;
    cd_copy->x_id = cd_orig->x_id;
    memcpy(cd_copy->verf_body, cd_orig->verf_body, MAX_AUTH_BYTES);
}
