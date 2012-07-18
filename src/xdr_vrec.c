
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
#include <sys/cdefs.h>
#include <sys/cdefs.h>

/*
 * xdr_rec.c, Implements TCP/IP based XDR streams with a "record marking"
 * layer above tcp (for rpc's use).
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * These routines interface XDRSTREAMS to a tcp/ip connection.
 * There is a record marking layer between the xdr stream
 * and the tcp transport level.  A record is composed on one or more
 * record fragments.  A record fragment is a thirty-two bit header followed
 * by n bytes of data, where n is contained in the header.  The header
 * is represented as a htonl(u_long).  Thegh order bit encodes
 * whether or not the fragment is the last fragment of the record
 * (1 => fragment is last, 0 => more fragments to follow.
 * The other 31 bits encode the byte length of the fragment.
 */

#include <sys/types.h>

#include <netinet/in.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <stddef.h>
#include <intrinsic.h>
#include "rpc_com.h"

#include <rpc/xdr_vrec.h>

static bool xdr_vrec_getlong(XDR *, long *);
static bool xdr_vrec_putlong(XDR *, const long *);
static bool xdr_vrec_getbytes(XDR *, char *, u_int);
static bool xdr_vrec_putbytes(XDR *, const char *, u_int);
static bool xdr_vrec_getbytes2(XDR *, char *, u_int, u_int);
static bool xdr_vrec_putbytes2(XDR *, const char *, u_int, u_int);
static u_int xdr_vrec_getpos(XDR *);
static bool xdr_vrec_setpos(XDR *, u_int);
static int32_t *xdr_vrec_inline(XDR *, u_int);
static void xdr_vrec_destroy(XDR *);
static bool xdr_vrec_noop(void);

typedef bool (* dummyfunc3)(XDR *, int, void *);
typedef bool (* dummyfunc4)(XDR *, const char *, u_int, u_int);

static const struct  xdr_ops xdr_vrec_ops = {
    xdr_vrec_getlong,
    xdr_vrec_putlong,
    xdr_vrec_getbytes,
    xdr_vrec_putbytes,
    xdr_vrec_getpos,
    xdr_vrec_setpos,
    xdr_vrec_inline,
    xdr_vrec_destroy,
    (dummyfunc3) xdr_vrec_noop, /* x_control */
    xdr_vrec_getbytes2,
    xdr_vrec_putbytes2
};

/*
 * A record is composed of one or more record fragments.
 * A record fragment is a four-byte header followed by zero to
 * 2**32-1 bytes.  The header is treated as a long unsigned and is
 * encode/decoded to the network via htonl/ntohl.  The low order 31 bits
 * are a byte count of the fragment.  The highest order bit is a boolean:
 * 1 => this fragment is the last fragment of the record,
 * 0 => this fragment is followed by more fragment(s).
 *
 * The fragment/record machinery is not general;  it is constructed to
 * meet the needs of xdr and rpc based on tcp.
 */

#define LAST_FRAG ((u_int32_t)(1 << 31))

static u_int fix_buf_size(u_int);
static bool flush_out(V_RECSTREAM *, bool);
static bool fill_input_buf(V_RECSTREAM *);
static bool get_input_bytes(V_RECSTREAM *, char *, int);
static bool set_input_fragment(V_RECSTREAM *);
static bool skip_input_bytes(V_RECSTREAM *, long);

/* XXX might not be faster than jemalloc, &c? */
static inline void
init_prealloc_queues(V_RECSTREAM *vrstrm)
{
    int ix;
    struct v_rec *vrec;

    opr_queue_Init(&vrstrm->prealloc.v_req.q);
    vrstrm->prealloc.v_req.size = 0;

    opr_queue_Init(&vrstrm->prealloc.v_req_buf.q);
    vrstrm->prealloc.v_req_buf.size = 0;

    for (ix = 0; ix < VQSIZE; ++ix) {
        vrec = mem_alloc(sizeof(struct v_rec));
        opr_queue_Append(&vrstrm->prealloc.v_req.q, &vrec->q);
        (vrstrm->prealloc.v_req.size)++;
    }
}

static inline struct v_rec *
vrec_get_vrec(V_RECSTREAM *vrstrm)
{
    struct v_rec *vrec;
    if (unlikely(vrstrm->prealloc.v_req.size == 0)) {
        vrec = mem_alloc(sizeof(struct v_rec));
    } else {
        vrec = opr_queue_First(&vrstrm->prealloc.v_req.q, struct v_rec, q);
        opr_queue_Remove(&vrec->q);
        (vrstrm->prealloc.v_req.size)--;
    }
    return (vrec);
}

static inline void
vrec_put_vrec(V_RECSTREAM *vrstrm, struct v_rec *vrec)
{
    opr_queue_Append(&vrstrm->prealloc.v_req.q, &vrec->q);
    (vrstrm->prealloc.v_req.size)++;
}

#if 0 /* jemalloc docs warn about reclaim */
#define vrec_alloc_buffer(size) mem_alloc_aligned(0x8, (size))
#else
#define vrec_alloc_buffer(size) mem_alloc((size))
#endif
#define vrec_release_buffer(addr) mem_free((addr), 0)

#define vrec_qlen(q) ((q)->size)
#define vrec_inpos(vrstrm) (&vrstrm->in_q.pos)
#define vrec_outpos(vrstrm) (&vrstrm->out_q.pos)

static inline void vrec_append_rec(struct v_rec_queue *q, struct v_rec *vrec)
{
    opr_queue_Append(&q->q, &vrec->q);
}


/*
 * Create an xdr handle for xdrrec
 * xdrrec_create fills in xdrs.  Sendsize and recvsize are
 * send and recv buffer sizes (0 => use default).
 * tcp_handle is an opaque handle that is passed as the first parameter to
 * the procedures readit and writeit.  Readit and writeit are read and
 * write respectively.   They are like the system
 * calls expect that they take an opaque handle rather than an fd.
 */
void
xdr_vrec_create(XDR *xdrs,
                u_int sendsize,
                u_int recvsize,
                void *tcp_handle,
                /* like read, but pass it a tcp_handle, not sock */
                size_t (*xreadv)(void *, struct iovec *, int, uint32_t),
                /* like write, but pass it a tcp_handle, not sock */
                size_t (*xwritev)(void *, struct iovec *, int, uint32_t))
{
    V_RECSTREAM *vrstrm = mem_alloc(sizeof(V_RECSTREAM));

    if (vrstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_vrec_create: out of memory");
        return;
    }

    init_prealloc_queues(vrstrm);

    /*
     * now the rest ...
     */
    xdrs->x_ops = &xdr_vrec_ops;
    xdrs->x_private = vrstrm;
    vrstrm->tcp_handle = tcp_handle;
    vrstrm->readv = xreadv;
    vrstrm->writev = xwritev;

    opr_queue_Init(&vrstrm->out_q.q);

    /* XXX */
    vrstrm->out_finger = vrstrm->out_boundry = vrstrm->out_base;
    vrstrm->frag_header = (u_int32_t *)(void *)vrstrm->out_base;
    vrstrm->out_finger += sizeof(u_int32_t);
    vrstrm->out_boundry += sendsize;
    vrstrm->frag_sent = FALSE;

    opr_queue_Init(&vrstrm->in_q.q);

    vrstrm->in_size = recvsize;
    /* XXX */
    vrstrm->in_boundry = vrstrm->in_base;
    vrstrm->in_finger = (vrstrm->in_boundry += recvsize);
    vrstrm->fbtbc = 0;
    vrstrm->last_frag = TRUE;
    vrstrm->in_haveheader = FALSE;
    vrstrm->in_hdrlen = 0;
    vrstrm->in_hdrp = (char *)(void *)&vrstrm->in_header;
    vrstrm->in_reclen = 0;
    vrstrm->in_received = 0;

#if 0 /* XXX */
    RECSTREAM *rstrm = mem_alloc(sizeof(RECSTREAM));

    if (rstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdrrec_create: out of memory");
        /*
         *  This is bad.  Should rework xdrrec_create to
         *  return a handle, and in this case return NULL
         */
        return;
    }
    rstrm->sendsize = sendsize = fix_buf_size(sendsize);
    rstrm->out_base = mem_alloc(rstrm->sendsize);
    if (rstrm->out_base == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdrrec_create: out of memory");
        mem_free(rstrm, sizeof(RECSTREAM));
        return;
    }
    rstrm->recvsize = recvsize = fix_buf_size(recvsize);
    rstrm->in_base = mem_alloc(recvsize);
    if (rstrm->in_base == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdrrec_create: out of memory");
        mem_free(rstrm->out_base, sendsize);
        mem_free(rstrm, sizeof(RECSTREAM));
        return;
    }
    /*
     * now the rest ...
     */
    xdrs->x_ops = &xdrrec_ops;
    xdrs->x_private = rstrm;
    rstrm->tcp_handle = tcp_handle;
    rstrm->readit = readit;
    rstrm->writeit = writeit;
    rstrm->out_finger = rstrm->out_boundry = rstrm->out_base;
    rstrm->frag_header = (u_int32_t *)(void *)rstrm->out_base;
    rstrm->out_finger += sizeof(u_int32_t);
    rstrm->out_boundry += sendsize;
    rstrm->frag_sent = FALSE;
    rstrm->in_size = recvsize;
    rstrm->in_boundry = rstrm->in_base;
    rstrm->in_finger = (rstrm->in_boundry += recvsize);
    rstrm->fbtbc = 0;
    rstrm->last_frag = TRUE;
    rstrm->in_haveheader = FALSE;
    rstrm->in_hdrlen = 0;
    rstrm->in_hdrp = (char *)(void *)&rstrm->in_header;
    rstrm->nonblock = FALSE;
    rstrm->in_reclen = 0;
    rstrm->in_received = 0;
#endif /* 0 */
}


/*
 * The routines defined below are the xdr ops which will go into the
 * xdr handle filled in by xdrrec_create.
 */

static bool
xdr_vrec_getlong(XDR *xdrs,  long *lp)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int32_t *buflp = (int32_t *)(void *)(rstrm->in_finger);
    int32_t mylong;

    /* first try the inline, fast case */
    if ((rstrm->fbtbc >= sizeof(int32_t)) &&
        (((long)rstrm->in_boundry - (long)buflp) >= sizeof(int32_t))) {
        *lp = (long)ntohl((u_int32_t)(*buflp));
        rstrm->fbtbc -= sizeof(int32_t);
        rstrm->in_finger += sizeof(int32_t);
    } else {
        if (! xdrrec_getbytes(xdrs, (char *)(void *)&mylong,
                              sizeof(int32_t)))
            return (FALSE);
        *lp = (long)ntohl((u_int32_t)mylong);
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_putlong(XDR *xdrs, const long *lp)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int32_t *dest_lp = ((int32_t *)(void *)(rstrm->out_finger));

    if ((rstrm->out_finger += sizeof(int32_t)) > rstrm->out_boundry) {
        /*
         * this case should almost never happen so the code is
         * inefficient
         */
        rstrm->out_finger -= sizeof(int32_t);
        rstrm->frag_sent = TRUE;
        if (! flush_out(rstrm, FALSE))
            return (FALSE);
        dest_lp = ((int32_t *)(void *)(rstrm->out_finger));
        rstrm->out_finger += sizeof(int32_t);
    }
    *dest_lp = (int32_t)htonl((u_int32_t)(*lp));
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool  /* must manage buffers, fragments, and records */
xdr_vrec_getbytes(XDR *xdrs, char *addr, u_int len)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int current;

    while (len > 0) {
        current = (int)rstrm->fbtbc;
        if (current == 0) {
            if (rstrm->last_frag)
                return (FALSE);
            if (! set_input_fragment(rstrm))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        if (! get_input_bytes(rstrm, addr, current))
            return (FALSE);
        addr += current;
        rstrm->fbtbc -= current;
        len -= current;
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool  /* must manage buffers, fragments, and records */
xdr_vrec_getbytes2(XDR *xdrs, char *addr, u_int len, u_int flags)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int current;

    while (len > 0) {
        current = (int)rstrm->fbtbc;
        if (current == 0) {
            if (rstrm->last_frag)
                return (FALSE);
            if (! set_input_fragment(rstrm))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        if (! get_input_bytes(rstrm, addr, current))
            return (FALSE);
        addr += current;
        rstrm->fbtbc -= current;
        len -= current;
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_putbytes(XDR *xdrs, const char *addr, u_int len)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    size_t current;

    while (len > 0) {
        current = (size_t)((u_long)rstrm->out_boundry -
                           (u_long)rstrm->out_finger);
        current = (len < current) ? len : current;
        memmove(rstrm->out_finger, addr, current);
        rstrm->out_finger += current;
        addr += current;
        len -= current;
        if (rstrm->out_finger == rstrm->out_boundry) {
            rstrm->frag_sent = TRUE;
            if (! flush_out(rstrm, FALSE))
                return (FALSE);
        }
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_putbytes2(XDR *xdrs, const char *addr, u_int len, u_int flags)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    size_t current;

    while (len > 0) {
        current = (size_t)((u_long)rstrm->out_boundry -
                           (u_long)rstrm->out_finger);
        current = (len < current) ? len : current;
        memmove(rstrm->out_finger, addr, current);
        rstrm->out_finger += current;
        addr += current;
        len -= current;
        if (rstrm->out_finger == rstrm->out_boundry) {
            rstrm->frag_sent = TRUE;
            if (! flush_out(rstrm, FALSE))
                return (FALSE);
        }
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static u_int
xdr_vrec_getpos(XDR *xdrs)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;
    off_t pos;

    switch (xdrs->x_op) {

    case XDR_ENCODE:
        pos = rstrm->out_finger - rstrm->out_base
            - BYTES_PER_XDR_UNIT;
        break;

    case XDR_DECODE:
        pos = rstrm->in_boundry - rstrm->in_finger
            - BYTES_PER_XDR_UNIT;
        break;

    default:
        pos = (off_t) -1;
        break;
    }
    return ((u_int) pos);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool
xdr_vrec_setpos(XDR *xdrs, u_int pos)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;
    u_int currpos = xdrrec_getpos(xdrs);
    int delta = currpos - pos;
    char *newpos;

    if ((int)currpos != -1)
        switch (xdrs->x_op) {

        case XDR_ENCODE:
            newpos = rstrm->out_finger - delta;
            if ((newpos > (char *)(void *)(rstrm->frag_header)) &&
                (newpos < rstrm->out_boundry)) {
                rstrm->out_finger = newpos;
                return (TRUE);
            }
            break;

        case XDR_DECODE:
            newpos = rstrm->in_finger - delta;
            if ((delta < (int)(rstrm->fbtbc)) &&
                (newpos <= rstrm->in_boundry) &&
                (newpos >= rstrm->in_base)) {
                rstrm->in_finger = newpos;
                rstrm->fbtbc -= delta;
                return (TRUE);
            }
            break;

        case XDR_FREE:
            break;
        }
    return (FALSE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static int32_t *
xdr_vrec_inline(XDR *xdrs, u_int len)
{
    V_RECSTREAM *vrstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    int32_t *buf = NULL;

    /* ok, we keep xdrrec's inline concept intact--return
     * bytes only if they are in a buffer (but we give it
     * an extra kick on skiprecord) */

    switch (xdrs->x_op) {

    case XDR_ENCODE:
        /* XXX we certainly use this, need to be careful */
        abort();
        break;

    case XDR_DECODE:
        pos = vrec_inpos(vrstrm);
        if (pos->btbc >= len) {
            buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
            pos->btbc -= len;
            pos->boff += len;
        }
        break;

    case XDR_FREE:
        break;
    }
    return (buf);

#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;
    int32_t *buf = NULL;

    switch (xdrs->x_op) {

    case XDR_ENCODE:
        if ((rstrm->out_finger + len) <= rstrm->out_boundry) {
            buf = (int32_t *)(void *)rstrm->out_finger;
            rstrm->out_finger += len;
        }
        break;

    case XDR_DECODE:
        if ((len <= rstrm->fbtbc) &&
            ((rstrm->in_finger + len) <= rstrm->in_boundry)) {
            buf = (int32_t *)(void *)rstrm->in_finger;
            rstrm->fbtbc -= len;
            rstrm->in_finger += len;
        }
        break;

    case XDR_FREE:
        break;
    }
    return (buf);
#endif /* 0 */
}

static void
xdr_vrec_destroy(XDR *xdrs)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;

    mem_free(rstrm->out_base, rstrm->sendsize);
    mem_free(rstrm->in_base, rstrm->recvsize);
    mem_free(rstrm, sizeof(RECSTREAM));
#else
    abort();
#endif /* 0 */
}


/*
 * Exported routines to manage xdr records
 */

/*
 * Before reading (deserializing from the stream), one should always call
 * this procedure to guarantee proper record alignment.
 */
bool
xdr_vrec_skiprecord(XDR *xdrs)
{
    V_RECSTREAM *vrstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *pos;
    struct iovec iov[1];
    struct v_rec *vrec;
    uint32_t nbytes;

    if (unlikely(vrec_qlen(&vrstrm->in_q)) == 0)
        return (TRUE);

    /* XXXX fix this, if we're actually skipping a
     * record XXX !*/
    pos = &vrstrm->in_q.pos;

    /* next, line up a new buffer head on which to read
     * incoming bytes */
    vrec = vrec_get_vrec(vrstrm);
    vrec->base = vrec_alloc_buffer(8192); /* first buffer in a fragment */
    vrec->flags = VR_FLAG_RECLAIM;
    vrec->off = 0;
    vrec->len = 0;

    /* XXX improve */
    pos->vrec = vrec;
    pos->loff = 0; /* XXX */
    pos->bpos = 0;
    pos->boff = 0;
    pos->btbc = 0;

    vrec_append_rec(&vrstrm->in_q, vrec);

    /* XXX assume it's better to read into WRITE data, than force
     * lots of tiny reads (as xdrrec seems to) */
    iov->iov_base = vrec->base;
    iov->iov_len = 8192; /* LFP */
    nbytes = vrstrm->readv(vrstrm->tcp_handle, iov, 1, VREC_O_NONBLOCK);
    pos->btbc += nbytes;

#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    enum xprt_stat xstat;

    while (rstrm->fbtbc > 0 || (! rstrm->last_frag)) {
        if (! skip_input_bytes(rstrm, rstrm->fbtbc))
            return (FALSE);
        rstrm->fbtbc = 0;
        if ((! rstrm->last_frag) && (! set_input_fragment(rstrm)))
            return (FALSE);
    }
    rstrm->last_frag = FALSE;
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

/*
 * Look ahead function.
 * Returns TRUE iff there is no more input in the buffer
 * after consuming the rest of the current record.
 */
bool
xdr_vrec_eof(XDR *xdrs)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);

    while (rstrm->fbtbc > 0 || (! rstrm->last_frag)) {
        if (! skip_input_bytes(rstrm, rstrm->fbtbc))
            return (TRUE);
        rstrm->fbtbc = 0;
        if ((! rstrm->last_frag) && (! set_input_fragment(rstrm)))
            return (TRUE);
    }
    if (rstrm->in_finger == rstrm->in_boundry)
        return (TRUE);
    return (FALSE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

/*
 * The client must tell the package when an end-of-record has occurred.
 * The second paramter tells whether the record should be flushed to the
 * (output) tcp stream.  (This let's the package support batched or
 * pipelined procedure calls.)  TRUE => immmediate flush to tcp connection.
 */
bool
xdr_vrec_endofrecord(XDR *xdrs, bool sendnow)
{
#if 0 /* XXX */
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    u_long len;  /* fragment length */

    if (sendnow || rstrm->frag_sent ||
        ((u_long)rstrm->out_finger + sizeof(u_int32_t) >=
         (u_long)rstrm->out_boundry)) {
        rstrm->frag_sent = FALSE;
        return (flush_out(rstrm, TRUE));
    }
    len = (u_long)(rstrm->out_finger) - (u_long)(rstrm->frag_header) -
        sizeof(u_int32_t);
    *(rstrm->frag_header) = htonl((u_int32_t)len | LAST_FRAG);
    rstrm->frag_header = (u_int32_t *)(void *)rstrm->out_finger;
    rstrm->out_finger += sizeof(u_int32_t);
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

/*
 * Internal useful routines
 */
static bool
flush_out(V_RECSTREAM *rstrm, bool eor)
{
#if 0 /* XXX */
    u_int32_t eormask = (eor == TRUE) ? LAST_FRAG : 0;
    u_int32_t len = (u_int32_t)((u_long)(rstrm->out_finger) -
                                (u_long)(rstrm->frag_header) - sizeof(u_int32_t));

    *(rstrm->frag_header) = htonl(len | eormask);
    len = (u_int32_t)((u_long)(rstrm->out_finger) -
                      (u_long)(rstrm->out_base));
    if ((*(rstrm->writeit))(rstrm->tcp_handle, rstrm->out_base, (int)len)
        != (int)len)
        return (FALSE);
    rstrm->frag_header = (u_int32_t *)(void *)rstrm->out_base;
    rstrm->out_finger = (char *)rstrm->out_base + sizeof(u_int32_t);
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool  /* knows nothing about records!  Only about input buffers */
fill_input_buf(V_RECSTREAM *rstrm)
{
#if 0 /* XXX */
    char *where;
    u_int32_t i;
    int len;

    where = rstrm->in_base;
    i = (u_int32_t)((u_long)rstrm->in_boundry % BYTES_PER_XDR_UNIT);
    where += i;
    len = (u_int32_t)(rstrm->in_size - i);
    if ((len = (*(rstrm->readit))(rstrm->tcp_handle, where, len)) == -1)
        return (FALSE);
    rstrm->in_finger = where;
    where += len;
    rstrm->in_boundry = where;
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool  /* knows nothing about records!  Only about input buffers */
get_input_bytes(V_RECSTREAM *rstrm, char *addr, int len)
{
#if 0 /* XXX */
    size_t current;

    while (len > 0) {
        current = (size_t)((long)rstrm->in_boundry -
                           (long)rstrm->in_finger);
        if (current == 0) {
            if (! fill_input_buf(rstrm))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        memmove(addr, rstrm->in_finger, current);
        rstrm->in_finger += current;
        addr += current;
        len -= current;
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static bool  /* next two bytes of the input stream are treated as a header */
set_input_fragment(V_RECSTREAM *rstrm)
{
#if 0 /* XXX */
    u_int32_t header;

    if (! get_input_bytes(rstrm, (char *)(void *)&header, sizeof(header)))
        return (FALSE);
    header = ntohl(header);
    rstrm->last_frag = ((header & LAST_FRAG) == 0) ? FALSE : TRUE;
    /*
     * Sanity check. Try not to accept wildly incorrect
     * record sizes. Unfortunately, the only record size
     * we can positively identify as being 'wildly incorrect'
     * is zero. Ridiculously large record sizes may look wrong,
     * but we don't have any way to be certain that they aren't
     * what the client actually intended to send us.
     */
    if (header == 0)
        return(FALSE);
    rstrm->fbtbc = header & (~LAST_FRAG);
    return (TRUE);
#else
    return (FALSE);
#endif /* 0 */
}

static bool  /* consumes input bytes; knows nothing about records! */
skip_input_bytes(V_RECSTREAM *rstrm, long cnt)
{
#if 0 /* XXX */
    u_int32_t current;

    while (cnt > 0) {
        current = (size_t)((long)rstrm->in_boundry -
                           (long)rstrm->in_finger);
        if (current == 0) {
            if (! fill_input_buf(rstrm))
                return (FALSE);
            continue;
        }
        current = (u_int32_t)((cnt < current) ? cnt : current);
        rstrm->in_finger += current;
        cnt -= current;
    }
    return (TRUE);
#else
    abort();
    return (FALSE);
#endif /* 0 */
}

static u_int
fix_buf_size(u_int s)
{

    if (s < 100)
        s = 4000;
    return (RNDUP(s));
}

static bool
xdr_vrec_noop(void)
{
    return (FALSE);
}
