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


#define VR_FLAG_RECLAIM 0x0001

struct v_rec
{
    struct opr_queue q;
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

struct v_rec_queue
{
    struct opr_queue q;
    struct v_rec_pos_t fpos; /* fill position */
    struct v_rec_pos_t lpos; /* logical position, GET|SETPOS */
    int32_t size; /* count of buffer segments */
};

/* Preallocate memory */
struct vrec_prealloc
{
    struct v_rec_queue v_req;
    struct v_rec_queue v_req_buf;
};

struct v_rec_strm
{
    void *tcp_handle; /* XXX generalize? */

    struct vrec_prealloc prealloc;

    /*
     * out-going bits
     */
    size_t (*writev)(void *, struct iovec *, int, uint32_t);

    struct v_rec_queue out_q;

    char *out_base; /* output buffer (points to frag header) */
    char *out_finger; /* next output position */
    char *out_boundry; /* data cannot up to this address */
    u_int32_t *frag_header; /* beginning of current fragment */
    bool frag_sent; /* true if buffer sent in middle of record */

    /*
     * in-coming bits
     */
    size_t (*readv)(void *, struct iovec *, int, uint32_t);

    struct v_rec_queue in_q;

    u_long in_size; /* default size of allocated buffers */
#if 0
    char *in_base;
    char *in_finger; /* location of next byte to be had */
    char *in_boundry; /* can read up to this location */
#endif

    long fbtbc;  /* fragment bytes to be consumed */
    bool last_frag;

    u_int sendsize;
    u_int recvsize;

    bool nonblock;
    bool in_haveheader;
    u_int32_t in_header;
    char *in_hdrp;
    int in_hdrlen;
    int in_reclen;
    int in_received;
    int in_maxrec;
};

typedef struct v_rec_strm V_RECSTREAM;

/* i/o provider inteface */

#define VREC_NONE          0x0000
#define VREC_O_NONBLOCK    0x0001

/* vector equivalents */

#define XDR_VREC_INREC     0x0001
#define XDR_VREC_OUTREC    0x0002

extern void xdr_vrec_create(XDR *, u_int, u_int, void *,
                            size_t (*)(void *, struct iovec *, int, u_int),
                            size_t (*)(void *, struct iovec *, int, u_int),
                            u_int flags);

/* make end of xdr record */
extern bool xdr_vrec_endofrecord(XDR *, bool);

/* move to beginning of next record */
extern bool xdr_vrec_skiprecord(XDR *);

/* true if no more input */
extern bool xdr_vrec_eof(XDR *);
