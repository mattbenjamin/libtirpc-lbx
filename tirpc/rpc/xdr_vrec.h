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

#include <stdint.h>
#include <stdbool.h>
#include <misc/opr_queue.h>

struct v_rec
{
    struct opr_queue ioq;
    struct opr_queue relq;
    uint32_t refcnt;
    void *base;
    void *p_1; /* private data */
    void *u_1; /* user data */
    u_int off;
    u_int len;
    u_int flags;
};

#define VQBUFSZ 16384
struct v_rec_buf
{
    struct opr_queue q;
    void *buf;
};

#define VQSIZE 64

struct v_rec_pos_t
{
    struct v_rec *vrec;
    int32_t loff; /* logical byte offset (convenience?) */
    int32_t bpos; /* buffer index (offset) in the current stream */
    int32_t boff; /* byte offset in buffer bix */
};

#define VREC_QFLAG_NONE      0x0000

struct v_rec_queue
{
    struct opr_queue q;
    struct v_rec_pos_t fpos; /* fill position */
    struct v_rec_pos_t lpos; /* logical position, GET|SETPOS */
    int size; /* count of buffer segments */
    u_int flags;
};

/* preallocate memory */
struct vrec_prealloc
{
    struct v_rec_queue v_req;
    struct v_rec_queue v_req_buf;
};

/* streams are unidirectional */
enum xdr_vrec_direction {
    XDR_VREC_INREC,
    XDR_VREC_OUTREC,
};

struct v_rec_strm
{
    enum xdr_vrec_direction direction;

    /* buffer queues */
    struct v_rec_queue ioq;
    struct v_rec_queue relq;

    /* opaque provider handle */
    void *vp_handle;

    /*
     * ops
     */
    union {
        size_t (*readv)(void *, struct iovec *, int, u_int);
        size_t (*writev)(void *, struct iovec *, int, u_int);
    } ops;

    /* stream params */
    int n_bufs;
    int maxbufs;
    u_int def_bsize; /* def. size of allocated buffers */

    /* stream state */
    union {
        struct {
            long fbtbc; /* fragment bytes to be consumed */
            u_int32_t header;
            char *hdrp;
            int hdrlen;
            int received;
            bool last_frag;
            bool haveheader;
            struct iovec iovsink[4];
        } in;
        struct {
            bool frag_sent;
        } out;
    } st_u;

    /* free lists */
    struct vrec_prealloc prealloc;
};

typedef struct v_rec_strm V_RECSTREAM;

/* i/o provider inteface */

#define VREC_FLAG_NONE          0x0000
#define VREC_FLAG_NONBLOCK      0x0001
#define VREC_FLAG_RECLAIM       0x0002
#define VREC_FLAG_BUFQ          0x0004
#define VREC_FLAG_RELQ          0x0008

/* vector equivalents */

extern void xdr_vrec_create(XDR *, enum xdr_vrec_direction, void *,
                            size_t (*)(void *, struct iovec *, int, u_int),
                            size_t (*)(void *, struct iovec *, int, u_int),
                            u_int, u_int);

/* make end of xdr record */
extern bool xdr_vrec_endofrecord(XDR *, bool);

/* move to beginning of next record */
extern bool xdr_vrec_skiprecord(XDR *);

/* true if no more input */
extern bool xdr_vrec_eof(XDR *);
