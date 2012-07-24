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
#include <assert.h>

#include "intrinsic.h"
#include "rpc_com.h"

#include <rpc/xdr_vrec.h>

static bool xdr_vrec_getlong(XDR *, long *);
static bool xdr_vrec_putlong(XDR *, const long *);
static bool xdr_vrec_getbytes(XDR *, char *, u_int);
static bool xdr_vrec_putbytes(XDR *, const char *, u_int);
static bool xdr_vrec_getbufs(XDR *, xdr_uio *, u_int, u_int);
static bool xdr_vrec_putbufs(XDR *, xdr_uio *, u_int, u_int);
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
    xdr_vrec_getbufs,
    xdr_vrec_putbufs
};

#define LAST_FRAG ((u_int32_t)(1 << 31))

static bool vrec_flush_out(V_RECSTREAM *, bool);
static bool vrec_get_input_fragment_bytes(V_RECSTREAM *, char **, int);
static bool vrec_set_input_fragment(V_RECSTREAM *);
static bool vrec_skip_input_bytes(V_RECSTREAM *, long);
static bool vrec_get_input_segments(V_RECSTREAM *, int);

/* XXX might not be faster than jemalloc, &c? */
static inline void
init_prealloc_queues(V_RECSTREAM *vstrm)
{
    int ix;
    struct v_rec *vrec;

    TAILQ_INIT(&vstrm->prealloc.v_req.q);
    vstrm->prealloc.v_req.size = 0;

    TAILQ_INIT(&vstrm->prealloc.v_req_buf.q);
    vstrm->prealloc.v_req_buf.size = 0;

    for (ix = 0; ix < VQSIZE; ++ix) {
        vrec = mem_alloc(sizeof(struct v_rec));
        TAILQ_INSERT_TAIL(&vstrm->prealloc.v_req.q, vrec, ioq);
        (vstrm->prealloc.v_req.size)++;
    }
}

static inline void
vrec_init_queue(struct v_rec_queue *q)
{
    TAILQ_INIT(&q->q);
    q->size = 0;
}

static inline struct v_rec *
vrec_get_vrec(V_RECSTREAM *vstrm)
{
    struct v_rec *vrec;
    if (unlikely(vstrm->prealloc.v_req.size == 0)) {
        vrec = mem_alloc(sizeof(struct v_rec));
    } else {
        vrec = TAILQ_FIRST(&vstrm->prealloc.v_req.q);
        TAILQ_REMOVE(&vstrm->prealloc.v_req.q, vrec, ioq);
        (vstrm->prealloc.v_req.size)--;
    }
    return (vrec);
}

static inline void
vrec_put_vrec(V_RECSTREAM *vstrm, struct v_rec *vrec)
{
    if (unlikely(vstrm->prealloc.v_req.size > VQSIZE))
        mem_free(vrec, sizeof(struct v_rec));
    else {
        TAILQ_INSERT_TAIL(&vstrm->prealloc.v_req.q, vrec, ioq);
        (vstrm->prealloc.v_req.size)++;
    }
}

#if 0 /* jemalloc docs warn about reclaim */
#define vrec_alloc_buffer(size) mem_alloc_aligned(0x8, (size))
#else
#define vrec_alloc_buffer(size) mem_alloc((size))
#endif /* 0 */
#define vrec_free_buffer(addr) mem_free((addr), 0)

#define VREC_NSINK 1
#define VREC_NFILL 6

static inline void
init_discard_buffers(V_RECSTREAM *vstrm)
{
    int ix;
    struct iovec *iov;

    for (ix = 0; ix < VREC_NSINK; ix++) {
        iov = &(vstrm->st_u.in.iovsink[ix]);
        iov->iov_base = vrec_alloc_buffer(vstrm->def_bsize);
        iov->iov_len = 0;
    }
}

static inline void
free_discard_buffers(V_RECSTREAM *vstrm)
{
    int ix;
    struct iovec *iov;

    for (ix = 0; ix < VREC_NSINK; ix++) {
        iov = &(vstrm->st_u.in.iovsink[ix]);
        vrec_free_buffer(iov->iov_base);
    }
}

#define vrec_qlen(q) ((q)->size)
#define vrec_fpos(vstrm) (&vstrm->ioq.fpos)
#define vrec_lpos(vstrm) (&vstrm->ioq.lpos)


static inline void vrec_append_rec(struct v_rec_queue *q, struct v_rec *vrec)
{
    TAILQ_INSERT_TAIL(&q->q, vrec, ioq);
    (q->size)++;
}

static inline void
vrec_init_ioq(V_RECSTREAM *vstrm)
{
    struct v_rec *vrec = vrec_get_vrec(vstrm);
    vrec->refcnt = 0;
    vrec->size = vstrm->def_bsize;
    vrec->base = vrec_alloc_buffer(vrec->size);
    vrec->off = 0;
    vrec->len = 0;
    vrec->flags = VREC_FLAG_RECLAIM;
    vrec_append_rec(&vstrm->ioq, vrec);
    (vstrm->ioq.size)++;
}

static inline void
vrec_rele(V_RECSTREAM *vstrm, struct v_rec *vrec)
{
    (vrec->refcnt)--;
    if (unlikely(vrec->refcnt == 0)) {
        if (vrec->flags & VREC_FLAG_RECLAIM) {
            vrec_free_buffer(vrec->base);
        }
        /* return to freelist */
        vrec_put_vrec(vstrm, vrec);
    }
}

static inline size_t
vrec_readahead_bytes(V_RECSTREAM *vstrm, int len, u_int flags)
{
    struct v_rec_pos_t *pos;
    struct iovec iov[1];
    uint32_t nbytes;

    pos = vrec_fpos(vstrm);
    iov->iov_base = pos->vrec->base + pos->vrec->off;
    iov->iov_len = len;
    nbytes = vstrm->ops.readv(vstrm->vp_handle, iov, 1, flags);
    vstrm->st_u.in.rbtbc += nbytes;
    pos->vrec->len += nbytes;

    return (nbytes);
}

#define vrec_nb_readahead(vstrm) \
    vrec_readahead_bytes((vstrm), (vstrm)->st_u.in.readahead_bytes, \
                         VREC_FLAG_NONBLOCK);

enum vrec_cursor
{
    VREC_FPOS,
    VREC_LPOS,
    VREC_RESET_POS
};

/*
 * Set initial read/insert or fill position.
 */
static inline void
vrec_stream_reset(V_RECSTREAM *vstrm, enum vrec_cursor wh_pos)
{
    struct v_rec_pos_t *pos;
    struct v_rec *vrec;

    switch (wh_pos) {
    case VREC_FPOS:
        pos = vrec_fpos(vstrm);
        break;
    case VREC_LPOS:
        pos = vrec_lpos(vstrm);
        break;
    case VREC_RESET_POS:
        vrec_stream_reset(vstrm, VREC_FPOS);
        vrec_stream_reset(vstrm, VREC_LPOS);
        return;
        break;
    default:
        abort();
        break;
    }

    vrec = TAILQ_FIRST(&vstrm->ioq.q);
    pos->vrec = vrec;
    pos->loff = 0;
    pos->bpos = 0; /* first position */
    pos->boff = 0;
}

static inline void
vrec_truncate_input_q(V_RECSTREAM *vstrm, int max)
{
    struct v_rec *vrec;

    /* the ioq queue can contain shared and special segments (eg, mapped
     * buffers).  if present, these segements will also be threaded on
     * a release (sub) queue.
     *
     * we are truncating a recv queue.  if any special segments are
     * present, we must detach them.
     */
    while (unlikely(! TAILQ_EMPTY(&vstrm->relq.q))) {
        vrec = TAILQ_FIRST(&vstrm->relq.q);
        /* dequeue segment */
        TAILQ_REMOVE(&vstrm->ioq.q, vrec, ioq);
        (vstrm->ioq.size)--;
        /* decref */
        (vrec->refcnt)--;
        /* vrec should be ref'd elsewhere (it was special) */
        if (unlikely(vrec->refcnt == 0)) {
            TAILQ_REMOVE(&vstrm->relq.q, vrec, relq);
            (vstrm->relq.size)--;
            if (unlikely(vrec->flags & VREC_FLAG_RECLAIM)) {
                vrec_free_buffer(vrec->base);
            }
            /* recycle it */
            vrec_put_vrec(vstrm, vrec);
        }
    }

    /* any segment left in ioq is a network buffer.  enforce upper
     * bound on ioq size.
     */
    while (unlikely(vstrm->ioq.size > max)) {
        vrec = TAILQ_LAST(&vstrm->ioq.q, vrq_tailq);
        TAILQ_REMOVE(&vstrm->ioq.q, vrec, ioq);
        (vstrm->ioq.size)--;
        /* almost certainly recycles */
        vrec_rele(vstrm, vrec);
    }

    /* ideally, the first read on the stream */
    if (unlikely(vstrm->ioq.size == 0))
        vrec_init_ioq(vstrm);

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        break;
    case XDR_VREC_OUT:
        vstrm->st_u.out.frag_len = 0;
        break;
    default:
        abort();
        break;
    }

    /* stream reset */
    vrec_stream_reset(vstrm, VREC_RESET_POS);

    return;
}

#define vrec_truncate_output_q vrec_truncate_input_q

/*
 * Advance read/insert or fill position.
 */
static inline bool
vrec_next(V_RECSTREAM *vstrm, enum vrec_cursor wh_pos, u_int flags)
{
    struct v_rec_pos_t *pos;
    struct v_rec *vrec;

    switch (wh_pos) {
    case VREC_FPOS:
        pos = vrec_fpos(vstrm);
        /* re-use following buffers */
        vrec = TAILQ_NEXT(vrec, ioq);
        /* append new buffers, iif requested */
        if (likely(! vrec) && (flags & VREC_FLAG_XTENDQ)) {
            vrec  = vrec_get_vrec(vstrm);
            vrec->size = vstrm->def_bsize;
            vrec->base = vrec_alloc_buffer(vrec->size);
            vrec->flags = VREC_FLAG_RECLAIM;
            vrec_append_rec(&vstrm->ioq, vrec);
            (vstrm->ioq.size)++;
        }
        vrec->refcnt = 0;
        vrec->off = 0;
        vrec->len = 0;
        pos->vrec = vrec;
        (pos->bpos)++;
        pos->boff = 0;
        /* pos->loff is unchanged */
        *(vrec_lpos(vstrm)) = *pos;
        break;
    case VREC_LPOS:
        pos = vrec_lpos(vstrm);
        vrec = TAILQ_NEXT(vrec, ioq);
        if (unlikely(! vrec))
            return (FALSE);
        pos->vrec = vrec;
        (pos->bpos)++;
        pos->boff = 0;
        break;
    default:
        abort();
        break;
    }
    return (TRUE);
}

/*
 * Create an xdr handle
 */
void
xdr_vrec_create(XDR *xdrs,
                enum xdr_vrec_direction direction, void *xhandle,
                size_t (*xreadv)(void *, struct iovec *, int, u_int),
                size_t (*xwritev)(void *, struct iovec *, int, u_int),
                u_int def_bsize, u_int flags)
{
    V_RECSTREAM *vstrm = mem_alloc(sizeof(V_RECSTREAM));

    if (vstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_vrec_create: out of memory");
        return;
    }

    xdrs->x_ops = &xdr_vrec_ops;
    xdrs->x_private = vstrm;

    vstrm->direction = direction;
    vstrm->vp_handle = xhandle;

    /* init queues */
    vrec_init_queue(&vstrm->ioq);
    vrec_init_queue(&vstrm->relq);

    /* buffer tuning */
    vstrm->def_bsize = def_bsize;

    switch (direction) {
    case XDR_VREC_IN:
        vstrm->ops.readv = xreadv;
        vstrm->st_u.in.readahead_bytes = 1200; /* XXX PMTU? */
        vstrm->st_u.in.fbtbc = 0;
        vstrm->st_u.in.rbtbc = 0;
        vstrm->st_u.in.haveheader = FALSE;
        vstrm->st_u.in.last_frag = TRUE;
        break;
    case XDR_VREC_OUT:
        vstrm->ops.writev = xwritev;
        vrec_init_ioq(vstrm);
        vrec_truncate_output_q(vstrm, 8);

        /* XXX finish */
        vstrm->st_u.out.frag_sent = FALSE;
        break;
    default:
        abort();
        break;
    }

    init_prealloc_queues(vstrm);
    init_discard_buffers(vstrm);

    return;
}

/*
 * The routines defined below are the xdr ops which will go into the
 * xdr handle filled in by xdr_vrec_create.
 */
static bool
xdr_vrec_getlong(XDR *xdrs,  long *lp)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    int32_t *buf = NULL;

    /* first try the inline, fast case */
    if (vstrm->st_u.in.fbtbc >= sizeof(int32_t)) {
        struct v_rec_pos_t *pos = vrec_lpos(vstrm);
        buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
        *lp = (long)ntohl(*buf);
    } else {
        if (! xdr_vrec_getbytes(
                xdrs, (char *)(void *)buf, sizeof(int32_t)))
            return (FALSE);
        *lp = (long)ntohl(*buf);
    }
    return (TRUE);
}

static bool
xdr_vrec_putlong(XDR *xdrs, const long *lp)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;

    pos = vrec_fpos(vstrm);
    if (unlikely((pos->vrec->size - pos->vrec->len)
                 < sizeof(int32_t))) {
        /* advance fill pointer */
        if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ))
            return (FALSE);
    }

    *((int32_t *)(pos->vrec->base + pos->vrec->off)) =
        (int32_t)htonl((u_int32_t)(*lp));

    pos->vrec->off += sizeof(int32_t);
    pos->boff += sizeof(int32_t);
    vstrm->st_u.out.frag_len += sizeof(int32_t);

    return (TRUE);
}

static bool
xdr_vrec_getbytes(XDR *xdrs, char *addr, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    u_long cbtbc;

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
            pos = vrec_lpos(vstrm);
        restart:
            /* CASE 1:  re-consuming bytes in a stream (after SETPOS/rewind) */
            while (pos->loff < vstrm->st_u.in.buflen) {
                while (len > 0) {
                    u_int delta = MIN(len, (pos->vrec->len - pos->boff));
                    if (unlikely(! delta)) {
                        if (! vrec_next(vstrm, VREC_LPOS, VREC_FLAG_NONE))
                            return (FALSE);
                        goto restart;
                    }
                    memcpy(addr, (pos->vrec->base + pos->boff), delta);
                    pos->loff += delta;
                    pos->boff += delta;
                    len -= delta;
                }
            }
            /* CASE 2: reading into the stream */
            while (len > 0) {
                cbtbc = vstrm->st_u.in.fbtbc;
                if (unlikely(! cbtbc)) {
                    if (vstrm->st_u.in.last_frag)
                        return (FALSE);
                    if (! vrec_set_input_fragment(vstrm))
                        return (FALSE);
                    continue;
                }
                cbtbc = (len < cbtbc) ? len : cbtbc;
                if (! vrec_get_input_segments(vstrm, cbtbc))
                    return (FALSE);
                /* now we have CASE 1 */
                goto restart;
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            /* TODO: implement */
            break;
        case XDR_DECODE:
        default:
            abort();
            break;
        }
        break;
    default:
        abort();
        break;
    }

    /* assert(len == 0); */
    return (TRUE);
}

static bool
xdr_vrec_putbytes(XDR *xdrs, const char *addr, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos;
    int delta;

    while (len > 0) {
        pos = vrec_fpos(vstrm);
        delta = pos->vrec->size - pos->vrec->len;
        if (unlikely(! delta)) {
            /* advance fill pointer */
            if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ))
                return (FALSE);
        }
        /* in glibc 2.14+ x86_64, memcpy no longer tries to handle
         * overlapping areas, see Fedora Bug 691336 (NOTABUG) */
        memcpy((pos->vrec->base + pos->vrec->off), addr, delta);
        pos->vrec->off += delta;
        pos->boff += delta;
        vstrm->st_u.out.frag_len += delta;
        len -= delta;
    }

    return (TRUE);
}

static bool
xdr_vrec_getbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
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
xdr_vrec_putbufs(XDR *xdrs, xdr_uio *uio, u_int len, u_int flags)
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
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *lpos = vrec_lpos(vstrm);

    return (lpos->loff);
}

static bool
xdr_vrec_setpos(XDR *xdrs, u_int pos)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);
    struct v_rec_pos_t *lpos = vrec_lpos(vstrm);
    struct v_rec *vrec;
    u_int resid = 0;
    int ix;

    ix = 0;
    TAILQ_FOREACH(vrec, &(vstrm->ioq.q), ioq) {
        if ((vrec->len + resid) > pos) {
            lpos->vrec = vrec;
            lpos->bpos = ix;
            lpos->loff = pos;
            lpos->boff = (pos-resid);
            return (TRUE);
        }
        resid += vrec->len;
        ++ix;
    }
    return (FALSE);
}

static int32_t *
xdr_vrec_inline(XDR *xdrs, u_int len)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec_pos_t *pos = vrec_lpos(vstrm);;
    int32_t *buf = NULL;

    /* we keep xdrrec's inline concept mostly intact.  the function
     * returns the address of the current logical offset in the stream
     * buffer, iff no less than len contiguous bytes are available at
     * the current logical offset in the stream. */

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            abort();
            break;
        case XDR_DECODE:
            if (vstrm->st_u.in.fbtbc >= len) {
                if ((pos->boff + len) <= pos->vrec->size) {
                    buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                    vstrm->st_u.in.fbtbc -= len;
                    pos->boff += len;
                }
            }
            break;
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
        switch (xdrs->x_op) {
        case XDR_ENCODE:
            if ((pos->boff + len) <= pos->vrec->size) {
                buf = (int32_t *)(void *)(pos->vrec->base + pos->boff);
                pos->boff += len;
            }
            break;
        case XDR_DECODE:
        default:
            abort();
            break;
        }
        break;
    default:
        abort();
        break;
    }

    return (buf);
}

static void
xdr_vrec_destroy(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)xdrs->x_private;
    struct v_rec *vrec;

    /* release queued buffers */
    while (vstrm->ioq.size > 0) {
        vrec = TAILQ_FIRST(&vstrm->ioq.q);
        vrec_rele(vstrm, vrec);
        TAILQ_REMOVE(&vstrm->ioq.q, vrec, ioq);
        (vstrm->ioq.size)--;
    }
    free_discard_buffers(vstrm);
    mem_free(vstrm, sizeof(V_RECSTREAM));
}

static bool
xdr_vrec_noop(void)
{
    return (FALSE);
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
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_DECODE:
            /* stream reset */
            while (vstrm->st_u.in.fbtbc > 0 ||
                   (! vstrm->st_u.in.last_frag)) {
                if (! vrec_skip_input_bytes(vstrm, vstrm->st_u.in.fbtbc))
                    return (FALSE);
                if ((! vstrm->st_u.in.last_frag) &&
                    (! vrec_set_input_fragment(vstrm)))
                    return (FALSE);
            }
            /* bound queue size and support future mapped reads */
            vrec_truncate_input_q(vstrm, 8);
            vstrm->st_u.in.fbtbc = 0;
            vstrm->st_u.in.rbtbc = 0;
            if (! vstrm->st_u.in.haveheader) {
                if (! vrec_set_input_fragment(vstrm))
                    return (FALSE);
            }
            /* reset logical stream position */
            *(vrec_lpos(vstrm)) = *(vrec_fpos(vstrm));
            break;
        case XDR_ENCODE:
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
    default:
        abort();
        break;
    }

    return (TRUE);
}

/*
 * Look ahead function.
 * Returns TRUE iff there is no more input in the buffer
 * after consuming the rest of the current record.
 */
bool
xdr_vrec_eof(XDR *xdrs)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    switch (vstrm->direction) {
    case XDR_VREC_IN:
        switch (xdrs->x_op) {
        case XDR_DECODE:
            /* stream reset */
            while (vstrm->st_u.in.fbtbc > 0 ||
                   (! vstrm->st_u.in.last_frag)) {
                if (! vrec_skip_input_bytes(vstrm, vstrm->st_u.in.fbtbc))
                    return (TRUE);
                if ((! vstrm->st_u.in.last_frag) &&
                    (! vrec_set_input_fragment(vstrm)))
                    return (TRUE);
            }
            break;
        case XDR_ENCODE:
        default:
            abort();
            break;
        }
        break;
    case XDR_VREC_OUT:
    default:
        abort();
        break;
    }
    return (FALSE);
}

/*
 * The client must tell the package when an end-of-record has occurred.
 * The second paramter tells whether the record should be flushed to the
 * (output) tcp stream.  (This let's the package support batched or
 * pipelined procedure calls.)  TRUE => immmediate flush to tcp connection.
 */
bool
xdr_vrec_endofrecord(XDR *xdrs, bool flush)
{
    V_RECSTREAM *vstrm = (V_RECSTREAM *)(xdrs->x_private);

    /* flush, resetting stream (sends LAST_FRAG) */
    if (flush || vstrm->st_u.out.frag_sent) {
        vstrm->st_u.out.frag_sent = FALSE;
        return (vrec_flush_out(vstrm, TRUE));
    }

    /* XXX check */
    vstrm->st_u.out.frag_header = 
        htonl((u_int32_t)(vstrm->st_u.out.frag_len | LAST_FRAG));

    /* advance fill pointer */
    if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ))
        return (FALSE);

    return (TRUE);
}

/*
 * Internal useful routines
 */

#define VREC_NIOVS 8

static inline void
vrec_flush_segments(V_RECSTREAM *vstrm, struct iovec *iov, int iovcnt,
                    u_int resid)
{
    uint32_t nbytes = 0;
    struct iovec *tiov;
    int ix;

    while (resid > 0) {
        /* advance iov */
        for (ix = 0; ((nbytes > 0) && (ix < iovcnt)); ++ix) {
            tiov = iov+ix;
            if (tiov->iov_len > nbytes) {
                tiov->iov_base += nbytes;
                tiov->iov_len -= nbytes;
            } else {
                nbytes -= tiov->iov_len;
                iovcnt--;
                continue;
            }
        }
        /* blocking write */
        nbytes = vstrm->ops.writev(
            vstrm->vp_handle, tiov, iovcnt, VREC_FLAG_NONE);
        resid -= nbytes;
    }
}

static bool
vrec_flush_out(V_RECSTREAM *vstrm, bool eor)
{
    u_int32_t eormask = (eor == TRUE) ? LAST_FRAG : 0;
    struct iovec iov[VREC_NIOVS];
    struct v_rec *vrec;
    u_int resid;
    int ix;

    /* update fragment header */
    vstrm->st_u.out.frag_header =
        htonl((u_int32_t)(vstrm->st_u.out.frag_len | eormask));

    iov[0].iov_base = &(vstrm->st_u.out.frag_header);
    iov[0].iov_len = sizeof(u_int32_t);

    ix = 1;
    resid = sizeof(u_int32_t);
    TAILQ_FOREACH(vrec, &(vstrm->ioq.q), ioq) {
        iov[ix].iov_base = vrec->base;
        iov[ix].iov_len = vrec->len;
        resid += vrec->len;
        if (unlikely((vrec == TAILQ_LAST(&(vstrm->ioq.q), vrq_tailq)) ||
                (ix == (VREC_NIOVS-1)))) {
            vrec_flush_segments(vstrm, iov, ix+1 /* iovcnt */, resid);
            resid = 0;
            ix = 0;
            continue;
        }
        ++ix;
    }
    vrec_truncate_output_q(vstrm, 8);
    return (TRUE);
}

/* Stream read operation intended for small, e.g., header-length
 * reads ONLY.  Note:  len is required to be less than the queue
 * default buffer size. */
static bool
vrec_get_input_fragment_bytes(V_RECSTREAM *vstrm, char **addr, int len)
{
    struct v_rec_pos_t *pos;

    if (! vstrm->st_u.in.rbtbc) {
        vrec_readahead_bytes(vstrm, vstrm->st_u.in.readahead_bytes,
                             VREC_FLAG_NONE);
        if (len <= vstrm->st_u.in.rbtbc) {
            pos = vrec_fpos(vstrm);
            *addr = (void *)(pos->vrec->base + pos->boff);
            pos->boff += len;
            return (TRUE);
        }
    }
    return (FALSE);
}

static bool  /* next two bytes of the input stream are treated as a header */
vrec_set_input_fragment(V_RECSTREAM *vstrm)
{
    u_int32_t header;
    char *addr;

    if (! vrec_get_input_fragment_bytes(vstrm, &addr, sizeof(header)))
        return (FALSE);
    header = ntohl(*((u_int32_t *)addr));
    vstrm->st_u.in.last_frag = ((header & LAST_FRAG) == 0) ? FALSE : TRUE;
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
    vstrm->st_u.in.fbtbc = header & (~LAST_FRAG);

    return (TRUE);
}

/* Consume and discard cnt bytes from the input stream. */
static bool
vrec_skip_input_bytes(V_RECSTREAM *vstrm, long cnt)
{
    int ix;
    u_int32_t nbytes, resid;
    struct iovec *iov;

    while (cnt > 0) {
        for (ix = 0, resid = cnt; ((resid > 0) && (ix < VREC_NSINK)); ++ix) {
            iov = &(vstrm->st_u.in.iovsink[ix]);
            iov->iov_len = MIN(resid, vstrm->def_bsize);
            resid -= iov->iov_len;
        }
        nbytes = vstrm->ops.readv(vstrm->vp_handle,
                                  (struct iovec *) &(vstrm->st_u.in.iovsink),
                                  ix+1 /* iovcnt */,
                                  VREC_FLAG_NONE);
        cnt -= nbytes;
    }
    return (TRUE);
}

/* Read input bytes from the stream, segment-wise. */
static bool vrec_get_input_segments(V_RECSTREAM *vstrm, int cnt)
{
    struct v_rec_pos_t *pos = vrec_fpos(vstrm);
    struct v_rec *vrecs[VREC_NFILL];
    struct iovec iov[VREC_NFILL];
    u_int32_t resid;
    int ix;

    resid = cnt;
    for (ix = 0; ((ix < VREC_NFILL) && (resid > 0)); ++ix) {
        /* XXX */
        if (! vrec_next(vstrm, VREC_FPOS, VREC_FLAG_XTENDQ))
            return (FALSE);
        vrecs[ix] = pos->vrec;
        iov[ix].iov_base = vrecs[ix]->base;
        iov[ix].iov_len = MIN(vrecs[ix]->size, resid);
        resid -= iov[ix].iov_len;
    }
    resid = vstrm->ops.readv(vstrm->vp_handle, iov, ix, VREC_FLAG_NONE);
    /* XXX callers will re-try if we get a short read */
    for (ix = 0; ((ix < VREC_NFILL) && (resid > 0)); ++ix) {
        vstrm->st_u.in.buflen += iov[ix].iov_len;
        vrecs[ix]->len = iov[ix].iov_len;
        resid -= iov[ix].iov_len;
    }
    pos->vrec = vrecs[ix];
    pos->loff = vstrm->st_u.in.buflen - vrecs[ix]->len;
    pos->bpos += ix;
    pos->boff = 0;
    return (TRUE);
}
