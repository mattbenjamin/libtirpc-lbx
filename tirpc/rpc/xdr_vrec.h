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
    struct opr_queue q;
    void *base;
    struct v_rec *alt;
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

/* XXX deleteme */
#define VQSIZE 64
struct v_q
{
    struct v_rec *vreqs[VQSIZE]; /* array of pointers to v_reqs */
    int len; /* slots in use */
};

struct opr_queue_ex
{
    struct opr_queue q;
    int32_t size;
};

/* Preallocate memory */
struct vrec_prealloc
{
    struct opr_queue_ex v_req;
    struct opr_queue_ex v_req_buf;
};

struct v_rec_strm
{
    void *tcp_handle; /* XXX generalize? */

    struct vrec_prealloc prealloc;

    /*
     * out-going bits
     */
    size_t (*writev)(void *, struct iovec *, int);

    struct opr_queue out_q;

    char *out_base; /* output buffer (points to frag header) */
    char *out_finger; /* next output position */
    char *out_boundry; /* data cannot up to this address */
    u_int32_t *frag_header; /* beginning of current fragment */
    bool frag_sent; /* true if buffer sent in middle of record */

    /*
     * in-coming bits
     */
    size_t (*readv)(void *, struct iovec *, int);

    struct opr_queue in_q;

    u_long in_size; /* fixed size of the input buffer */
    char *in_base;
    char *in_finger; /* location of next byte to be had */
    char *in_boundry; /* can read up to this location */
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
