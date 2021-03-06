/* Copyright 2004 Justin Erenkrantz and Greg Stein
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include <apr_general.h>  /* for strcasecmp() */

#include "serf.h"
#include "serf_bucket_util.h"


typedef struct header_list {
    const char *header;
    const char *value;

    apr_size_t header_size;
    apr_size_t value_size;

    int alloc_flags;
#define ALLOC_HEADER 0x0001  /* header lives in our allocator */
#define ALLOC_VALUE  0x0002  /* value lives in our allocator */

    struct header_list *next;
} header_list_t;

typedef struct {
    header_list_t *list;

    header_list_t *cur_read;
    enum {
        READ_START,     /* haven't started reading yet */
        READ_HEADER,    /* reading cur_read->header */
        READ_SEP,       /* reading ": " */
        READ_VALUE,     /* reading cur_read->value */
        READ_CRLF,      /* reading "\r\n" */
        READ_TERM,      /* reading the final "\r\n" */
        READ_DONE       /* no more data to read */
    } state;
    apr_size_t amt_read; /* how much of the current state we've read */

} headers_context_t;


SERF_DECLARE(serf_bucket_t *) serf_bucket_headers_create(
    serf_bucket_alloc_t *allocator)
{
    headers_context_t *ctx;

    ctx = serf_bucket_mem_alloc(allocator, sizeof(*ctx));
    ctx->list = NULL;
    ctx->state = READ_START;

    return serf_bucket_create(&serf_bucket_type_headers, allocator, ctx);
}

SERF_DECLARE(void) serf_bucket_headers_setx(
    serf_bucket_t *bkt,
    const char *header, apr_size_t header_size, int header_copy,
    const char *value, apr_size_t value_size, int value_copy)
{
    headers_context_t *ctx = bkt->data;
    header_list_t *found = ctx->list;
    header_list_t *hdr;

    /* Check to see if this header is already present. */
    while (found) {
      if (strncasecmp(found->header, header, header_size) == 0)
            break;
      found = found->next;
    }

    if (found) {

        /* The header is already present.  RFC 2616, section 4.2
           indicates that we should append the new value, separated by
           a comma.  Reasoning: for headers whose values are known to
           be comma-separated, that is clearly the correct behavior;
           for others, the correct behavior is undefined anyway. */

        /* The "+1" is for the comma; serf_bstrmemdup() will also add
           one slot for the terminating '\0'. */
        apr_size_t new_size = found->value_size + value_size + 1;
        char *new_val = serf_bucket_mem_alloc(bkt->allocator, new_size);
        memcpy(new_val, found->value, found->value_size);
        new_val[found->value_size] = ',';
        memcpy(new_val + found->value_size + 1, value, value_size);
        new_val[new_size] = '\0';
        found->value = new_val;
        found->value_size = new_size;
        found->alloc_flags |= ALLOC_VALUE;
        return;
    }

    /* Else the header is not already present.  Add it to the bucket. */

#if 0
    /* ### include this? */
    if (ctx->cur_read) {
        /* we started reading. can't change now. */
        abort();
    }
#endif

    hdr = serf_bucket_mem_alloc(bkt->allocator, sizeof(*hdr));
    hdr->header_size = header_size;
    hdr->value_size = value_size;
    hdr->alloc_flags = 0;

    if (header_copy) {
        hdr->header = serf_bstrmemdup(bkt->allocator, header, header_size);
        hdr->alloc_flags |= ALLOC_HEADER;
    }
    else {
        hdr->header = header;
    }

    if (value_copy) {
        hdr->value = serf_bstrmemdup(bkt->allocator, value, value_size);
        hdr->alloc_flags |= ALLOC_VALUE;
    }
    else {
        hdr->value = value;
    }

    hdr->next = ctx->list;
    ctx->list = hdr;
}

SERF_DECLARE(void) serf_bucket_headers_set(
    serf_bucket_t *headers_bucket,
    const char *header,
    const char *value)
{
    serf_bucket_headers_setx(headers_bucket,
                             header, strlen(header), 0,
                             value, strlen(value), 1);
}

SERF_DECLARE(void) serf_bucket_headers_setc(
    serf_bucket_t *headers_bucket,
    const char *header,
    const char *value)
{
    serf_bucket_headers_setx(headers_bucket,
                             header, strlen(header), 1,
                             value, strlen(value), 1);
}

SERF_DECLARE(void) serf_bucket_headers_setn(
    serf_bucket_t *headers_bucket,
    const char *header,
    const char *value)
{
    serf_bucket_headers_setx(headers_bucket,
                             header, strlen(header), 0,
                             value, strlen(value), 0);
}

SERF_DECLARE(const char *) serf_bucket_headers_get(
    serf_bucket_t *headers_bucket,
    const char *header)
{
    headers_context_t *ctx = headers_bucket->data;
    header_list_t *scan = ctx->list;

    while (scan) {
        if (strcasecmp(scan->header, header) == 0)
            return scan->value;
        scan = scan->next;
    }

    return NULL;
}

SERF_DECLARE(void) serf_bucket_headers_do(
    serf_bucket_t *headers_bucket,
    serf_bucket_headers_do_callback_fn_t func,
    void *baton)
{
    headers_context_t *ctx = headers_bucket->data;
    header_list_t *scan = ctx->list;

    while (scan) {
        if (func(baton, scan->header, scan->value) != 0) {
            break;
        }
        scan = scan->next;
    }
}

static void serf_headers_destroy_and_data(serf_bucket_t *bucket)
{
    headers_context_t *ctx = bucket->data;
    header_list_t *scan = ctx->list;

    while (scan) {
        header_list_t *next_hdr = scan->next;

        if (scan->alloc_flags & ALLOC_HEADER)
            serf_bucket_mem_free(bucket->allocator, (void *)scan->header);
        if (scan->alloc_flags & ALLOC_VALUE)
            serf_bucket_mem_free(bucket->allocator, (void *)scan->value);
        serf_bucket_mem_free(bucket->allocator, scan);

        scan = next_hdr;
    }

    serf_default_destroy_and_data(bucket);
}

static void select_value(
    headers_context_t *ctx,
    const char **value,
    apr_size_t *len)
{
    const char *v;
    apr_size_t l;

    if (ctx->state == READ_START) {
        if (ctx->list == NULL) {
            /* No headers. Move straight to the TERM state. */
            ctx->state = READ_TERM;
        }
        else {
            ctx->state = READ_HEADER;
            ctx->cur_read = ctx->list;
        }
        ctx->amt_read = 0;
    }

    switch (ctx->state) {
    case READ_HEADER:
        v = ctx->cur_read->header;
        l = ctx->cur_read->header_size;
        break;
    case READ_SEP:
        v = ": ";
        l = 2;
        break;
    case READ_VALUE:
        v = ctx->cur_read->value;
        l = ctx->cur_read->value_size;
        break;
    case READ_CRLF:
    case READ_TERM:
        v = "\r\n";
        l = 2;
        break;
    case READ_DONE:
        *len = 0;
        return;
    default:
        /* Not reachable */
        return;
    }

    *value = v + ctx->amt_read;
    *len = l - ctx->amt_read;
}

/* the current data chunk has been read/consumed. move our internal state. */
static apr_status_t consume_chunk(headers_context_t *ctx)
{
    /* move to the next state, resetting the amount read. */
    ++ctx->state;
    ctx->amt_read = 0;

    /* just sent the terminator and moved to DONE. signal completion. */
    if (ctx->state == READ_DONE)
        return APR_EOF;

    /* end of this header. move to the next one. */
    if (ctx->state == READ_TERM) {
        ctx->cur_read = ctx->cur_read->next;
        if (ctx->cur_read != NULL) {
            /* We've got another head to send. Reset the read state. */
            ctx->state = READ_HEADER;
        }
        /* else leave in READ_TERM */
    }

    /* there is more data which can be read immediately. */
    return APR_SUCCESS;
}

static apr_status_t serf_headers_peek(serf_bucket_t *bucket,
                                      const char **data,
                                      apr_size_t *len)
{
    headers_context_t *ctx = bucket->data;

    select_value(ctx, data, len);

    /* already done or returning the CRLF terminator? return EOF */
    if (ctx->state == READ_DONE || ctx->state == READ_TERM)
        return APR_EOF;

    return APR_SUCCESS;
}

static apr_status_t serf_headers_read(serf_bucket_t *bucket,
                                      apr_size_t requested,
                                      const char **data, apr_size_t *len)
{
    headers_context_t *ctx = bucket->data;
    apr_size_t avail;

    select_value(ctx, data, &avail);
    if (ctx->state == READ_DONE)
        return APR_EOF;

    if (requested >= avail) {
        /* return everything from this chunk */
        *len = avail;

        /* we consumed this chunk. advance the state. */
        return consume_chunk(ctx);
    }

    /* return just the amount requested, and advance our pointer */
    *len = requested;
    ctx->amt_read += requested;

    /* there is more that can be read immediately */
    return APR_SUCCESS;
}

static apr_status_t serf_headers_readline(serf_bucket_t *bucket,
                                          int acceptable, int *found,
                                          const char **data, apr_size_t *len)
{
    headers_context_t *ctx = bucket->data;
    apr_status_t status;

    /* ### what behavior should we use here? APR_EGENERAL for now */
    if ((acceptable & SERF_NEWLINE_CRLF) == 0)
        return APR_EGENERAL;

    /* get whatever is in this chunk */
    select_value(ctx, data, len);
    if (ctx->state == READ_DONE)
        return APR_EOF;

    /* we consumed this chunk. advance the state. */
    status = consume_chunk(ctx);

    /* the type of newline found is easy... */
    *found = (ctx->state == READ_CRLF || ctx->state == READ_TERM)
        ? SERF_NEWLINE_CRLF : SERF_NEWLINE_NONE;

    return status;
}

static apr_status_t serf_headers_read_iovec(serf_bucket_t *bucket,
                                            apr_size_t requested,
                                            int vecs_size,
                                            struct iovec *vecs,
                                            int *vecs_used)
{
    apr_size_t avail = requested;
    int i;

    *vecs_used = 0;

    for (i = 0; i < vecs_size; i++) {
        const char *data;
        apr_size_t len;
        apr_status_t status;

        /* Calling read() would not be a safe opt in the general case, but it
         * is here for the header bucket as it only frees all of the header
         * keys and values when the entire bucket goes away - not on a
         * per-read() basis as is normally the case.
         */
        status = serf_headers_read(bucket, avail, &data, &len);

        if (len) {
            vecs[*vecs_used].iov_base = (char*)data;
            vecs[*vecs_used].iov_len = len;

            (*vecs_used)++;

            if (avail != SERF_READ_ALL_AVAIL) {
                avail -= len;

                /* If we reach 0, then read()'s status will suffice.  */
                if (avail == 0) {
                    return status;
                }
            }
        }

        if (status) {
            return status;
        }
    }

    return APR_SUCCESS;
}

SERF_DECLARE_DATA const serf_bucket_type_t serf_bucket_type_headers = {
    "HEADERS",
    serf_headers_read,
    serf_headers_readline,
    serf_headers_read_iovec,
    serf_default_read_for_sendfile,
    serf_default_read_bucket,
    serf_headers_peek,
    serf_headers_destroy_and_data,
    serf_default_snapshot,
    serf_default_restore_snapshot,
    serf_default_is_snapshot_set,
};
