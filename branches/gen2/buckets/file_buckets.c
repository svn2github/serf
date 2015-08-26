/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2003 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 */

#include <apr_pools.h>

#include "serf.h"
#include "serf_bucket_util.h"

typedef struct {
    apr_file_t *file;

    serf_databuf_t databuf;

} file_context_t;


static apr_status_t file_reader(void *baton, apr_size_t bufsize,
                                char *buf, apr_size_t *len)
{
    file_context_t *ctx = baton;

    *len = bufsize;
    return apr_file_read(ctx->file, buf, len);
}

SERF_DECLARE(serf_bucket_t *) serf_bucket_file_create(
    apr_file_t *file,
    serf_bucket_alloc_t *allocator)
{
    file_context_t *ctx;
#if APR_HAS_MMAP
    apr_finfo_t finfo;
    const char *file_path;

    /* See if we'd be better off mmap'ing this file instead. */
    apr_file_name_get(&file_path, file);
    apr_stat(&finfo, file_path, APR_FINFO_SIZE,
             serf_bucket_allocator_get_pool(allocator));
    if (APR_MMAP_CANDIDATE(finfo.size)) {
        apr_mmap_t *file_mmap;
        apr_mmap_create(&file_mmap, file, 0, finfo.size, APR_MMAP_READ,
                        serf_bucket_allocator_get_pool(allocator));
        return serf_bucket_mmap_create(file_mmap, allocator);
    }
#endif

    /* Oh, well. */
    ctx = serf_bucket_mem_alloc(allocator, sizeof(*ctx));
    ctx->file = file;

    serf_databuf_init(&ctx->databuf);
    ctx->databuf.read = file_reader;
    ctx->databuf.read_baton = ctx;

    return serf_bucket_create(&serf_bucket_type_file, allocator, ctx);
}

static apr_status_t serf_file_read(serf_bucket_t *bucket,
                                   apr_size_t requested,
                                   const char **data, apr_size_t *len)
{
    file_context_t *ctx = bucket->data;

    return serf_databuf_read(&ctx->databuf, requested, data, len);
}

static apr_status_t serf_file_readline(serf_bucket_t *bucket,
                                       int acceptable, int *found,
                                       const char **data, apr_size_t *len)
{
    file_context_t *ctx = bucket->data;

    return serf_databuf_readline(&ctx->databuf, acceptable, found, data, len);
}

static apr_status_t serf_file_peek(serf_bucket_t *bucket,
                                   const char **data,
                                   apr_size_t *len)
{
    file_context_t *ctx = bucket->data;

    return serf_databuf_peek(&ctx->databuf, data, len);
}

SERF_DECLARE_DATA const serf_bucket_type_t serf_bucket_type_file = {
    "FILE",
    serf_file_read,
    serf_file_readline,
    serf_default_read_iovec,
    serf_default_read_for_sendfile,
    serf_default_read_bucket,
    serf_file_peek,
    serf_default_get_metadata,
    serf_default_set_metadata,
    serf_default_destroy_and_data,
};
