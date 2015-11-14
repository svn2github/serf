/* ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "serf.h"
#include "serf_bucket_util.h"


/* Should be an APR_RING? */
typedef struct bucket_list {
    serf_bucket_t *bucket;
    struct bucket_list *next;
} bucket_list_t;

typedef struct aggregate_context_t {
    bucket_list_t *list; /* active buckets */
    bucket_list_t *last; /* last bucket of the list */
    bucket_list_t *done; /* we finished reading this; now pending a destroy */

    serf_bucket_aggregate_eof_t hold_open;
    void *hold_open_baton;

    serf_config_t *config;
} aggregate_context_t;


static void cleanup_aggregate(aggregate_context_t *ctx,
                              serf_bucket_alloc_t *allocator)
{
    bucket_list_t *next_list;

    /* If we finished reading a bucket during the previous read, then
     * we can now toss that bucket.
     */
    while (ctx->done != NULL) {
        next_list = ctx->done->next;

        serf_bucket_destroy(ctx->done->bucket);
        serf_bucket_mem_free(allocator, ctx->done);

        ctx->done = next_list;
    }
}

void serf_bucket_aggregate_cleanup(
    serf_bucket_t *bucket, serf_bucket_alloc_t *allocator)
{
    aggregate_context_t *ctx = bucket->data;

    cleanup_aggregate(ctx, allocator);
}

static aggregate_context_t *create_aggregate(serf_bucket_alloc_t *allocator)
{
    aggregate_context_t *ctx;

    ctx = serf_bucket_mem_alloc(allocator, sizeof(*ctx));

    ctx->list = NULL;
    ctx->last = NULL;
    ctx->done = NULL;
    ctx->hold_open = NULL;
    ctx->hold_open_baton = NULL;
    ctx->config = NULL;

    return ctx;
}

serf_bucket_t *serf_bucket_aggregate_create(
    serf_bucket_alloc_t *allocator)
{
    aggregate_context_t *ctx;

    ctx = create_aggregate(allocator);

    return serf_bucket_create(&serf_bucket_type_aggregate, allocator, ctx);
}


static void serf_aggregate_destroy_and_data(serf_bucket_t *bucket)
{
    aggregate_context_t *ctx = bucket->data;
    bucket_list_t *next_ctx;

    while (ctx->list) {
        serf_bucket_destroy(ctx->list->bucket);
        next_ctx = ctx->list->next;
        serf_bucket_mem_free(bucket->allocator, ctx->list);
        ctx->list = next_ctx;
    }
    cleanup_aggregate(ctx, bucket->allocator);

    serf_default_destroy_and_data(bucket);
}

void serf_bucket_aggregate_become(serf_bucket_t *bucket)
{
    aggregate_context_t *ctx;

    ctx = create_aggregate(bucket->allocator);

    bucket->type = &serf_bucket_type_aggregate;
    bucket->data = ctx;

    /* The allocator remains the same. */
}


void serf_bucket_aggregate_prepend(
    serf_bucket_t *aggregate_bucket,
    serf_bucket_t *prepend_bucket)
{
    aggregate_context_t *ctx = aggregate_bucket->data;
    bucket_list_t *new_list;

    new_list = serf_bucket_mem_alloc(aggregate_bucket->allocator,
                                     sizeof(*new_list));
    new_list->bucket = prepend_bucket;
    new_list->next = ctx->list;

    if (ctx->list == NULL)
        ctx->last = new_list;
    ctx->list = new_list;

    /* Share our config with this new bucket */
    serf_bucket_set_config(prepend_bucket, ctx->config);
}

void serf_bucket_aggregate_append(
    serf_bucket_t *aggregate_bucket,
    serf_bucket_t *append_bucket)
{
    aggregate_context_t *ctx = aggregate_bucket->data;
    bucket_list_t *new_list;

    new_list = serf_bucket_mem_alloc(aggregate_bucket->allocator,
                                     sizeof(*new_list));
    new_list->bucket = append_bucket;
    new_list->next = NULL;

    /* If we use APR_RING, this is trivial.  So, wait.
    new_list->next = ctx->list;
    ctx->list = new_list;
    */
    if (ctx->list == NULL) {
        ctx->list = new_list;
        ctx->last = new_list;
    }
    else {
        ctx->last->next = new_list;
        ctx->last = ctx->last->next;
    }

    /* Share our config with this new bucket */
    serf_bucket_set_config(append_bucket, ctx->config);
}

void serf_bucket_aggregate_hold_open(serf_bucket_t *aggregate_bucket, 
                                     serf_bucket_aggregate_eof_t fn,
                                     void *baton)
{
    aggregate_context_t *ctx = aggregate_bucket->data;
    ctx->hold_open = fn;
    ctx->hold_open_baton = baton;
}

void serf_bucket_aggregate_prepend_iovec(
    serf_bucket_t *aggregate_bucket,
    struct iovec *vecs,
    int vecs_count)
{
    int i;

    /* Add in reverse order. */
    for (i = vecs_count - 1; i >= 0; i--) {
        serf_bucket_t *new_bucket;

        new_bucket = serf_bucket_simple_create(vecs[i].iov_base,
                                               vecs[i].iov_len,
                                               NULL, NULL,
                                               aggregate_bucket->allocator);

        serf_bucket_aggregate_prepend(aggregate_bucket, new_bucket);

    }
}

void serf_bucket_aggregate_append_iovec(
    serf_bucket_t *aggregate_bucket,
    struct iovec *vecs,
    int vecs_count)
{
    /* aggregate_context_t *ctx = aggregate_bucket->data; */
    serf_bucket_t *new_bucket;

    new_bucket = serf_bucket_iovec_create(vecs, vecs_count,
                                          aggregate_bucket->allocator);

    serf_bucket_aggregate_append(aggregate_bucket, new_bucket);
}

static apr_status_t read_aggregate(serf_bucket_t *bucket,
                                   apr_size_t requested,
                                   int vecs_size, struct iovec *vecs,
                                   int *vecs_used)
{
    aggregate_context_t *ctx = bucket->data;
    int cur_vecs_used;
    apr_status_t status;

    *vecs_used = 0;

    if (!ctx->list) {
        if (ctx->hold_open) {
            return ctx->hold_open(ctx->hold_open_baton, bucket);
        }
        else {
            return APR_EOF;
        }
    }

    status = APR_SUCCESS;
    while (requested) {
        serf_bucket_t *head = ctx->list->bucket;

        status = serf_bucket_read_iovec(head, requested, vecs_size, vecs,
                                        &cur_vecs_used);

        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        /* Add the number of vecs we read to our running total. */
        *vecs_used += cur_vecs_used;

        if (cur_vecs_used > 0 || status) {
            bucket_list_t *next_list;

            /* If we got SUCCESS (w/bytes) or EAGAIN, we want to return now
             * as it isn't safe to read more without returning to our caller.
             */
            if (!status || APR_STATUS_IS_EAGAIN(status) || status == SERF_ERROR_WAIT_CONN) {
                return status;
            }

            /* However, if we read EOF, we can stash this bucket in a
             * to-be-freed list and move on to the next bucket.  This ensures
             * that the bucket stays alive (so as not to violate our read
             * semantics).  We'll destroy this list of buckets the next time
             * we are asked to perform a read operation - thus ensuring the
             * proper read lifetime.
             */
            if (cur_vecs_used > 0) {
                next_list = ctx->list->next;
                ctx->list->next = ctx->done;
                ctx->done = ctx->list;
                ctx->list = next_list;
            }
            else {
                /* This bucket didn't add a single byte.
                   We can destroy it directly */
                next_list = ctx->list;
                ctx->list = next_list->next;
                serf_bucket_destroy(next_list->bucket);
                serf_bucket_mem_free(bucket->allocator, next_list);
            }

            /* If we have no more in our list, return EOF. */
            if (!ctx->list) {
                ctx->last = NULL;

                if (ctx->hold_open) {
                    status = ctx->hold_open(ctx->hold_open_baton, bucket);
                    if (status || !ctx->list)
                        return status;
                    /* Wow, we 'magically' refilled! */
                }
                else {
                    return APR_EOF;
                }
            }

            /* At this point, it safe to read the next bucket - if we can. */

            /* If the caller doesn't want ALL_AVAIL, decrement the size
             * of the items we just read from the list.
             */
            if (requested != SERF_READ_ALL_AVAIL) {
                int i;

                for (i = 0; i < cur_vecs_used; i++)
                    requested -= vecs[i].iov_len;
            }

            /* Adjust our vecs to account for what we just read. */
            vecs_size -= cur_vecs_used;
            vecs += cur_vecs_used;

            /* We reached our max.  Oh well. */
            if (!requested || !vecs_size) {
                return APR_SUCCESS;
            }
        }
        else if (!status) {
            /* Success and no data. Let's return what we have.
               Better luck next time.

               This scenario is triggered by test_split_buckets(),
               in a case where EAGAIN is really not what we want.
             */

            return APR_SUCCESS;
        }
    }

    return status;
}

static apr_status_t serf_aggregate_read(serf_bucket_t *bucket,
                                        apr_size_t requested,
                                        const char **data, apr_size_t *len)
{
    aggregate_context_t *ctx = bucket->data;
    struct iovec vec;
    int vecs_used;
    apr_status_t status;

    cleanup_aggregate(ctx, bucket->allocator);

    status = read_aggregate(bucket, requested, 1, &vec, &vecs_used);

    if (!vecs_used) {
        *len = 0;
    }
    else {
        *data = vec.iov_base;
        *len = vec.iov_len;
    }

    return status;
}

static apr_status_t serf_aggregate_read_iovec(serf_bucket_t *bucket,
                                              apr_size_t requested,
                                              int vecs_size,
                                              struct iovec *vecs,
                                              int *vecs_used)
{
    aggregate_context_t *ctx = bucket->data;

    cleanup_aggregate(ctx, bucket->allocator);

    return read_aggregate(bucket, requested, vecs_size, vecs, vecs_used);
}

static apr_status_t serf_aggregate_readline(serf_bucket_t *bucket,
                                            int acceptable, int *found,
                                            const char **data, apr_size_t *len)
{
    aggregate_context_t *ctx = bucket->data;
    apr_status_t status;

    *found = SERF_NEWLINE_NONE;
    cleanup_aggregate(ctx, bucket->allocator);

    do {
        serf_bucket_t *head;

        *len = 0;

        if (!ctx->list) {
            if (ctx->hold_open) {
                return ctx->hold_open(ctx->hold_open_baton, bucket);
            }
            else {
                return APR_EOF;
            }
        }

        head = ctx->list->bucket;

        status = serf_bucket_readline(head, acceptable, found,
                                      data, len);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        if (APR_STATUS_IS_EOF(status)) {
            bucket_list_t *next_list;

            /* head bucket is empty, move to to-be-cleaned-up list. */
            next_list = ctx->list->next;
            ctx->list->next = ctx->done;
            ctx->done = ctx->list;
            ctx->list = next_list;

            /* If we have no more in our list, return EOF. */
            if (!ctx->list) {
                ctx->last = NULL;

                if (ctx->hold_open) {
                    return ctx->hold_open(ctx->hold_open_baton, bucket);
                }
                else {
                    return APR_EOF;
                }
            }

            /* we read something, so bail out and let the appl. read again. */
            if (*len)
                status = APR_SUCCESS;
        }

        /* continue with APR_SUCCESS or APR_EOF and no data read yet. */
    } while (!*len && status != APR_EAGAIN);

    return status;
}

static apr_status_t serf_aggregate_peek(serf_bucket_t *bucket,
                                        const char **data,
                                        apr_size_t *len)
{
    aggregate_context_t *ctx = bucket->data;
    serf_bucket_t *head;
    apr_status_t status;

    cleanup_aggregate(ctx, bucket->allocator);

    /* Peek the first bucket in the list, if any. */
    if (!ctx->list) {
        *len = 0;
        if (ctx->hold_open) {
            status = ctx->hold_open(ctx->hold_open_baton, bucket);
            if (APR_STATUS_IS_EAGAIN(status))
                status = APR_SUCCESS;
            return status;
        }
        else {
            return APR_EOF;
        }
    }

    head = ctx->list->bucket;

    status = serf_bucket_peek(head, data, len);

    /* Is the current head *at* eof? */
    while (APR_STATUS_IS_EOF(status) && !*len) {
        bucket_list_t *item = ctx->list;

        if (item->next)
            ctx->list = item->next;
        else
            ctx->list = ctx->last = NULL;

        /* We don't have outstanding data. We are free to release now */
        serf_bucket_destroy(item->bucket);
        serf_bucket_mem_free(bucket->allocator, item);

        if (ctx->list) {
            head = ctx->list->bucket;
            status = serf_bucket_peek(head, data, len);
        }
        else
            break; /* Check hold open below */
    }

    if (APR_STATUS_IS_EOF(status)) {
        if (ctx->list && ctx->list->next) {
            status = APR_SUCCESS;
        } else {
            if (ctx->hold_open) {
                status = ctx->hold_open(ctx->hold_open_baton, bucket);
                if (APR_STATUS_IS_EAGAIN(status))
                    status = APR_SUCCESS;
                return status;
            }
        }
    }

    return status;
}

static serf_bucket_t * serf_aggregate_read_bucket(
    serf_bucket_t *bucket,
    const serf_bucket_type_t *type)
{
    aggregate_context_t *ctx = bucket->data;
    serf_bucket_t *found_bucket;
    bucket_list_t *list;

    if (!ctx->list) {
        return NULL;
    }

    list = ctx->list;
    if (list->bucket->type == type) {
        /* Got the bucket. Consume it from our list. */
        found_bucket = list->bucket;
        ctx->list = list->next;

        /* And destroy the now unused item */
        serf_bucket_mem_free(bucket->allocator, list);

        return found_bucket;
    }

    /* Call read_bucket on first one in our list. */
    return serf_bucket_read_bucket(ctx->list->bucket, type);
}

static apr_uint64_t serf_aggregate_get_remaining(serf_bucket_t *bucket)
{
    aggregate_context_t *ctx = bucket->data;
    apr_uint64_t remaining = 0;
    bucket_list_t *cur;

    if (ctx->hold_open) {
        return SERF_LENGTH_UNKNOWN;
    }

    for(cur = ctx->list; cur != NULL; cur = cur->next) {
        apr_uint64_t bucket_remaining = serf_bucket_get_remaining(cur->bucket);

        if (bucket_remaining == SERF_LENGTH_UNKNOWN) {
            return SERF_LENGTH_UNKNOWN;
        }

        remaining += bucket_remaining;
    }

    return remaining;
}

static apr_status_t serf_aggregate_set_config(serf_bucket_t *bucket,
                                              serf_config_t *config)
{
    /* This bucket doesn't need/update any shared config, but we need to pass
       it along to our wrapped buckets. Store it for all buckets that will be
       be added later. */
    aggregate_context_t *ctx = bucket->data;
    bucket_list_t *cur;
    apr_status_t err_status = APR_SUCCESS;

    ctx->config = config;

    for(cur = ctx->list; cur != NULL; cur = cur->next) {
        apr_status_t status;

        status = serf_bucket_set_config(cur->bucket, config);
        if (status)
            err_status = status;
    }
    return err_status;
}

const serf_bucket_type_t serf_bucket_type_aggregate = {
    "AGGREGATE",
    serf_aggregate_read,
    serf_aggregate_readline,
    serf_aggregate_read_iovec,
    serf_default_read_for_sendfile,
    serf_buckets_are_v2,
    serf_aggregate_peek,
    serf_aggregate_destroy_and_data,
    serf_aggregate_read_bucket,
    serf_aggregate_get_remaining,
    serf_aggregate_set_config,
};
