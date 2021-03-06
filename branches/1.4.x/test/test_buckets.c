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

#define APR_WANT_MEMFUNC
#include <apr_want.h>
#include <apr.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_random.h>
#include <zlib.h>

#include "serf.h"
#include "test_serf.h"

/* test case has access to internal functions. */
#include "serf_private.h"
#include "serf_bucket_util.h"

#include "protocols/http2_buckets.h"

#ifdef SERF_DEBUG_BUCKET_USE
#define DRAIN_BUCKET(b) serf__bucket_drain(b)
#else
#define DRAIN_BUCKET(b) while (0 && (b))
#endif


static apr_status_t read_all(serf_bucket_t *bkt,
                             char *buf,
                             apr_size_t buf_len,
                             apr_size_t *read_len)
{
    const char *data;
    apr_size_t data_len;
    apr_status_t status;
    apr_size_t read;

    read = 0;

    do
    {
        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &data_len);

        if (!SERF_BUCKET_READ_ERROR(status))
        {
            if (data_len > buf_len - read)
            {
                /* Buffer is not large enough to read all data */
                data_len = buf_len - read;
                status = REPORT_TEST_SUITE_ERROR();
            }
            memcpy(buf + read, data, data_len);
            read += data_len;
        }
    } while(status == APR_SUCCESS);

    *read_len = read;
    return status;
}

/* Reads bucket until EOF found and compares read data with zero terminated
   string expected. Report all failures using CuTest. */
void read_and_check_bucket(CuTest *tc, serf_bucket_t *bkt,
                           const char *expected)
{
    apr_status_t status;
    do
    {
        const char *data;
        apr_size_t len;

        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &len);
        CuAssert(tc, "Got error during bucket reading.",
                 !SERF_BUCKET_READ_ERROR(status));
        CuAssert(tc, "Read more data than expected.",
                 strlen(expected) >= len);
        CuAssert(tc, "Read data is not equal to expected.",
                 strncmp(expected, data, len) == 0);

        expected += len;
    } while(!APR_STATUS_IS_EOF(status));

    CuAssert(tc, "Read less data than expected.", strlen(expected) == 0);
}

/* Reads bucket with serf_bucket_readline until EOF found and compares:
   - actual line endings with expected line endings
   - actual data with zero terminated string expected.
   Reports all failures using CuTest. */
void readlines_and_check_bucket(CuTest *tc, serf_bucket_t *bkt,
                                int acceptable,
                                const char *expected,
                                int expected_nr_of_lines)
{
    apr_status_t status;
    int actual_nr_of_lines = 0;

    do
    {
        const char *data;
        apr_size_t len;
        int found;

        status = serf_bucket_readline(bkt, acceptable, &found,
                                      &data, &len);
        CuAssert(tc, "Got error during bucket reading.",
                 !SERF_BUCKET_READ_ERROR(status));
        CuAssert(tc, "Read more data than expected.",
                 strlen(expected) >= len);
        CuAssert(tc, "Read data is not equal to expected.",
                 strncmp(expected, data, len) == 0);

        expected += len;

        if (found == SERF_NEWLINE_CRLF_SPLIT)
            continue;

        if (found != SERF_NEWLINE_NONE)
        {
            actual_nr_of_lines++;

            CuAssert(tc, "Unexpected line ending type!",
                     found & acceptable);
            if (found & SERF_NEWLINE_CR)
                CuAssert(tc, "CR Line ending was reported but not in data!",
                         strncmp(data + len - 1, "\r", 1) == 0);
            if (found & SERF_NEWLINE_LF)
                CuAssert(tc, "LF Line ending was reported but not in data!",
                         strncmp(data + len - 1, "\n", 1) == 0);
            if (found & SERF_NEWLINE_CRLF)
                CuAssert(tc, "CRLF Line ending was reported but not in data!",
                         strncmp(data + len - 2, "\r\n", 2) == 0);
        } else
        {
            if (status == APR_EOF && len)
                actual_nr_of_lines++;

            if (acceptable & SERF_NEWLINE_CR)
                CuAssert(tc, "CR Line ending was not reported but in data!",
                         strncmp(data + len - 1, "\r", 1) != 0);
            if (acceptable & SERF_NEWLINE_LF)
                CuAssert(tc, "LF Line ending was not reported but in data!",
                         strncmp(data + len - 1, "\n", 1) != 0);
            if (acceptable & SERF_NEWLINE_CRLF)
                CuAssert(tc, "CRLF Line ending was not reported but in data!",
                         strncmp(data + len - 2, "\r\n", 2) != 0);
        }
    } while(!APR_STATUS_IS_EOF(status));

    CuAssertIntEquals(tc, expected_nr_of_lines, actual_nr_of_lines);
    CuAssert(tc, "Read less data than expected.", strlen(expected) == 0);
}

static apr_status_t discard_data(serf_bucket_t *bkt,
                                 apr_size_t *read_len)
{
    const char *data;
    apr_size_t data_len;
    apr_status_t status;
    apr_size_t read;

    read = 0;

    do
    {
        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &data_len);

        if (!SERF_BUCKET_READ_ERROR(status)) {
            read += data_len;
        }
    } while(status == APR_SUCCESS);

    *read_len = read;
    return status;
}

/******************************** TEST CASES **********************************/

static void test_simple_bucket_readline(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    apr_status_t status;
    serf_bucket_t *bkt;
    const char *data;
    int found;
    apr_size_t len;
    const char *body;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    bkt = SERF_BUCKET_SIMPLE_STRING(
                                    "line1" CRLF
                                    "line2",
                                    alloc);

    /* Initialize parameters to check that they will be initialized. */
    len = 0x112233;
    data = 0;
    status = serf_bucket_readline(bkt, SERF_NEWLINE_CRLF, &found, &data, &len);

    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, SERF_NEWLINE_CRLF, found);
    CuAssertIntEquals(tc, 7, len);
    CuAssert(tc, data, strncmp("line1" CRLF, data, len) == 0);

    /* Initialize parameters to check that they will be initialized. */
    len = 0x112233;
    data = 0;
    status = serf_bucket_readline(bkt, SERF_NEWLINE_CRLF, &found, &data, &len);

    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, SERF_NEWLINE_NONE, found);
    CuAssertIntEquals(tc, 5, len);
    CuAssert(tc, data, strncmp("line2", data, len) == 0);
    serf_bucket_destroy(bkt);

    /* acceptable line types should be reported */
    bkt = SERF_BUCKET_SIMPLE_STRING("line1" CRLF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_CRLF, "line1" CRLF, 1);
    serf_bucket_destroy(bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("line1" LF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_LF, "line1" LF, 1);
    serf_bucket_destroy(bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("line1" LF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_LF, "line1" LF, 1);
    serf_bucket_destroy(bkt);

    /* special cases, but acceptable */
    bkt = SERF_BUCKET_SIMPLE_STRING("line1" CRLF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_CR, "line1" CRLF, 2);
    serf_bucket_destroy(bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("line1" CRLF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_LF, "line1" CRLF, 1);
    serf_bucket_destroy(bkt);

    /* Unacceptable line types should not be reported */
    bkt = SERF_BUCKET_SIMPLE_STRING("line1" LF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_CR, "line1" LF, 1);
    serf_bucket_destroy(bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("line1" LF, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_CRLF, "line1" LF, 1);
    serf_bucket_destroy(bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("line1" CR, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_LF, "line1" CR, 1);
    serf_bucket_destroy(bkt);

#if 0
    /* TODO: looks like a bug, CRLF acceptable on buffer with CR returns
       SERF_NEWLINE_CRLF_SPLIT, but here that CR comes at the end of the
       buffer (APR_EOF), so should have been SERF_NEWLINE_NONE! */
    bkt = SERF_BUCKET_SIMPLE_STRING("line1" CR, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_CRLF, "line1" CR, 1);
    serf_bucket_destroy(bkt);
#endif

    body = "12345678901234567890" CRLF
           "12345678901234567890" CRLF
           "12345678901234567890" CRLF;
    bkt = SERF_BUCKET_SIMPLE_STRING(body, alloc);
    readlines_and_check_bucket(tc, bkt, SERF_NEWLINE_LF, body, 3);
    serf_bucket_destroy(bkt);
}

static void test_response_bucket_read(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    apr_status_t status;
    int found;
    const char *data;
    apr_size_t len;
    serf_status_line sline;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING(
        "HTTP/1.1 200 OK" CRLF
        "Content-Length: 7" CRLF
        CRLF
        "abc1234",
        alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    /* Read all bucket and check it content. */
    read_and_check_bucket(tc, bkt, "abc1234");
    serf_bucket_destroy(bkt);

    tmp = SERF_BUCKET_SIMPLE_STRING(
            "HTTP/1.1 200 OK" CRLF
            "cONTENT-lENGTH: 7" CRLF
            CRLF
            "abc1234" /* NO CRLF... just 7 bytes!*/
            "HTTP/1.1 304 Unmodified" CRLF
            CRLF,
            alloc);

    bkt = serf_bucket_response_create(
              serf_bucket_barrier_create(tmp, alloc),
              alloc);

    status = serf_bucket_readline(bkt, SERF_NEWLINE_ANY,
                                  &found, &data, &len);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 7, len);
    CuAssertStrnEquals(tc, "abc1234", len, data);
    serf_bucket_destroy(bkt);

    /* 304 has no body, but we should be able to read it */
    bkt = serf_bucket_response_create(tmp, alloc);
    read_and_check_bucket(tc, bkt, "");

    CuAssertIntEquals(tc, APR_SUCCESS,
                      serf_bucket_response_status(bkt, &sline));
    CuAssertIntEquals(tc, 304, sline.code);
    serf_bucket_destroy(bkt);
}

static void test_response_bucket_headers(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp, *hdr;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING(
        "HTTP/1.1 405 Method Not Allowed" CRLF
        "Date: Sat, 12 Jun 2010 14:17:10 GMT"  CRLF
        "Server: Apache"  CRLF
        "Allow: "  CRLF
        "Content-Length: 7"  CRLF
        "Content-Type: text/html; charset=iso-8859-1" CRLF
        "NoSpace:" CRLF
        CRLF
        "abc1234",
        alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    /* Read all bucket and check it content. */
    read_and_check_bucket(tc, bkt, "abc1234");

    hdr = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc,
        "",
        serf_bucket_headers_get(hdr, "Allow"));
    CuAssertStrEquals(tc,
        "7",
        serf_bucket_headers_get(hdr, "Content-Length"));
    CuAssertStrEquals(tc,
        "",
        serf_bucket_headers_get(hdr, "NoSpace"));
    serf_bucket_destroy(bkt);
}

static void test_response_bucket_chunked_read(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp, *hdrs;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING(
        "HTTP/1.1 200 OK" CRLF
        "Transfer-Encoding: chunked" CRLF
        CRLF
        "3" CRLF
        "abc" CRLF
        "4" CRLF
        "1234" CRLF
        "0" CRLF
        "Footer: value" CRLF
        CRLF,
        alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    /* Read all bucket and check it content. */
    read_and_check_bucket(tc, bkt, "abc1234");

    hdrs = serf_bucket_response_get_headers(bkt);
    CuAssertTrue(tc, hdrs != NULL);

    /* Check that trailing headers parsed correctly. */
    CuAssertStrEquals(tc, "value", serf_bucket_headers_get(hdrs, "Footer"));
    serf_bucket_destroy(bkt);
}

static void test_bucket_header_set(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    serf_bucket_t *hdrs = serf_bucket_headers_create(alloc);

    CuAssertTrue(tc, hdrs != NULL);

    /* no headers set yet */
    CuAssertPtrEquals(tc, NULL, (void *)serf_bucket_headers_get(hdrs, "Foo"));

    serf_bucket_headers_set(hdrs, "Foo", "bar");

    CuAssertStrEquals(tc, "bar", serf_bucket_headers_get(hdrs, "Foo"));

    serf_bucket_headers_set(hdrs, "Foo", "baz");

    CuAssertStrEquals(tc, "bar,baz", serf_bucket_headers_get(hdrs, "Foo"));

    serf_bucket_headers_set(hdrs, "Foo", "test");

    CuAssertStrEquals(tc, "bar,baz,test", serf_bucket_headers_get(hdrs, "Foo"));

    /* headers are case insensitive. */
    CuAssertStrEquals(tc, "bar,baz,test", serf_bucket_headers_get(hdrs, "fOo"));

    /* header not found */
    CuAssertPtrEquals(tc, NULL, (void *)serf_bucket_headers_get(hdrs, "blabla"));

    serf_bucket_destroy(hdrs);
}

static int
store_header_in_table(void *baton, const char *key, const char *value)
{
    apr_table_t *hdrs = baton;

    apr_table_add(hdrs, key, value);

    return 0;
}

static void test_bucket_header_do(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    serf_bucket_t *hdrs = serf_bucket_headers_create(alloc);
    struct kv {
        const char *key;
        const char *value;
    } exp_hdrs[] = {
        { "Foo", "bar" },
        { "Foo", "baz" },
        { "Bar", "foo" },
        { "Faz", "boo" },
        { "Foo", "bof" },
    };
    int i;
    apr_table_t *actual_hdrs;
    const apr_table_entry_t *elts;
    const apr_array_header_t *arr;
    const int num_hdrs = sizeof(exp_hdrs) / sizeof(exp_hdrs[0]);

    for (i = 0 ; i < num_hdrs; i ++)
        serf_bucket_headers_set(hdrs, exp_hdrs[i].key, exp_hdrs[i].value);

    actual_hdrs = apr_table_make(tb->pool, num_hdrs);

    serf_bucket_headers_do(hdrs, store_header_in_table, actual_hdrs);

    /* The actual_hdrs dictionary should now have all key/value pairs, in the
       same order as exp_hdrs (assuming apr_table_t maintains order). */
    CuAssertIntEquals(tc, num_hdrs, apr_table_elts(actual_hdrs)->nelts);

    arr = apr_table_elts(actual_hdrs);
    CuAssertPtrNotNull(tc, arr);

    elts = (const apr_table_entry_t *)arr->elts;

    for (i = 0; i < arr->nelts; ++i) {
        CuAssertStrEquals(tc, elts[i].key, exp_hdrs[i].key);
        CuAssertStrEquals(tc, elts[i].val, exp_hdrs[i].value);
    }

    serf_bucket_destroy(hdrs);
}

static void test_iovec_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    apr_status_t status;
    serf_bucket_t *bkt, *iobkt;
    const char *data;
    apr_size_t len;
    struct iovec vecs[32];
    struct iovec tgt_vecs[32];
    int i;
    int vecs_used;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    /* Test 1: Read a single string in an iovec, store it in a iovec_bucket
       and then read it back. */
    bkt = SERF_BUCKET_SIMPLE_STRING(
        "line1" CRLF
        "line2",
        alloc);

    status = serf_bucket_read_iovec(bkt, SERF_READ_ALL_AVAIL, 32, vecs,
                                    &vecs_used);

    iobkt = serf_bucket_iovec_create(vecs, vecs_used, alloc);

    /* Check serf_bucket_get_remaining() result. */
    CuAssertIntEquals(tc, 12, (int)serf_bucket_get_remaining(iobkt));

    /* Check available data */
    status = serf_bucket_peek(iobkt, &data, &len);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, strlen("line1" CRLF "line2"), len);

    /* Try to read only a few bytes (less than what's in the first buffer). */
    status = serf_bucket_read_iovec(iobkt, 3, 32, tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 1, vecs_used);
    CuAssertIntEquals(tc, 3, tgt_vecs[0].iov_len);
    CuAssert(tc, tgt_vecs[0].iov_base,
             strncmp("lin", tgt_vecs[0].iov_base, tgt_vecs[0].iov_len) == 0);

    status = serf_bucket_peek(iobkt, &data, &len);
    CuAssertTrue(tc, (status == APR_SUCCESS) || APR_STATUS_IS_EOF(status));
    CuAssertIntEquals(tc, 0, memcmp(data, "e1" CRLF "line2", len));

    /* Check serf_bucket_get_remaining() result. */
    CuAssertIntEquals(tc, 9, (int)serf_bucket_get_remaining(iobkt));

    /* Read the rest of the data. */
    status = serf_bucket_read_iovec(iobkt, SERF_READ_ALL_AVAIL, 32, tgt_vecs,
                                    &vecs_used);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 1, vecs_used);
    CuAssertIntEquals(tc, strlen("e1" CRLF "line2"), tgt_vecs[0].iov_len);
    CuAssert(tc, tgt_vecs[0].iov_base,
             strncmp("e1" CRLF "line2", tgt_vecs[0].iov_base, tgt_vecs[0].iov_len - 3) == 0);

    /* Check serf_bucket_get_remaining() result. */
    CuAssertIntEquals(tc, 0, (int)serf_bucket_get_remaining(iobkt));

    /* Bucket should now be empty */
    status = serf_bucket_peek(iobkt, &data, &len);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 0, len);

    /* Test 2: Read multiple character bufs in an iovec, then read them back
       in bursts. */
    for (i = 0; i < 32 ; i++) {
        vecs[i].iov_base = apr_psprintf(tb->pool, "data %02d 901234567890", i);
        vecs[i].iov_len = strlen(vecs[i].iov_base);
    }

    serf_bucket_destroy(bkt);
    serf_bucket_destroy(iobkt);

    iobkt = serf_bucket_iovec_create(vecs, 32, alloc);

    /* Check serf_bucket_get_remaining() result. */
    CuAssertIntEquals(tc, 640, (int)serf_bucket_get_remaining(iobkt));

    /* Check that some data is in the buffer. Don't verify the actual data, the
       amount of data returned is not guaranteed to be the full buffer. */
    status = serf_bucket_peek(iobkt, &data, &len);
    CuAssertTrue(tc, len > 0);
    CuAssertIntEquals(tc, APR_SUCCESS, status); /* this assumes not all data is
                                                   returned at once,
                                                   not guaranteed! */

    /* Read 1 buf.   20 = sizeof("data %2d 901234567890") */
    status = serf_bucket_read_iovec(iobkt, 1 * 20, 32,
                                    tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 1, vecs_used);
    CuAssert(tc, tgt_vecs[0].iov_base,
             strncmp("data 00 901234567890", tgt_vecs[0].iov_base, tgt_vecs[0].iov_len) == 0);
    CuAssertIntEquals(tc, 620, (int)serf_bucket_get_remaining(iobkt));

    /* Read 2 bufs. */
    status = serf_bucket_read_iovec(iobkt, 2 * 20, 32,
                                    tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 2, vecs_used);
    CuAssertIntEquals(tc, 580, (int)serf_bucket_get_remaining(iobkt));

    /* Read the remaining 29 bufs. */
    vecs_used = 400;  /* test if iovec code correctly resets vecs_used */
    status = serf_bucket_read_iovec(iobkt, SERF_READ_ALL_AVAIL, 32,
                                    tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 29, vecs_used);
    CuAssertIntEquals(tc, 0, (int)serf_bucket_get_remaining(iobkt));

    /* Test 3: use serf_bucket_read */
    for (i = 0; i < 32 ; i++) {
        vecs[i].iov_base = apr_psprintf(tb->pool, "DATA %02d 901234567890", i);
        vecs[i].iov_len = strlen(vecs[i].iov_base);
    }

    serf_bucket_destroy(iobkt);

    iobkt = serf_bucket_iovec_create(vecs, 32, alloc);

    /* Check serf_bucket_get_remaining() result. */
    CuAssertIntEquals(tc, 640, (int)serf_bucket_get_remaining(iobkt));

    status = serf_bucket_read(iobkt, 10, &data, &len);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 10, len);
    CuAssert(tc, data,
             strncmp("DATA 00 90", data, len) == 0);
    CuAssertIntEquals(tc, 630, (int)serf_bucket_get_remaining(iobkt));

    status = serf_bucket_read(iobkt, 10, &data, &len);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 10, len);
    CuAssert(tc, tgt_vecs[0].iov_base,
             strncmp("1234567890", data, len) == 0);
    CuAssertIntEquals(tc, 620, (int)serf_bucket_get_remaining(iobkt));

    for (i = 1; i < 31 ; i++) {
        const char *exp = apr_psprintf(tb->pool, "DATA %02d 901234567890", i);
        status = serf_bucket_read(iobkt, SERF_READ_ALL_AVAIL, &data, &len);
        CuAssertIntEquals(tc, APR_SUCCESS, status);
        CuAssertIntEquals(tc, 20, len);
        CuAssert(tc, data,
                 strncmp(exp, data, len) == 0);

    }

    CuAssertIntEquals(tc, 20, (int)serf_bucket_get_remaining(iobkt));

    status = serf_bucket_read(iobkt, 20, &data, &len);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 20, len);
    CuAssert(tc, data,
             strncmp("DATA 31 901234567890", data, len) == 0);
    CuAssertIntEquals(tc, 0, (int)serf_bucket_get_remaining(iobkt));

    serf_bucket_destroy(iobkt);

    /* Test 3: read an empty iovec */
    iobkt = serf_bucket_iovec_create(vecs, 0, alloc);
    status = serf_bucket_read_iovec(iobkt, SERF_READ_ALL_AVAIL, 32,
                                    tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 0, vecs_used);
    CuAssertIntEquals(tc, 0, (int)serf_bucket_get_remaining(iobkt));

    status = serf_bucket_read(iobkt, SERF_READ_ALL_AVAIL, &data, &len);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 0, len);
    CuAssertIntEquals(tc, 0, (int)serf_bucket_get_remaining(iobkt));

    serf_bucket_destroy(iobkt);

    /* Test 4: read 0 bytes from an iovec */
    bkt = SERF_BUCKET_SIMPLE_STRING("line1" CRLF, alloc);
    status = serf_bucket_read_iovec(bkt, SERF_READ_ALL_AVAIL, 32, vecs,
                                    &vecs_used);
    iobkt = serf_bucket_iovec_create(vecs, vecs_used, alloc);
    /* Check serf_bucket_get_remaining() result. */
    CuAssertIntEquals(tc, 7, (int)serf_bucket_get_remaining(iobkt));

    status = serf_bucket_read_iovec(iobkt, 0, 32,
                                    tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 0, vecs_used);
    CuAssertIntEquals(tc, 7, (int)serf_bucket_get_remaining(iobkt));
    serf_bucket_destroy(bkt);
    DRAIN_BUCKET(iobkt);
    serf_bucket_destroy(iobkt);
}

/* Construct a header bucket with some headers, and then read from it. */
static void test_header_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char *cur;

    serf_bucket_t *hdrs = serf_bucket_headers_create(alloc);
    CuAssertTrue(tc, hdrs != NULL);

    serf_bucket_headers_set(hdrs, "Content-Type", "text/plain");
    serf_bucket_headers_set(hdrs, "Content-Length", "100");

    /* Note: order not guaranteed, assume here that it's fifo. */
    cur = "Content-Type: text/plain" CRLF
          "Content-Length: 100" CRLF
          CRLF;

    read_and_check_bucket(tc, hdrs, cur);

    serf_bucket_destroy(hdrs);
}

static apr_status_t append_magic(void *baton,
                                 serf_bucket_t *bucket)
{
  serf_bucket_t *bkt;
  int *append = baton;

  if (*append)
    {
      (*append)--;
      bkt = SERF_BUCKET_SIMPLE_STRING("magic", bucket->allocator);
      serf_bucket_aggregate_append(bucket, bkt);
      return APR_SUCCESS;
    }

  return APR_EOF;
}


static void test_aggregate_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    apr_status_t status;
    serf_bucket_t *bkt, *aggbkt;
    struct iovec tgt_vecs[32];
    int vecs_used;
    apr_size_t len;
    const char *data;
    int append = 3;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char *BODY = "12345678901234567890"\
                       "12345678901234567890"\
                       "12345678901234567890"\
                       CRLF;

    /* Test 1: read 0 bytes from an aggregate */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    /* If you see result -1 in the next line, this is most likely caused by
       not properly detecting v2 buckets via the magic function pointer.
       Most likely you are seeing a linkage problem which causes seeing
       different pointers for serf_buckets_are_v2() */
    CuAssertIntEquals(tc, 62, (int)serf_bucket_get_remaining(aggbkt));

    status = serf_bucket_read_iovec(aggbkt, 0, 32,
                                    tgt_vecs, &vecs_used);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 0, vecs_used);


    /* Test 2: peek the available bytes, should be non-0 */
    len = SERF_READ_ALL_AVAIL;
    status = serf_bucket_peek(aggbkt, &data, &len);

    /* status should be either APR_SUCCESS or APR_EOF */
    if (status == APR_SUCCESS)
        CuAssertTrue(tc, len > 0 && len < strlen(BODY));
    else if (status == APR_EOF)
        CuAssertIntEquals(tc, strlen(BODY), len);
    else
        CuAssertIntEquals(tc, APR_SUCCESS, status);

    /* Test 3: read the data from the bucket. */
    read_and_check_bucket(tc, aggbkt, BODY);

    serf_bucket_destroy(aggbkt);

    /* Test 4: multiple child buckets appended. */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 15, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+15, strlen(BODY)-15, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    CuAssertTrue(tc, serf_bucket_get_remaining(aggbkt) == 62);

    read_and_check_bucket(tc, aggbkt, BODY);

    serf_bucket_destroy(aggbkt);

    /* Test 5: multiple child buckets prepended. */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+15, strlen(BODY)-15, alloc);
    serf_bucket_aggregate_prepend(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 15, alloc);
    serf_bucket_aggregate_prepend(aggbkt, bkt);

    CuAssertTrue(tc, serf_bucket_get_remaining(aggbkt) == 62);

    read_and_check_bucket(tc, aggbkt, BODY);

    serf_bucket_destroy(aggbkt);

    /* Test 6: ensure peek doesn't return APR_EAGAIN, or APR_EOF incorrectly. */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 15, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+15, strlen(BODY)-15, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    len = 1234;
    status = serf_bucket_peek(aggbkt, &data, &len);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssert(tc, "Length should be positive.",
             len > 0 && len <= strlen(BODY) );
    CuAssert(tc, "Data should match first part of body.",
             strncmp(BODY, data, len) == 0);

    serf_bucket_destroy(aggbkt);

    aggbkt = serf_bucket_aggregate_create(alloc);

    /* Put bkt in the aggregate */
    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    /* And now remove it */
    CuAssertPtrEquals(tc, bkt,
                      serf_bucket_read_bucket(aggbkt,
                                              &serf_bucket_type_simple));
    /* Ok, then aggregate should be empty */
    read_and_check_bucket(tc, aggbkt, "");
    /* And can be destroyed */
    serf_bucket_destroy(aggbkt);
    /* While it doesn't affect the inner bucket */
    read_and_check_bucket(tc, bkt, BODY);
    serf_bucket_destroy(bkt);

    aggbkt = serf_bucket_aggregate_create(alloc);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 15, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    serf_bucket_aggregate_hold_open(aggbkt, append_magic, &append);

    CuAssertIntEquals(tc, APR_EOF,
                      serf_bucket_read_iovec(aggbkt, SERF_READ_ALL_AVAIL,
                                             32, tgt_vecs, &vecs_used));
    CuAssertIntEquals(tc, 4, vecs_used);
    CuAssertIntEquals(tc, 15, tgt_vecs[0].iov_len);
    CuAssertIntEquals(tc, 5, tgt_vecs[1].iov_len);
    CuAssertIntEquals(tc, 5, tgt_vecs[2].iov_len);
    CuAssertIntEquals(tc, 5, tgt_vecs[3].iov_len);

    serf_bucket_destroy(aggbkt);

    /* Test 7: test prepend to empty aggregate bucket. */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING("prepend", alloc);
    serf_bucket_aggregate_prepend(aggbkt, bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("append", alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    read_and_check_bucket(tc, aggbkt, "prepend" "append");

    serf_bucket_destroy(aggbkt);

    /* Test 8: test empty bucket handling since we have optimized
               codepath for this case. */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING("", alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    bkt = SERF_BUCKET_SIMPLE_STRING("body", alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    read_and_check_bucket(tc, aggbkt, "" "body");

    serf_bucket_destroy(aggbkt);
}

static void test_aggregate_bucket_readline(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *aggbkt;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char *BODY = "12345678901234567890" CRLF
                       "12345678901234567890" CRLF
                       "12345678901234567890" CRLF;

    /* Test 1: read lines from an aggregate bucket */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 22, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt); /* 1st line */
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+22, strlen(BODY)-22, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt); /* 2nd and 3rd line */

    readlines_and_check_bucket(tc, aggbkt, SERF_NEWLINE_CRLF, BODY, 3);

    serf_bucket_destroy(aggbkt);

    /* Test 2: start with empty bucket */
    aggbkt = serf_bucket_aggregate_create(alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING_LEN("", 0, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt); /* empty bucket */
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 22, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt); /* 1st line */
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+22, strlen(BODY)-22, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt); /* 2nd and 3rd line */

    readlines_and_check_bucket(tc, aggbkt, SERF_NEWLINE_CRLF, BODY, 3);

    serf_bucket_destroy(aggbkt);
}

/* Test for issue: the server aborts the connection in the middle of
   streaming the body of the response, where the length was set with the
   Content-Length header. Test that we get a decent error code from the
   response bucket instead of APR_EOF. */
static void test_response_body_too_small_cl(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    /* Make a response of 60 bytes, but set the Content-Length to 100. */
#define BODY "12345678901234567890"\
             "12345678901234567890"\
             "12345678901234567890"

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Content-Length: 100" CRLF
                                    CRLF
                                    BODY,
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        const char *data;
        apr_size_t len;
        apr_status_t status;

        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &len);

        /* On error data and len is undefined.*/
        if (!SERF_BUCKET_READ_ERROR(status)) {
            CuAssert(tc, "Read more data than expected.",
                     strlen(BODY) >= len);
            CuAssert(tc, "Read data is not equal to expected.",
                     strncmp(BODY, data, len) == 0);
            CuAssert(tc, "Error expected due to response body too short!",
                     SERF_BUCKET_READ_ERROR(status));
        }
        CuAssertIntEquals(tc, SERF_ERROR_TRUNCATED_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}
#undef BODY

/* Test for issue: the server aborts the connection in the middle of
   streaming the body of the response, using chunked encoding. Test that we get
   a decent error code from the response bucket instead of APR_EOF. */
static void test_response_body_too_small_chunked(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    /* Make a response of 60 bytes, but set the chunk size to 60 and don't end
       with chunk of length 0. */
#define BODY "12345678901234567890"\
"12345678901234567890"\
"12345678901234567890"

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    CRLF
                                    "64" CRLF BODY,
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        const char *data;
        apr_size_t len;
        apr_status_t status;

        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &len);

        /* On error data and len is undefined.*/
        if (!SERF_BUCKET_READ_ERROR(status)) {
            CuAssert(tc, "Read more data than expected.",
                     strlen(BODY) >= len);
            CuAssert(tc, "Read data is not equal to expected.",
                     strncmp(BODY, data, len) == 0);
            CuAssert(tc, "Error expected due to response body too short!",
                     SERF_BUCKET_READ_ERROR(status));
        }
        CuAssertIntEquals(tc, SERF_ERROR_TRUNCATED_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}
#undef BODY

/* Test for issue: the server aborts the connection in the middle of
   streaming trailing CRLF after body chunk. Test that we get
   a decent error code from the response bucket instead of APR_EOF. */
static void test_response_body_chunked_no_crlf(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    CRLF
                                    "2" CRLF
                                    "AB",
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);

        CuAssertIntEquals(tc, SERF_ERROR_TRUNCATED_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

/* Test for issue: the server aborts the connection in the middle of
   streaming trailing CRLF after body chunk. Test that we get
   a decent error code from the response bucket instead of APR_EOF. */
static void test_response_body_chunked_incomplete_crlf(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    CRLF
                                    "2" CRLF
                                    "AB"
                                    "\r",
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);

        CuAssertIntEquals(tc, SERF_ERROR_TRUNCATED_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

static void test_response_body_chunked_gzip_small(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    "Content-Encoding: gzip" CRLF
                                    CRLF
                                    "2" CRLF
                                    "A",
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);

        CuAssertIntEquals(tc, SERF_ERROR_TRUNCATED_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

/* Test for issue: the server aborts the connection and also sends
   a bogus CRLF in place of the expected chunk size. Test that we get
   a decent error code from the response bucket instead of APR_EOF. */
static void test_response_body_chunked_bogus_crlf(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    CRLF
                                    "2" CRLF
                                    "AB" CRLF
                                    CRLF,
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);

        CuAssertIntEquals(tc, SERF_ERROR_BAD_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

static void test_response_body_chunked_invalid_len(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    CRLF
                                    "2" CRLF
                                    "AB" CRLF
                                    "invalid" CRLF
                                    CRLF,
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);

        CuAssertIntEquals(tc, SERF_ERROR_BAD_HTTP_RESPONSE, status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

static void test_response_body_chunked_overflow_len(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Transfer-Encoding: chunked" CRLF
                                    CRLF
                                    "2" CRLF
                                    "AB" CRLF
                                    "12345678901234567890123456789" CRLF
                                    CRLF,
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);

        CuAssertIntEquals(tc, APR_FROM_OS_ERROR(ERANGE), status);
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

static void test_response_bucket_peek_at_headers(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *resp_bkt1, *tmp, *hdrs;
    serf_status_line sl;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char *hdr_val, *cur;
    apr_status_t status;

#define EXP_RESPONSE "HTTP/1.1 200 OK" CRLF\
                     "Content-Type: text/plain" CRLF\
                     "Content-Length: 100" CRLF\
                     CRLF\
                     "12345678901234567890"\
                     "12345678901234567890"\
                     "12345678901234567890"

    tmp = SERF_BUCKET_SIMPLE_STRING(EXP_RESPONSE,
                                    alloc);

    resp_bkt1 = serf_bucket_response_create(tmp, alloc);

    status = serf_bucket_response_status(resp_bkt1, &sl);
    CuAssertIntEquals(tc, 200, sl.code);
    CuAssertStrEquals(tc, "OK", sl.reason);
    CuAssertIntEquals(tc, SERF_HTTP_11, sl.version);

    /* Ensure that the status line & headers are read in the response_bucket. */
    status = serf_bucket_response_wait_for_headers(resp_bkt1);
    CuAssert(tc, "Unexpected error when waiting for response headers",
             !SERF_BUCKET_READ_ERROR(status));

    hdrs = serf_bucket_response_get_headers(resp_bkt1);
    CuAssertPtrNotNull(tc, hdrs);

    hdr_val = serf_bucket_headers_get(hdrs, "Content-Type");
    CuAssertStrEquals(tc, "text/plain", hdr_val);
    hdr_val = serf_bucket_headers_get(hdrs, "Content-Length");
    CuAssertStrEquals(tc, "100", hdr_val);

    /* Create a new bucket for the response which still has the original
       status line & headers. */

    status = serf_response_full_become_aggregate(resp_bkt1);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    cur = EXP_RESPONSE;

    while (1) {
        const char *data;
        apr_size_t len;

        status = serf_bucket_read(resp_bkt1, SERF_READ_ALL_AVAIL, &data, &len);
        CuAssert(tc, "Unexpected error when waiting for response headers",
                 !SERF_BUCKET_READ_ERROR(status));
        if (SERF_BUCKET_READ_ERROR(status) ||
            APR_STATUS_IS_EOF(status))
            break;

        /* Check that the bytes read match with expected at current position. */
        CuAssertStrnEquals(tc, cur, len, data);
        cur += len;
    }

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(resp_bkt1);
}
#undef EXP_RESPONSE

/* Test that the internal function serf_default_read_iovec, used by many
   bucket types, groups multiple buffers in one iovec. */
static void test_copy_bucket(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    apr_status_t status;
    serf_bucket_t *bkt, *aggbkt, *copybkt;
    struct iovec tgt_vecs[2];
    int vecs_used, i;
    apr_size_t actual_len = 0;
    const char *data;
    apr_size_t len;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char *BODY = "12345678901234567890"\
                       "12345678901234567890"\
                       "12345678901234567890"\
                       CRLF;

    /* Test 1: multiple children, should be read in one iovec. */
    aggbkt = serf_bucket_aggregate_create(alloc);
    copybkt = serf_bucket_copy_create(aggbkt, 1024, alloc);

    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 20, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+20, 20, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY+40, strlen(BODY)-40, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    CuAssertIntEquals(tc, strlen(BODY),
                      (int)serf_bucket_get_remaining(aggbkt));
    CuAssertIntEquals(tc, strlen(BODY),
                      (int)serf_bucket_get_remaining(copybkt));

    /* When < min_size, everything should be read in one go */
    status = serf_bucket_read(copybkt, SERF_READ_ALL_AVAIL, &data, &len);
    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, strlen(BODY), len);

    serf_bucket_destroy(copybkt);

    /* Fill again aggregate bucket again. */
    aggbkt = serf_bucket_aggregate_create(alloc);
    copybkt = serf_bucket_copy_create(aggbkt, 35, alloc);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 20, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY + 20, 20, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY + 40, strlen(BODY) - 40, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    CuAssertIntEquals(tc, strlen(BODY),
                      (int)serf_bucket_get_remaining(copybkt));

    /* When, requesting more than min_size, but more than in the first chunk
       we will get min_size */
    status = serf_bucket_read(copybkt, SERF_READ_ALL_AVAIL, &data, &len);
    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 35, len);

    /* We can read the rest */
    CuAssertIntEquals(tc, APR_EOF, discard_data(copybkt, &len));
    CuAssertIntEquals(tc, strlen(BODY) - 35, len);

    serf_bucket_destroy(copybkt);

    /* Fill again aggregate bucket again. */
    aggbkt = serf_bucket_aggregate_create(alloc);
    copybkt = serf_bucket_copy_create(aggbkt, 45, alloc);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY, 20, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY + 20, 20, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING_LEN(BODY + 40, strlen(BODY) - 40, alloc);
    serf_bucket_aggregate_append(aggbkt, bkt);

    CuAssertIntEquals(tc, strlen(BODY),
                      (int)serf_bucket_get_remaining(copybkt));

    /* Now test if we can read everything as two vecs */
    status = serf_bucket_read_iovec(copybkt, SERF_READ_ALL_AVAIL,
                                    2, tgt_vecs, &vecs_used);

    CuAssertIntEquals(tc, APR_EOF, status);
    for (i = 0; i < vecs_used; i++)
        actual_len += tgt_vecs[i].iov_len;
    CuAssertIntEquals(tc, strlen(BODY), actual_len);

    serf_bucket_destroy(copybkt);
}


/* Test that serf doesn't hang in an endless loop when a linebuf is in
   split-CRLF state. */
static void test_linebuf_crlf_split(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *mock_bkt, *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    mockbkt_action actions[]= {
        { 1, "HTTP/1.1 200 OK" CRLF, APR_SUCCESS },
        { 1, "Content-Type: text/plain" CRLF
             "Transfer-Encoding: chunked" CRLF
             CRLF, APR_SUCCESS },
        { 1, "6" CR, APR_SUCCESS },
        { 1, "", APR_EAGAIN },
        { 1,  LF "blabla" CRLF "0" CRLF CRLF, APR_SUCCESS }, };
    apr_status_t status;

    const char *expected = "blabla";

    mock_bkt = serf_bucket_mock_create(actions, 5, alloc);
    bkt = serf_bucket_response_create(mock_bkt, alloc);

    do
    {
        const char *data;
        apr_size_t len;

        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &len);
        CuAssert(tc, "Got error during bucket reading.",
                 !SERF_BUCKET_READ_ERROR(status));
        CuAssert(tc, "Read more data than expected.",
                 strlen(expected) >= len);
        CuAssert(tc, "Read data is not equal to expected.",
                 strncmp(expected, data, len) == 0);

        expected += len;

        if (len == 0 && status == APR_EAGAIN)
            serf_bucket_mock_more_data_arrived(mock_bkt);
    } while(!APR_STATUS_IS_EOF(status));

    CuAssert(tc, "Read less data than expected.", strlen(expected) == 0);

    serf_bucket_destroy(bkt);
}

/* Test that the Content-Length header will be ignored when the response
   should not have returned a body. See RFC2616, section 4.4, nbr. 1. */
static void test_response_no_body_expected(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    char buf[1024];
    apr_size_t len;
    serf_bucket_alloc_t *alloc;
    int i;
    apr_status_t status;

    /* response bucket should consider the blablablablabla as start of the
       next response, in all these cases it should APR_EOF after the empty
       line. */
    const char *message_list[] = {
        "HTTP/1.1 100 Continue" CRLF
          "Content-Type: text/plain" CRLF
          "Content-Length: 6500000" CRLF
          CRLF
          "blablablablabla" CRLF,
        "HTTP/1.1 204 No Content" CRLF
          "Content-Type: text/plain" CRLF
          "Content-Length: 6500000" CRLF
          CRLF
          "blablablablabla" CRLF,
        "HTTP/1.1 304 Not Modified" CRLF
          "Content-Type: text/plain" CRLF
          "Content-Length: 6500000" CRLF
          CRLF
          "blablablablabla" CRLF,
        "HTTP/1.1 100 Continue" CRLF
          CRLF
        "HTTP/1.1 204 No Content" CRLF
          "Content-Type: text/plain" CRLF
          "Content-Length: 6500000" CRLF
          CRLF
          "blablablablabla" CRLF,
    };

    alloc = test__create_bucket_allocator(tc, tb->pool);

    /* Test 1: a response to a HEAD request. */
    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 200 OK" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Content-Length: 6500000" CRLF
                                    CRLF
                                    "blablablablabla" CRLF,
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);
    serf_bucket_response_set_head(bkt);

    status = read_all(bkt, buf, sizeof(buf), &len);

    CuAssertIntEquals(tc, APR_EOF, status);
    CuAssertIntEquals(tc, 0, len);

    DRAIN_BUCKET(tmp);
    serf_bucket_destroy(bkt);

    /* Test 2: a response with status for which server must not send a body. */
    for (i = 0; i < sizeof(message_list) / sizeof(const char*); i++) {

        tmp = SERF_BUCKET_SIMPLE_STRING(message_list[i], alloc);
        bkt = serf_bucket_response_create(tmp, alloc);

        status = read_all(bkt, buf, sizeof(buf), &len);

        if (i == 0) {
            /* blablablablabla is parsed as the next status line */
            CuAssertIntEquals(tc, SERF_ERROR_BAD_HTTP_RESPONSE, status);
            DRAIN_BUCKET(tmp);
        }
        else {
            CuAssertIntEquals(tc, APR_EOF, status);
            CuAssertIntEquals(tc, 0, len);

            read_and_check_bucket(tc, tmp, "blablablablabla" CRLF);
        }

        serf_bucket_destroy(bkt);
    }
}

/* Test handling IIS 'extended' status codes (like 401.1) by response
   buckets. */
static void test_response_bucket_iis_status_code(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_status_line sline;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 401.1 Logon failed." CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Content-Length: 2" CRLF
                                    CRLF
                                    "AB",
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    read_and_check_bucket(tc, bkt, "AB");

    serf_bucket_response_status(bkt, &sline);
    CuAssertTrue(tc, sline.version == SERF_HTTP_11);
    CuAssertIntEquals(tc, 401, sline.code);

    /* Probably better to have just "Logon failed" as reason. But current
       behavior is also acceptable.*/
    CuAssertStrEquals(tc, ".1 Logon failed.", sline.reason);

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

/* Test handling responses without a reason by response buckets. */
static void test_response_bucket_no_reason(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *tmp;
    serf_status_line sline;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    tmp = SERF_BUCKET_SIMPLE_STRING("HTTP/1.1 401" CRLF
                                    "Content-Type: text/plain" CRLF
                                    "Content-Length: 2" CRLF
                                    CRLF
                                    "AB",
                                    alloc);

    bkt = serf_bucket_response_create(tmp, alloc);

    read_and_check_bucket(tc, bkt, "AB");

    serf_bucket_response_status(bkt, &sline);
    CuAssertTrue(tc, sline.version == SERF_HTTP_11);
    CuAssertIntEquals(tc, 401, sline.code);

    /* Probably better to have just "Logon failed" as reason. But current
       behavior is also acceptable.*/
    CuAssertStrEquals(tc, "", sline.reason);

    /* This will also destroy response stream bucket. */
    serf_bucket_destroy(bkt);
}

/* Test that serf can handle lines that don't arrive completely in one go.
   It doesn't really run random, it tries inserting APR_EAGAIN in all possible
   places in the response message, only one currently. */
static void test_random_eagain_in_response(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    apr_pool_t *iter_pool;

#define BODY "12345678901234567890123456789012345678901234567890"\
             "12345678901234567890123456789012345678901234567890"

    const char *expected = apr_psprintf(tb->pool, "%s%s", BODY, BODY);
    const char *fullmsg = "HTTP/1.1 200 OK" CRLF
    "Date: Fri, 12 Jul 2013 15:13:52 GMT" CRLF
    "Server: Apache/2.2.17 (Unix) mod_ssl/2.2.17 OpenSSL/1.0.1e DAV/2 "
    "mod_wsgi/3.4 Python/2.7.3 SVN/1.7.10" CRLF
    "DAV: 1,2" CRLF
    "DAV: version-control,checkout,working-resource" CRLF
    "DAV: merge,baseline,activity,version-controlled-collection" CRLF
    "DAV: http://subversion.tigris.org/xmlns/dav/svn/depth" CRLF
    "DAV: http://subversion.tigris.org/xmlns/dav/svn/log-revprops" CRLF
    "DAV: http://subversion.tigris.org/xmlns/dav/svn/atomic-revprops" CRLF
    "DAV: http://subversion.tigris.org/xmlns/dav/svn/partial-replay" CRLF
    "DAV: http://subversion.tigris.org/xmlns/dav/svn/mergeinfo" CRLF
    "DAV: <http://apache.org/dav/propset/fs/1>" CRLF
    "MS-Author-Via: DAV" CRLF
    "Allow: OPTIONS,GET,HEAD,POST,DELETE,TRACE,PROPFIND,PROPPATCH,COPY,MOVE,"
    "LOCK,UNLOCK,CHECKOUT" CRLF
    "SVN-Youngest-Rev: 1502584" CRLF
    "SVN-Repository-UUID: 13f79535-47bb-0310-9956-ffa450edef68" CRLF
    "SVN-Repository-Root: /repos/asf" CRLF
    "SVN-Me-Resource: /repos/asf/!svn/me" CRLF
    "SVN-Rev-Root-Stub: /repos/asf/!svn/rvr" CRLF
    "SVN-Rev-Stub: /repos/asf/!svn/rev" CRLF
    "SVN-Txn-Root-Stub: /repos/asf/!svn/txr" CRLF
    "SVN-Txn-Stub: /repos/asf/!svn/txn" CRLF
    "SVN-VTxn-Root-Stub: /repos/asf/!svn/vtxr" CRLF
    "SVN-VTxn-Stub: /repos/asf/!svn/vtxn" CRLF
    "Vary: Accept-Encoding" CRLF
    "Content-Type: text/plain" CRLF
    "Content-Type: text/xml; charset=\"utf-8\"" CRLF
    "Transfer-Encoding: chunked" CRLF
    CRLF
    "64" CRLF
    BODY CRLF
    "64" CRLF
    BODY CRLF
    "0" CRLF
    CRLF;

    const long nr_of_tests = strlen(fullmsg);
    long i;

    mockbkt_action actions[]= {
        { 1, NULL, APR_EAGAIN },
        { 1, NULL, APR_EAGAIN },
    };

    apr_pool_create(&iter_pool, tb->pool);

    for (i = 0; i < nr_of_tests; i++) {
        serf_bucket_t *mock_bkt, *bkt;
        serf_bucket_alloc_t *alloc;
        const char *ptr = expected;
        const char *part1, *part2;
        apr_size_t cut;
        apr_status_t status;

        apr_pool_clear(iter_pool);

        alloc = test__create_bucket_allocator(tc, iter_pool);

        cut = i % strlen(fullmsg);
        part1 = apr_pstrndup(iter_pool, fullmsg, cut);
        part2 = apr_pstrdup(iter_pool, fullmsg + cut);

        actions[0].data = part1;
        actions[1].data = part2;

        mock_bkt = serf_bucket_mock_create(actions, 2, alloc);
        bkt = serf_bucket_response_create(mock_bkt, alloc);

        do
        {
            const char *data, *errmsg;
            apr_size_t len;

            status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &len);
            CuAssert(tc, "Got error during bucket reading.",
                     !SERF_BUCKET_READ_ERROR(status));
            errmsg = apr_psprintf(iter_pool,
                                  "Read more data than expected, EAGAIN"
                                  " inserted at pos: %" APR_SIZE_T_FMT
                                  ", remainder: \"%s\"",
                                  cut, fullmsg + cut);
            CuAssert(tc, errmsg, strlen(ptr) >= len);
            errmsg = apr_psprintf(iter_pool,
                                  "Read data is not equal to expected, EAGAIN"
                                  " inserted at pos: %" APR_SIZE_T_FMT
                                  ", remainder: \"%s\"",
                                  cut, fullmsg + cut);
            CuAssertStrnEquals_Msg(tc, errmsg, ptr, len, data);

            ptr += len;

            if (len == 0 && status == APR_EAGAIN)
                serf_bucket_mock_more_data_arrived(mock_bkt);
        } while(!APR_STATUS_IS_EOF(status));

        CuAssert(tc, "Read less data than expected.", strlen(ptr) == 0);

        serf_bucket_destroy(bkt);
    }
    apr_pool_destroy(iter_pool);
}
#undef BODY

static void test_response_continue(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *bkt, *headers;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char long_response[] =
        "HTTP/1.1 100 Continue" CRLF
        "H: 1" CRLF
        "Foo: Bar" CRLF
    CRLF
        "HTTP/1.1 109 Welcome to HTTP-9" CRLF
        "Connection: Upgrade" CRLF
        "H: 2" CRLF
        "Upgrade: h9c" CRLF
    CRLF
        "HTTP/9.0 200 OK" CRLF
        "Content-Type: text/plain" CRLF
        "Content-Length: 26" CRLF
        "H: 3" CRLF
    CRLF
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    CRLF
    CRLF;

    /* 1: First verify that we just read the body*/
    bkt = SERF_BUCKET_SIMPLE_STRING(long_response, alloc);
    bkt = serf_bucket_response_create(bkt, alloc);

    read_and_check_bucket(tc, bkt, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    serf_bucket_destroy(bkt);

    /* 2: Check the headers the normal way */
    bkt = SERF_BUCKET_SIMPLE_STRING(long_response, alloc);
    bkt = serf_bucket_response_create(bkt, alloc);

    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_headers(bkt));
    headers = serf_bucket_response_get_headers(bkt);
    /* Verify that we just have the final set */
    CuAssertStrEquals(tc, "3", serf_bucket_headers_get(headers, "H"));
    CuAssertStrEquals(tc, NULL, serf_bucket_headers_get(headers, "Foo"));
    read_and_check_bucket(tc, bkt, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    serf_bucket_destroy(bkt);

    /* 3: Fetch the separate headers */
    bkt = SERF_BUCKET_SIMPLE_STRING(long_response, alloc);
    bkt = serf_bucket_response_create(bkt, alloc);

    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_some_headers(bkt, FALSE));
    headers = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc, "1", serf_bucket_headers_get(headers, "H"));

    /* Again*/
    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_some_headers(bkt, FALSE));
    headers = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc, "1", serf_bucket_headers_get(headers, "H"));

    /* Now fetch second set */
    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_some_headers(bkt, TRUE));
    headers = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc, "2", serf_bucket_headers_get(headers, "H"));

    /* Now fetch final set */
    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_some_headers(bkt, TRUE));
    headers = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc, "3", serf_bucket_headers_get(headers, "H"));

    /* Fetch same again */
    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_some_headers(bkt, TRUE));
    headers = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc, "3", serf_bucket_headers_get(headers, "H"));

    CuAssertIntEquals(tc, APR_SUCCESS,
                     serf_bucket_response_wait_for_headers(bkt));

    headers = serf_bucket_response_get_headers(bkt);
    CuAssertStrEquals(tc, "3", serf_bucket_headers_get(headers, "H"));
    CuAssertStrEquals(tc, NULL, serf_bucket_headers_get(headers, "Foo"));
    read_and_check_bucket(tc, bkt, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    serf_bucket_destroy(bkt);
}


static void test_dechunk_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *mock_bkt, *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    mockbkt_action actions[]= {
        /* one chunk */
        { 1, "6" CRLF "blabla" CRLF, APR_SUCCESS },
        /* EAGAIN after first chunk */
        { 1, "6" CRLF "blabla" CRLF, APR_EAGAIN },
        { 1, "6" CRLF "blabla" CRLF, APR_SUCCESS },
        /* CRLF after body split */
        { 1, "6" CRLF "blabla" CR, APR_EAGAIN },
        { 1,  LF, APR_SUCCESS },
        /* CRLF before body split */
        { 1, "6" CR, APR_SUCCESS },
        { 1, "", APR_EAGAIN },
        { 1,  LF "blabla" CRLF, APR_SUCCESS },
        /* empty chunk */
        { 1, "", APR_SUCCESS },
        /* two chunks */
        { 1, "6" CRLF "blabla" CRLF "6" CRLF "blabla" CRLF, APR_SUCCESS },
        /* three chunks */
        { 1, "6" CRLF "blabla" CRLF "6" CRLF "blabla" CRLF
             "0" CRLF "" CRLF, APR_SUCCESS },
    };
    const int nr_of_actions = sizeof(actions) / sizeof(mockbkt_action);
    apr_status_t status;
    const char *body = "blabla";
    const char *expected_data = apr_psprintf(tb->pool, "%s%s%s%s%s%s%s%s%s",
                                             body, body, body, body, body,
                                             body, body, body, body);
    const char *expected = expected_data;

    mock_bkt = serf_bucket_mock_create(actions, nr_of_actions, alloc);
    bkt = serf_bucket_dechunk_create(mock_bkt, alloc);

    do
    {
        const char *data;
        apr_size_t len;

        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &len);
        CuAssert(tc, "Got error during bucket reading.",
                 !SERF_BUCKET_READ_ERROR(status));
        CuAssert(tc, "Read more data than expected.",
                 strlen(expected) >= len);
        CuAssert(tc, "Read data is not equal to expected.",
                 strncmp(expected, data, len) == 0);

        expected += len;

        if (len == 0 && status == APR_EAGAIN)
            serf_bucket_mock_more_data_arrived(mock_bkt);
    } while(!APR_STATUS_IS_EOF(status));

    CuAssert(tc, "Read less data than expected.", strlen(expected) == 0);

    serf_bucket_destroy(bkt);

    mock_bkt = serf_bucket_mock_create(actions, nr_of_actions, alloc);
    bkt = serf_bucket_dechunk_create(mock_bkt, alloc);
    expected = expected_data;
    do
    {
        const char *data;
        apr_size_t len;
        int found;

        status = serf_bucket_readline(bkt, SERF_NEWLINE_ANY, &found, &data,
                                      &len);

        CuAssert(tc, "Got error during bucket reading.",
                 !SERF_BUCKET_READ_ERROR(status));
        CuAssert(tc, "Read more data than expected.",
                 strlen(expected) >= len);
        CuAssert(tc, "Read data is not equal to expected.",
                 strncmp(expected, data, len) == 0);

        expected += len;

        CuAssertIntEquals(tc, SERF_NEWLINE_NONE, found);

        if (len == 0 && status == APR_EAGAIN)
            serf_bucket_mock_more_data_arrived(mock_bkt);
    }
    while (!APR_STATUS_IS_EOF(status));

    serf_bucket_destroy(bkt);
}

static apr_status_t deflate_compress(const char **data, apr_size_t *len,
                                     z_stream *zdestr,
                                     const char *orig, apr_size_t orig_len,
                                     int last,
                                     serf_bucket_alloc_t *alloc)
{
    int zerr;
    apr_size_t buf_size;
    void *write_buf;

    /* The largest buffer we should need is 0.1% larger than the
       uncompressed data, + 12 bytes. This info comes from zlib.h.
       buf_size = orig_len + (orig_len / 1000) + 12;
       Note: This isn't sufficient when using Z_NO_FLUSH and extremely compressed
       data. Use a buffer bigger than what we need. */
    buf_size = 100000;

    write_buf = serf_bucket_mem_alloc(alloc, buf_size);

    zdestr->next_in = (Bytef *)orig;  /* Casting away const! */
    zdestr->avail_in = (uInt)orig_len;

    zerr = Z_OK;
    zdestr->next_out = write_buf;
    zdestr->avail_out = (uInt)buf_size;

    while ((last && zerr != Z_STREAM_END) ||
           (!last && zdestr->avail_in > 0))
    {
        zerr = deflate(zdestr, last ? Z_FINISH : Z_NO_FLUSH);
        if (zerr < 0)
            return APR_EGENERAL;
    }

    *data = write_buf;
    *len = buf_size - zdestr->avail_out;

    if (!*len) {
        serf_bucket_mem_free(alloc, write_buf);
        *data = "";
    }

    return APR_SUCCESS;
}

/* Reads bucket until EOF found and compares read data with zero terminated
   string expected. The expected pattern is looped when necessary to match
   the number of expected bytes. Report all failures using CuTest. */
static void read_bucket_and_check_pattern(CuTest *tc, serf_bucket_t *bkt,
                                          const char *pattern,
                                          apr_size_t expected_len)
{
    apr_status_t status;
    const char *expected = NULL;
    const apr_size_t pattern_len = strlen(pattern);

    apr_size_t exp_rem = 0;
    apr_size_t actual_len = 0;

    do
    {
        const char *data;
        apr_size_t act_rem;

        status = serf_bucket_read(bkt, SERF_READ_ALL_AVAIL, &data, &act_rem);

        CuAssert(tc, "Got error during bucket reading.",
                 !SERF_BUCKET_READ_ERROR(status));

        actual_len += act_rem;

        while (act_rem > 0) {
            apr_size_t bytes_to_compare;

            if (exp_rem == 0) {
                /* Init pattern. Potentially again */
                expected = pattern;
                exp_rem = pattern_len;
            }

            bytes_to_compare = act_rem < exp_rem ? act_rem : exp_rem;
            CuAssert(tc, "Read data is not equal to expected.",
                     strncmp(expected, data, bytes_to_compare) == 0);
            data += bytes_to_compare;
            act_rem -= bytes_to_compare;

            expected += bytes_to_compare;
            exp_rem -= bytes_to_compare;
        }
    } while(!APR_STATUS_IS_EOF(status));

    CuAssertIntEquals_Msg(tc, "Read less data than expected.", 0, exp_rem);
    CuAssertIntEquals_Msg(tc, "Read less/more data than expected.", actual_len,
                          expected_len);
}

static void deflate_buckets(CuTest *tc, int nr_of_loops)
{
    const char *msg = "12345678901234567890123456789012345678901234567890";

    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc = tb->bkt_alloc;
    z_stream zdestr;
    int i;
    const char gzip_header[10] =
    { '\037', '\213', Z_DEFLATED, 0,
        0, 0, 0, 0, /* mtime */
        0, 0x03 /* Unix OS_CODE */
    };

    serf_bucket_t *aggbkt = serf_bucket_aggregate_create(alloc);
    serf_bucket_t *defbkt = serf_bucket_deflate_create(aggbkt, alloc,
                                                       SERF_DEFLATE_GZIP);
    serf_bucket_t *strbkt;

#if 0 /* Enable logging */
    {
        serf_config_t *config;

        serf_context_t *ctx = serf_context_create(pool);
        /* status = */ serf__config_store_get_config(ctx, NULL, &config, pool);

        serf_bucket_set_config(defbkt, config);
    }
#endif

    memset(&zdestr, 0, sizeof(z_stream));
    /* HTTP uses raw deflate format, so windows size => -15 */
    CuAssert(tc, "zlib init failed.",
             deflateInit2(&zdestr, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                          Z_DEFAULT_STRATEGY) == Z_OK);

    strbkt = SERF_BUCKET_SIMPLE_STRING_LEN(gzip_header, 10, alloc);
    serf_bucket_aggregate_append(aggbkt, strbkt);

    for (i = 0; i < nr_of_loops; i++) {
        const char *data = NULL;
        apr_size_t len = 0;

        if (i == nr_of_loops - 1) {
            CuAssertIntEquals(tc, APR_SUCCESS,
                              deflate_compress(&data, &len, &zdestr, msg,
                                               strlen(msg), 1, alloc));
        } else {
            CuAssertIntEquals(tc, APR_SUCCESS,
                              deflate_compress(&data, &len, &zdestr, msg,
                                               strlen(msg), 0, alloc));
        }

        if (len == 0)
            continue;

        strbkt = serf_bucket_simple_own_create(data, len, alloc);

        serf_bucket_aggregate_append(aggbkt, strbkt);
    }

    tb->user_baton_l = APR_EOF;
    read_bucket_and_check_pattern(tc, defbkt, msg, nr_of_loops * strlen(msg));

    /* Release a few MB of memory kept by zlib */
    CuAssertIntEquals(tc, Z_OK, deflateEnd(&zdestr));

    serf_bucket_destroy(defbkt);
}

static void test_deflate_buckets(CuTest *tc)
{
    int i;
    for (i = 1; i < 1000; i++) {
        deflate_buckets(tc, i);
    }
}

#ifdef SERF_TEST_DEFLATE_4GBPLUS_BUCKETS
static apr_status_t hold_open(void *baton, serf_bucket_t *aggbkt)
{
    test_baton_t *tb = baton;

    return tb->user_baton_l;
}

static void put_32bit(unsigned char *buf, unsigned long x)
{
    buf[0] = (unsigned char)(x & 0xFF);
    buf[1] = (unsigned char)((x & 0xFF00) >> 8);
    buf[2] = (unsigned char)((x & 0xFF0000) >> 16);
    buf[3] = (unsigned char)((x & 0xFF000000) >> 24);
}

static serf_bucket_t *
create_gzip_deflate_bucket(serf_bucket_t *stream, z_stream *outzstr,
                           serf_bucket_alloc_t *alloc)
{
    serf_bucket_t *strbkt;
    serf_bucket_t *defbkt = serf_bucket_deflate_create(stream, alloc,
                                                       SERF_DEFLATE_GZIP);
    int zerr;
    static const char gzip_header[10] =
    { '\037', '\213', Z_DEFLATED, 0,
        0, 0, 0, 0, /* mtime */
        0, 0x03 /* Unix OS_CODE */
    };

    memset(outzstr, 0, sizeof(z_stream));

    /* HTTP uses raw deflate format, so windows size => -15 */
    zerr = deflateInit2(outzstr, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                        Z_DEFAULT_STRATEGY);
    if (zerr != Z_OK)
        return NULL;

    strbkt = SERF_BUCKET_SIMPLE_STRING_LEN(gzip_header, 10, alloc);
    serf_bucket_aggregate_append(stream, strbkt);

    return defbkt;
}

/* Test for issue #152: the trailers of gzipped data only store the 4 most
   significant bytes of the length, so when the compressed data is >4GB
   we can't just compare actual length with expected length. */
static void test_deflate_4GBplus_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    int i;
    unsigned char gzip_trailer[8];
    z_stream zdestr;
    serf_bucket_t *aggbkt = serf_bucket_aggregate_create(alloc);
    serf_bucket_t *defbkt = create_gzip_deflate_bucket(aggbkt, &zdestr, alloc);
    serf_bucket_t *strbkt;
    apr_uint64_t actual_size;
    unsigned long unc_crc = 0;
    unsigned long unc_length = 0;

#define NR_OF_LOOPS 550000
#define BUFSIZE 8096
    unsigned char uncompressed[BUFSIZE];

    serf_bucket_aggregate_hold_open(aggbkt, hold_open, tb);
    tb->user_baton_l = APR_EAGAIN;


#if 0 /* Enable logging */
    {
        serf_config_t *config;

        serf_context_t *ctx = serf_context_create(tb->pool);
        /* status = */ serf__config_store_get_config(ctx, NULL, &config, tb->pool);

        serf_bucket_set_config(defbkt, config);
    }
#endif

    printf("\n");
    actual_size = 0;
    for (i = 0; i < NR_OF_LOOPS; i++) {
        const char *data;
        apr_size_t len;
        apr_size_t read_len;
        apr_status_t status;

        if (i % 1000 == 0) {
            printf("\rtest_deflate_4GBplus_buckets: %d of %d",
                   i, NR_OF_LOOPS);
            fflush(stdout);
        }

        status = apr_generate_random_bytes(uncompressed, BUFSIZE);
        CuAssertIntEquals(tc, APR_SUCCESS, status);

        unc_crc = crc32(unc_crc, (const Bytef *)uncompressed, BUFSIZE);
        unc_length += BUFSIZE;

        if (i == NR_OF_LOOPS - 1) {
            CuAssertIntEquals(tc, APR_SUCCESS,
                              deflate_compress(&data, &len, &zdestr,
                                               (const char *)uncompressed,
                                               BUFSIZE, 1, alloc));
        } else {
            CuAssertIntEquals(tc, APR_SUCCESS,
                              deflate_compress(&data, &len, &zdestr,
                                               (const char *)uncompressed,
                                               BUFSIZE, 0, alloc));
        }

        if (len == 0)
            continue;

        strbkt = serf_bucket_simple_own_create(data, len, alloc);
        serf_bucket_aggregate_append(aggbkt, strbkt);

        /* Start reading inflated data */
        status = discard_data(defbkt, &read_len);
        CuAssert(tc, "Got error during discarding of compressed data.",
                 !SERF_BUCKET_READ_ERROR(status));

        actual_size += read_len;
    }
    printf("\n");

    put_32bit(&gzip_trailer[0], unc_crc);
    put_32bit(&gzip_trailer[4], unc_length);
    strbkt = SERF_BUCKET_SIMPLE_STRING_LEN((const char *)gzip_trailer,
                                           sizeof(gzip_trailer), alloc);
    serf_bucket_aggregate_append(aggbkt, strbkt);

    tb->user_baton_l = APR_EOF;

    while (1) {
        apr_size_t read_len;
        apr_status_t status = discard_data(defbkt, &read_len);
        CuAssert(tc, "Got error during discarding of compressed data.",
                 !SERF_BUCKET_READ_ERROR(status));
        actual_size += read_len;
        if (status == APR_EOF)
            break;
    }

    {
        apr_uint64_t expected_size = (apr_uint64_t)NR_OF_LOOPS *
                                     (apr_uint64_t)BUFSIZE;
        CuAssertTrue(tc, actual_size == expected_size);
    }
#undef NR_OF_LOOPS
#undef BUFSIZE
}
#endif /* SERF_TEST_DEFLATE_4GBPLUS_BUCKETS */

/* Basic test for serf_linebuf_fetch(). */
static void test_linebuf_fetch_crlf(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    mockbkt_action actions[]= {
        { 1, "line1" CRLF, APR_SUCCESS },
        { 1, "line2" CR, APR_EAGAIN },
        { 1, "" LF, APR_SUCCESS },
        { 1, "" CRLF, APR_EOF},
    };
    serf_bucket_t *bkt;
    serf_linebuf_t linebuf;
    serf_bucket_type_t *unfriendly;

    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    bkt = serf_bucket_mock_create(actions, sizeof(actions)/sizeof(actions[0]),
                                  alloc);

    serf_linebuf_init(&linebuf);
    CuAssertStrEquals(tc, "", linebuf.line);
    CuAssertIntEquals(tc, 0, linebuf.used);
    CuAssertIntEquals(tc, SERF_LINEBUF_EMPTY, linebuf.state);

    CuAssertIntEquals(tc, APR_SUCCESS,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    /* We got first line in one call. */
    CuAssertIntEquals(tc, SERF_LINEBUF_READY, linebuf.state);
    CuAssertStrEquals(tc, "line1", linebuf.line);
    CuAssertIntEquals(tc, 5, linebuf.used);

    /* The second line CR and LF splitted across packets. */
    CuAssertIntEquals(tc, APR_EAGAIN,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    CuAssertIntEquals(tc, SERF_LINEBUF_CRLF_SPLIT, linebuf.state);

    CuAssertIntEquals(tc, APR_SUCCESS,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));

    CuAssertIntEquals(tc, SERF_LINEBUF_READY, linebuf.state);
    CuAssertIntEquals(tc, 5, linebuf.used);
    CuAssertStrnEquals(tc, "line2", 0, linebuf.line);

    /* Last line is empty. */
    CuAssertIntEquals(tc, APR_EOF,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    CuAssertIntEquals(tc, SERF_LINEBUF_READY, linebuf.state);
    CuAssertStrEquals(tc, "", linebuf.line);
    CuAssertIntEquals(tc, 0, linebuf.used);

    serf_bucket_destroy(bkt);

    /* And now try again with a less frienly peek implementation */
    bkt = serf_bucket_mock_create(actions, sizeof(actions) / sizeof(actions[0]),
                                  alloc);

    unfriendly = serf_bmemdup(alloc, bkt->type, sizeof(*bkt->type));
    unfriendly->peek = serf_default_peek; /* Unable to peek */
    bkt->type = unfriendly;

    serf_linebuf_init(&linebuf);
    CuAssertStrEquals(tc, "", linebuf.line);
    CuAssertIntEquals(tc, 0, linebuf.used);
    CuAssertIntEquals(tc, SERF_LINEBUF_EMPTY, linebuf.state);

    CuAssertIntEquals(tc, APR_SUCCESS,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    /* We got first line in one call. */
    CuAssertIntEquals(tc, SERF_LINEBUF_READY, linebuf.state);
    CuAssertStrEquals(tc, "line1", linebuf.line);
    CuAssertIntEquals(tc, 5, linebuf.used);

    /* The second line CR and LF splitted across packets. */
    CuAssertIntEquals(tc, APR_EAGAIN,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    CuAssertIntEquals(tc, SERF_LINEBUF_CRLF_SPLIT, linebuf.state);

    /* This test gets now stuck here, because EAGAIN will be returned forever */
    CuAssertIntEquals(tc, APR_EAGAIN,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    CuAssertIntEquals(tc, SERF_LINEBUF_CRLF_SPLIT, linebuf.state);

#if 0
    CuAssertIntEquals(tc, SERF_LINEBUF_READY, linebuf.state);
    CuAssertIntEquals(tc, 5, linebuf.used);
    CuAssertStrnEquals(tc, "line2", 0, linebuf.line);

    /* Last line is empty. */
    CuAssertIntEquals(tc, APR_EOF,
                      serf_linebuf_fetch(&linebuf, bkt, SERF_NEWLINE_CRLF));
    CuAssertIntEquals(tc, SERF_LINEBUF_READY, linebuf.state);
    CuAssertStrEquals(tc, "", linebuf.line);
    CuAssertIntEquals(tc, 0, linebuf.used);

    serf_bucket_destroy(bkt);
    serf_bucket_mem_free(alloc, unfriendly);
#endif
}

typedef struct prefix_cb
{
    serf_bucket_alloc_t *allocator;
    apr_size_t len;
    char *data;
} prefix_cb;

/* Implements serf_bucket_prefix_handler_t */
static apr_status_t prefix_callback(void *baton,
                                    serf_bucket_t *inner_bucket,
                                    const char *data,
                                    apr_size_t len)
{
    prefix_cb *pb = baton;

    pb->len = len;
    pb->data = serf_bstrmemdup(pb->allocator, data, len);
    return APR_SUCCESS;
}


static void test_prefix_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc;
    prefix_cb pb;
    serf_bucket_t *agg, *bkt, *prefix;
    const char *BODY = "12345678901234567890";
    const char *data;
    apr_size_t len;

    alloc = test__create_bucket_allocator(tc, tb->pool);
    pb.allocator = alloc;

    agg = serf_bucket_aggregate_create(alloc);

    /* First try reading less than first chunk */
    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(agg, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(agg, bkt);

    prefix = serf_bucket_prefix_create(agg, 15,
                                       prefix_callback, &pb, alloc);

    pb.data = NULL;
    read_and_check_bucket(tc, prefix, "6789012345678901234567890");
    CuAssertIntEquals(tc, 15, pb.len);
    CuAssertStrEquals(tc, "123456789012345", pb.data);

    serf_bucket_mem_free(alloc, pb.data);
    serf_bucket_destroy(prefix);

    /* Then more than first chunk*/
    agg = serf_bucket_aggregate_create(alloc);
    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(agg, bkt);
    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(agg, bkt);

    prefix = serf_bucket_prefix_create(agg, 25,
                                       prefix_callback, &pb, alloc);

    pb.data = NULL;
    read_and_check_bucket(tc, prefix, "678901234567890");
    CuAssertIntEquals(tc, 25, pb.len);
    CuAssertStrEquals(tc, "1234567890123456789012345", pb.data);

    serf_bucket_mem_free(alloc, pb.data);
    serf_bucket_destroy(prefix);

    /* And an early EOF */
    agg = serf_bucket_aggregate_create(alloc);
    bkt = SERF_BUCKET_SIMPLE_STRING(BODY, alloc);
    serf_bucket_aggregate_append(agg, bkt);

    prefix = serf_bucket_prefix_create(agg, 25,
                                       prefix_callback, &pb, alloc);

    CuAssertIntEquals(tc, APR_EOF,
                      serf_bucket_read(prefix, SERF_READ_ALL_AVAIL,
                                       &data, &len));
    CuAssertIntEquals(tc, 0, len);
    CuAssertIntEquals(tc, 20, pb.len);
    CuAssertStrEquals(tc, "12345678901234567890", pb.data);

    serf_bucket_mem_free(alloc, pb.data);
    serf_bucket_destroy(prefix);
}

static void test_limit_buckets(CuTest *tc)
{
  test_baton_t *tb = tc->testBaton;
  serf_bucket_alloc_t *alloc = tb->bkt_alloc;
  char buffer[26];
  apr_size_t len;
  serf_bucket_t *raw;
  serf_bucket_t *limit;
  apr_status_t status;

  /* The normal usecase */
  raw = SERF_BUCKET_SIMPLE_STRING("ABCDEFGHIJKLMNOPQRSTUVWXYZ", alloc);
  limit = serf_bucket_limit_create(raw, 13, alloc);
  read_and_check_bucket(tc, limit, "ABCDEFGHIJKLM");
  read_and_check_bucket(tc, raw, "NOPQRSTUVWXYZ");
  serf_bucket_destroy(limit);

  /* What if there is not enough data? */
  raw = SERF_BUCKET_SIMPLE_STRING("ABCDE", alloc);
  limit = serf_bucket_limit_create(raw, 13, alloc);

  status = read_all(limit, buffer, sizeof(buffer), &len);
  CuAssertIntEquals(tc, SERF_ERROR_TRUNCATED_STREAM, status);
  serf_bucket_destroy(limit);

  {
    const char *data;
    int found;

    raw = SERF_BUCKET_SIMPLE_STRING("ABCDEF\nGHIJKLMNOP", alloc);
    limit = serf_bucket_limit_create(raw, 5, alloc);

    CuAssertIntEquals(tc, APR_EOF,
                      serf_bucket_readline(limit, SERF_NEWLINE_ANY, &found,
                                           &data, &len));
    CuAssertIntEquals(tc, SERF_NEWLINE_NONE, found);
    CuAssertIntEquals(tc, len, 5); /* > 5 is over limit -> bug */
    DRAIN_BUCKET(raw);
    serf_bucket_destroy(limit);
  }
}

/* Implements serf_bucket_event_callback_t */
static apr_status_t update_total(void *baton,
                         apr_uint64_t bytes_read)
{
  apr_uint64_t *sum = baton;

  (*sum) += bytes_read;
  return APR_SUCCESS;
}

static void test_split_buckets(CuTest *tc)
{
  test_baton_t *tb = tc->testBaton;
  serf_bucket_alloc_t *alloc = tb->bkt_alloc;
  char buffer[128];
  apr_size_t len;
  serf_bucket_t *raw;
  serf_bucket_t *head, *tail;
  serf_bucket_t *agg;
  apr_status_t status;

  /* The normal usecase but different way */
  raw = SERF_BUCKET_SIMPLE_STRING("ABCDEFGHIJKLMNOPQRSTUVWXYZ", alloc);
  serf_bucket_split_create(&head, &tail, raw, 13, 13);
  agg = serf_bucket_aggregate_create(alloc);
  serf_bucket_aggregate_prepend(agg, head);
  serf_bucket_aggregate_append(agg,
                               serf_bucket_simple_create("!", 1, NULL, NULL,
                                                         alloc));
  serf_bucket_aggregate_append(agg, tail);
  read_and_check_bucket(tc, agg, "ABCDEFGHIJKLM!NOPQRSTUVWXYZ");
  serf_bucket_destroy(agg);

  /* What if there is not enough data? */
  raw = SERF_BUCKET_SIMPLE_STRING("ABCDE", alloc);
  serf_bucket_split_create(&head, &tail, raw, 13, 13);

  status = read_all(head, buffer, sizeof(buffer), &len);
  CuAssertIntEquals(tc, APR_EOF, status);
  CuAssertIntEquals(tc, len, 5);
  serf_bucket_destroy(head);
  serf_bucket_destroy(tail);

  /* And now a really bad case of the 'different way' */
  raw = SERF_BUCKET_SIMPLE_STRING("ABCDE", alloc);
  serf_bucket_split_create(&head, &tail, raw, 5, 5);
  agg = serf_bucket_aggregate_create(alloc);
  serf_bucket_aggregate_prepend(agg, head);
  serf_bucket_aggregate_append(agg, tail);

  {
    struct iovec vecs[12];
    int vecs_read;

    /* This used to trigger a problem via the aggregate bucket,
       as reading the last part destroyed the data pointed to by
       iovecs of the first */

    CuAssertIntEquals(tc, APR_SUCCESS,
                      serf_bucket_read_iovec(agg, SERF_READ_ALL_AVAIL,
                                             12, vecs, &vecs_read));

    serf__copy_iovec(buffer, &len, vecs, vecs_read);

    CuAssertIntEquals(tc, 5, len);

    CuAssertIntEquals(tc, APR_EOF,
                      serf_bucket_read_iovec(agg, SERF_READ_ALL_AVAIL,
                                             12, vecs, &vecs_read));
    CuAssertIntEquals(tc, 0, vecs_read);
  }
  serf_bucket_destroy(agg);

  {
    const char *data;
    int found;

    raw = SERF_BUCKET_SIMPLE_STRING("ABCDEF\nGHIJKLMNOP", alloc);
    serf_bucket_split_create(&head, &tail, raw, 5, 5);

    CuAssertIntEquals(tc, APR_EOF,
                      serf_bucket_readline(head, SERF_NEWLINE_ANY, &found,
                                           &data, &len));
    CuAssertIntEquals(tc, SERF_NEWLINE_NONE, found);
    CuAssertIntEquals(tc, len, 5); /* > 5 is over limit -> bug */
    DRAIN_BUCKET(head);
    DRAIN_BUCKET(tail);
    serf_bucket_destroy(head);
    serf_bucket_destroy(tail);
  }

  {
    const char *data;
    int i;
    apr_int64_t total1, total2;
    apr_size_t min_r, max_r;

    agg = serf_bucket_aggregate_create(alloc);

    /* Create a huge body of 173 times 59 chars (both primes) */
    for (i = 0; i < 173; i++)
      {
        serf_bucket_aggregate_append(agg,
          serf_bucket_simple_create(
            "12345678901234567890123456789012345678901234567890123", 53,
            NULL, NULL, alloc));
      }

    CuAssertIntEquals(tc, (173 * 53), (int)serf_bucket_get_remaining(agg));

    total1 = total2 = 0;
    min_r = APR_SIZE_MAX;
    max_r = 0;
    tail = agg;
    while ((APR_EOF != serf_bucket_peek(tail, &data, &len)) || len)
      {
        serf_bucket_split_create(&head, &tail, tail, 5, 17);

        head = serf__bucket_event_create(head, &total1, NULL, update_total,
                                         NULL, alloc);

        status = read_all(head, buffer, sizeof(buffer), &len);
        CuAssertIntEquals(tc, APR_EOF, status);
        total2 += len;

        serf_bucket_destroy(head);

        if (total1 < (173 * 53)) {
          CuAssertTrue(tc, (len >= 5) && (len <= 17));

          min_r = MIN(min_r, len);
          max_r = MAX(max_r, len);
        }
      }
    serf_bucket_destroy(tail);

    CuAssertTrue(tc, min_r < 10); /* There should be much smaller buckets */
    CuAssertIntEquals(tc, 17, max_r); /* First call should hit 17 */
    CuAssertIntEquals(tc, (173 * 53), (int)total1);
    CuAssertIntEquals(tc, (173 * 53), (int)total2);
  }
}

static void test_deflate_compress_buckets(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_alloc_t *alloc = tb->bkt_alloc;
    serf_bucket_t *bkt;
    int i;
    const char *body = "12345678901234567890" CRLF
                       "12345678901234567890" CRLF
                       "12345678901234567890" CRLF;

    for (i = SERF_DEFLATE_GZIP; i <= SERF_DEFLATE_DEFLATE; i++) {
        bkt = SERF_BUCKET_SIMPLE_STRING(body, alloc);
        bkt = serf_bucket_deflate_compress_create(bkt, 0, i, alloc);
        bkt = serf_bucket_deflate_create(bkt, alloc, i);

        read_and_check_bucket(tc, bkt, body);
        serf_bucket_destroy(bkt);
    }
}

/* Basic test for unframe buckets. */
static void test_http2_unframe_buckets(CuTest *tc)
{
  test_baton_t *tb = tc->testBaton;
  const char raw_frame1[] = "\x00\x00\x0c"      /* 12 bytes payload */
    "\x04\x00"          /* Settings frame, no flags*/
    "\x00\x00\x00\x00"  /* Stream 0 */

    "\x00\x01"          /* SETTINGS_HEADER_TABLE_SIZE */
    "\x00\x00\x00\x00"  /* Value: 0 */

    "\x00\x02"          /* 0x2: SETTINGS_ENABLE_PUSH */
    "\x00\x00\x00\x00"  /* Value: 0 */
    "";
  const char raw_frame2[] = "\x00\x00\x06"      /* 6 bytes payload */
    "\x01\x02"          /* Frame type 0x01, Flags 0x02 */
    "\x83\x04\x05\x06"  /* Stream 0x03040506. Highest (undefined) bit set */

    "\x00\x01"          /* SETTINGS_HEADER_TABLE_SIZE */
    "\x00\x00\x00\x00"  /* Value: 0 */
    "";
  serf_bucket_alloc_t *alloc;
  serf_bucket_t *raw;
  serf_bucket_t *unframe;
  char result1[12];
  char result2[6];
  apr_status_t status;
  apr_size_t read_len;

  alloc = test__create_bucket_allocator(tc, tb->pool);

  raw = serf_bucket_simple_create(raw_frame1, sizeof(raw_frame1) - 1,
                                  NULL, NULL, alloc);

  unframe = serf__bucket_http2_unframe_create(raw, SERF_READ_ALL_AVAIL, alloc);

  CuAssertTrue(tc, SERF__BUCKET_IS_HTTP2_UNFRAME(unframe));

  status = read_all(unframe, result1, sizeof(result1), &read_len);
  CuAssertIntEquals(tc, APR_EOF, status);
  CuAssertIntEquals(tc, sizeof(result1), read_len);

  CuAssertIntEquals(tc, 0, memcmp(result1, "\x00\x01\x00\x00\x00\x00"
                                  "\x00\x02\x00\x00\x00\x00", read_len));

  {
    apr_int32_t stream_id;
    unsigned char frame_type, flags;

    CuAssertIntEquals(tc, 0,
                      serf__bucket_http2_unframe_read_info(unframe,
                                                           &stream_id,
                                                           &frame_type,
                                                           &flags));
    CuAssertIntEquals(tc, 0, stream_id);
    CuAssertIntEquals(tc, 4, frame_type);
    CuAssertIntEquals(tc, 0, flags);
  }

  serf_bucket_destroy(unframe);
  /* http2_unframe() bucket doesn't destroy inner stream bucket. */
  serf_bucket_destroy(raw);

  raw = serf_bucket_simple_create(raw_frame2, sizeof(raw_frame2) - 1,
                                  NULL, NULL, alloc);

  unframe = serf__bucket_http2_unframe_create(raw, SERF_READ_ALL_AVAIL, alloc);

  status = read_all(unframe, result2, sizeof(result2), &read_len);
  CuAssertIntEquals(tc, APR_EOF, status);
  CuAssertIntEquals(tc, sizeof(result2), read_len);

  CuAssertIntEquals(tc, 0, memcmp(result2, "\x00\x01\x00\x00\x00\x00",
                                  read_len));

  {
    apr_int32_t stream_id;
    unsigned char frame_type, flags;

    CuAssertIntEquals(tc, 0,
                      serf__bucket_http2_unframe_read_info(unframe,
                                                           &stream_id,
                                                           &frame_type,
                                                           &flags));
    CuAssertIntEquals(tc, 0x03040506, stream_id);
    CuAssertIntEquals(tc, 0x01, frame_type);
    CuAssertIntEquals(tc, 0x02, flags);
  }

  serf_bucket_destroy(unframe);
  /* http2_unframe() bucket doesn't destroy inner stream bucket. */
  serf_bucket_destroy(raw);

  /* And now check the frame oversized error */
  raw = serf_bucket_simple_create(raw_frame2, sizeof(raw_frame2) - 1,
                                  NULL, NULL, alloc);

  unframe = serf__bucket_http2_unframe_create(raw, 5, alloc);

  status = read_all(unframe, result2, sizeof(result2), &read_len);
  CuAssertIntEquals(tc, SERF_ERROR_HTTP2_FRAME_SIZE_ERROR, status);

  serf_bucket_destroy(unframe);
  /* http2_unframe() bucket doesn't destroy inner stream bucket. */
  DRAIN_BUCKET(raw);
  serf_bucket_destroy(raw);
}

/* Basic test for unframe buckets. */
static void test_http2_unpad_buckets(CuTest *tc)
{
  test_baton_t *tb = tc->testBaton;
  const char raw_frame[] = "\x00\x00\x18"      /* 24 bytes payload */
                           "\x00\x08"          /* Data frame, padding flag */
                           "\x00\x00\x00\x07"  /* Stream 7 */

                           "\x07"              /* 7 bytes padding at end */

                           "\x01\x03\x05\x07\x09\x0B\x0D\x0F"  /* 16 bytes */
                           "\x00\x02\x04\x06\x08\x0A\x0C\x0E"

                           "\x00\x00\x00\x00\x00\x00\x00" /* 7 bytes padding*/

                           "Extra"
                           "";
  serf_bucket_alloc_t *alloc;
  serf_bucket_t *raw;
  serf_bucket_t *unframe;
  serf_bucket_t *unpad;
  char result1[16];
  apr_status_t status;
  apr_size_t read_len;

  alloc = test__create_bucket_allocator(tc, tb->pool);

  raw = serf_bucket_simple_create(raw_frame, sizeof(raw_frame)-1,
                                  NULL, NULL, alloc);

  unframe = serf__bucket_http2_unframe_create(raw, SERF_READ_ALL_AVAIL, alloc);

  {
    apr_int32_t streamid;
    unsigned char frame_type, flags;

    status = serf__bucket_http2_unframe_read_info(unframe, &streamid,
                                                  &frame_type, &flags);

    CuAssertIntEquals(tc, APR_SUCCESS, status);
    CuAssertIntEquals(tc, 7, streamid);
    CuAssertIntEquals(tc, 0, frame_type);
    CuAssertIntEquals(tc, 8, flags);
  }

  unpad = serf__bucket_http2_unpad_create(unframe, alloc);

  CuAssertTrue(tc, SERF__BUCKET_IS_HTTP2_UNPAD(unpad));

  status = read_all(unpad, result1, sizeof(result1), &read_len);
  CuAssertIntEquals(tc, APR_EOF, status);
  CuAssertIntEquals(tc, sizeof(result1), read_len);

  read_and_check_bucket(tc, raw, "Extra");

  /* This is also destroy UNFRAME bucket. */
  serf_bucket_destroy(unpad);

  /* http2_unframe() bucket doesn't destroy inner stream bucket. */
  serf_bucket_destroy(raw);

  raw = serf_bucket_simple_create("\0a", 2, NULL, NULL, alloc);
  unpad = serf__bucket_http2_unpad_create(raw, alloc);
  read_and_check_bucket(tc, unpad, "a");

  serf_bucket_destroy(unpad);

  raw = serf_bucket_simple_create("\5a", 2, NULL, NULL, alloc);
  unpad = serf__bucket_http2_unpad_create(raw, alloc);

  {
    const char *data;
    apr_size_t sz;

    CuAssertIntEquals(tc, SERF_ERROR_HTTP2_PROTOCOL_ERROR,
                      serf_bucket_read(unpad, SERF_READ_ALL_AVAIL,
                                       &data, &sz));
  }
  DRAIN_BUCKET(raw);
  serf_bucket_destroy(unpad);
}

static void test_hpack_huffman_decode(CuTest *tc)
{
  char result[64];
  apr_size_t len;
  const unsigned char pre1[] = "\xF1\xE3\xC2\xE5\xF2\x3A\x6B\xA0\xAB\x90\xF4"
    "\xFF";
  const unsigned char pre2[] = "\xA8\xEB\x10\x64\x9C\xBF";
  const unsigned char pre3[] = "\x25\xA8\x49\xE9\x5B\xA9\x7D\x7F";
  const unsigned char pre4[] = "\x25\xA8\x49\xE9\x5B\xB8\xE8\xB4\xBF";
  const unsigned char pre5[] = "\x64\x02";
  const unsigned char pre6[] = "\xAE\xC3\x77\x1A\x4B";
  const unsigned char pre7[] = "\xD0\x7A\xBE\x94\x10\x54\xD4\x44\xA8\x20\x05"
                               "\x95\x04\x0B\x81\x66\xE0\x82\xA6\x2D\x1B\xFF";
  const unsigned char pre8[] = "\x9D\x29\xAD\x17\x18\x63\xC7\x8F\x0B\x97\xC8"
                               "\xE9\xAE\x82\xAE\x43\xD3";
  const unsigned char pre9[] = "\x64\x0E\xFF";
  const unsigned char preA[] = "\xD0\x7A\xBE\x94\x10\x54\xD4\x44\xA8\x20\x05"
                               "\x95\x04\x0B\x81\x66\xE0\x84\xA6\x2D\x1B\xFF";
  const unsigned char preB[] = "\x9B\xD9\xAB";
  const unsigned char preC[] = "\x94\xE7\x82\x1D\xD7\xF2\xE6\xC7\xB3\x35\xDF"
                               "\xDF\xCD\x5B\x39\x60\xD5\xAF\x27\x08\x7F\x36"
                               "\x72\xC1\xAB\x27\x0F\xB5\x29\x1F\x95\x87\x31"
                               "\x60\x65\xC0\x03\xED\x4E\xE5\xB1\x06\x3D\x50"
                               "\x07";
  const unsigned char preD[] = "\0\0\0\0\0";

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre1, sizeof(pre1) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "www.example.com", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre2, sizeof(pre2) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "no-cache", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre3, sizeof(pre3) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "custom-key", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre4, sizeof(pre4) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "custom-value", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre5, sizeof(pre5) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "302", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre6, sizeof(pre6) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "private", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre7, sizeof(pre7) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "Mon, 21 Oct 2013 20:13:21 GMT", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre8, sizeof(pre8) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "https://www.example.com", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(pre9, sizeof(pre9) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "307", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(preA, sizeof(preA) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "Mon, 21 Oct 2013 20:13:22 GMT", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(preB, sizeof(preB) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "gzip", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(preC, sizeof(preC) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; "
                        "version=1", result);
  CuAssertIntEquals(tc, strlen(result), len);

  CuAssertIntEquals(tc, 0, serf__hpack_huffman_decode(preD, sizeof(preD) - 1,
                                                      sizeof(result), result,
                                                      &len));
  CuAssertStrEquals(tc, "00000000", result);
  CuAssertIntEquals(tc, strlen(result), len);

  /* And now check some corner cases as specified in the RFC:

     The remaining bits must be filled out with the prefix of EOS */
  CuAssertIntEquals(tc, 0,
                    serf__hpack_huffman_decode((const unsigned char*)"\x07", 1,
                                               sizeof(result), result,
                                               &len));
  CuAssertStrEquals(tc, "0", result);
  CuAssertIntEquals(tc, 1, len);

  CuAssertIntEquals(tc, APR_EINVAL,
                    serf__hpack_huffman_decode((const unsigned char*)"\x06", 1,
                                               sizeof(result),
                                               result, &len));
  CuAssertIntEquals(tc, APR_EINVAL,
                    serf__hpack_huffman_decode((const unsigned char*)"\x01", 1,
                                               sizeof(result),
                                               result, &len));

  /* EOS may not appear itself */
  CuAssertIntEquals(tc, APR_EINVAL,
                    serf__hpack_huffman_decode((const unsigned char*)
                                                    "\xFF\xFF\xFF\xFF", 4,
                                               sizeof(result), result, &len));
}

#define VERIFY_REVERSE(x)                                                  \
  do                                                                       \
   {                                                                       \
     const char *v = (x);                                                  \
     apr_size_t sz2;                                                       \
     CuAssertIntEquals(tc, 0,                                              \
                       serf__hpack_huffman_encode(v, strlen(v),            \
                                                  sizeof(encoded),         \
                                                  encoded, &encoded_len)); \
     CuAssertIntEquals(tc, 0,                                              \
                       serf__hpack_huffman_encode(v, strlen(v),            \
                                                  0, NULL, &sz2));         \
     CuAssertIntEquals(tc, encoded_len, sz2);                              \
     CuAssertIntEquals(tc, 0,                                              \
                       serf__hpack_huffman_decode(encoded, encoded_len,    \
                                                  sizeof(text), text,      \
                                                  &text_len));             \
     CuAssertStrEquals(tc, v, text);                                       \
     CuAssertIntEquals(tc, strlen(v), text_len);                           \
   }                                                                       \
  while(0)

static void test_hpack_huffman_encode(CuTest *tc)
{
  unsigned char encoded[1024];
  char text[1024];
  apr_size_t encoded_len;
  apr_size_t text_len;

  VERIFY_REVERSE("1234567890");
  VERIFY_REVERSE("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

  {
    char from[256];
    int i;

    for (i = 0; i < sizeof(from); i++)
      from[i] = (char)i;

    CuAssertIntEquals(tc, 0,
                      serf__hpack_huffman_encode(from, sizeof(from),
                                                 sizeof(encoded),
                                                 encoded, &encoded_len));
    /* Nice.. need 583 bytes to encode these 256 bytes :-) */
    CuAssertIntEquals(tc, 583, encoded_len);
    text[256] = 0xFE;
    CuAssertIntEquals(tc, 0,
                      serf__hpack_huffman_decode(encoded, encoded_len,
                                                 sizeof(text), text,
                                                 &text_len));
    CuAssertIntEquals(tc, 256, text_len);
    CuAssertIntEquals(tc, 0, memcmp(from, text, sizeof(from)));
    /* If there is space in the buffer serf__hpack_huffman_decode will add
       a final '\0' after the buffer */
    CuAssertIntEquals(tc, 0, text[256]);

    for (i = 0; i < sizeof(from); i++)
      from[i] = '0';

    CuAssertIntEquals(tc, 0,
                      serf__hpack_huffman_encode(from, sizeof(from),
                                                 sizeof(encoded),
                                                 encoded, &encoded_len));
    /* Ok, 160 to encode 256. Maybe there is some use case */
    CuAssertIntEquals(tc, 160, encoded_len);
    text[256] = 0xEF;
    CuAssertIntEquals(tc, 0,
                      serf__hpack_huffman_decode(encoded, encoded_len,
                                                 sizeof(text), text,
                                                 &text_len));
    CuAssertIntEquals(tc, 256, text_len);
    CuAssertIntEquals(tc, 0, memcmp(from, text, sizeof(from)));
    /* If there is space in the buffer serf__hpack_huffman_decode will add
    a final '\0' after the buffer */
    CuAssertIntEquals(tc, 0, text[256]);
  }
}
#undef VERIFY_REVERSE

static void test_hpack_header_encode(CuTest *tc)
{
  test_baton_t *tb = tc->testBaton;
  serf_bucket_alloc_t *alloc;
  serf_hpack_table_t *tbl;
  serf_bucket_t *hpack;

  char resultbuffer[1024];
  apr_size_t sz;

  alloc = test__create_bucket_allocator(tc, tb->pool);
  tbl = serf__hpack_table_create(TRUE, 16384, tb->pool);

  hpack = serf__bucket_hpack_create(tbl, alloc);

  CuAssertTrue(tc, SERF_BUCKET_IS_HPACK(hpack));

  serf__bucket_hpack_setc(hpack, ":method", "PUT");
  serf__bucket_hpack_setc(hpack, ":scheme", "https");
  serf__bucket_hpack_setc(hpack, ":path", "/");
  serf__bucket_hpack_setc(hpack, ":authority", "localhost");

  CuAssertIntEquals(tc, APR_EOF,
                    read_all(hpack, resultbuffer, sizeof(resultbuffer), &sz));

  /* CuAssertTrue(tc, ! SERF_BUCKET_IS_HPACK(hpack)); */
  CuAssertTrue(tc, sz > 4);
  CuAssertTrue(tc, sz <= 20); /* The all literal approach takes 59 bytes
                                 Current size (2015-11 is 15)*/

  serf_bucket_destroy(hpack);
}

static void test_http2_frame_bucket_basic(CuTest *tc)
{
  test_baton_t *tb = tc->testBaton;
  serf_bucket_alloc_t *alloc;
  serf_bucket_t *body_in;
  serf_bucket_t *frame_in;
  serf_bucket_t *frame_out;
  apr_int32_t exp_streamid = 0x01020304;

  alloc = test__create_bucket_allocator(tc, tb->pool);

  body_in = SERF_BUCKET_SIMPLE_STRING("This is no config!", alloc);
  frame_in = serf__bucket_http2_frame_create(body_in, 99, 7, &exp_streamid,
                                             NULL, NULL,
                                             16384, alloc);
  frame_out = serf__bucket_http2_unframe_create(frame_in, 16384, alloc);

  read_and_check_bucket(tc, frame_out, "This is no config!");

  {
    apr_int32_t streamid;
    unsigned char frametype;
    unsigned char flags;
    const char *buffer;
    apr_size_t sz;

    CuAssertIntEquals(tc, 0,
                      serf__bucket_http2_unframe_read_info(frame_out, &streamid,
                                                           &frametype, &flags));
    CuAssertIntEquals(tc, 0x01020304, streamid);
    CuAssertIntEquals(tc, 99, frametype);
    CuAssertIntEquals(tc, 7, flags);

    CuAssertIntEquals(tc, APR_EOF,
                      serf_bucket_read(frame_in, SERF_READ_ALL_AVAIL,
                                       &buffer, &sz));
    CuAssertIntEquals(tc, 0, sz);
  }

  /* http2 unframe bucket uses non-standard semantic and doesn't
   * destroy source stream bucket on desotry. */
  serf_bucket_destroy(frame_in);
  serf_bucket_destroy(frame_out);
}

static void test_brotli_decompress_bucket_basic(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    {
        const char input_data[] = {
            "\x3B"
        };

        input = serf_bucket_simple_create(input_data, sizeof(input_data) - 1,
                                          NULL, NULL, alloc);
        bkt = serf_bucket_brotli_decompress_create(input, alloc);
        read_and_check_bucket(tc, bkt, "");
        serf_bucket_destroy(bkt);
    }

    {
        const char input_data[] = {
            "\x8B\x03\x80\x61\x62\x63\x64\x65\x66\x67\x68\x03"
        };

        input = serf_bucket_simple_create(input_data, sizeof(input_data) - 1,
                                          NULL, NULL, alloc);
        bkt = serf_bucket_brotli_decompress_create(input, alloc);
        read_and_check_bucket(tc, bkt, "abcdefgh");
        serf_bucket_destroy(bkt);
    }

    {
        const char input_data[] = {
            "\x1B\x0E\x00\x00\x84\x71\xC0\xC6\xDA\x50\x22\x80\x88\x26"
            "\x81\x14\x35\x1F"
        };

        input = serf_bucket_simple_create(input_data, sizeof(input_data) - 1,
                                          NULL, NULL, alloc);
        bkt = serf_bucket_brotli_decompress_create(input, alloc);
        read_and_check_bucket(tc, bkt, "aaabbbcccdddeee");
        serf_bucket_destroy(bkt);
    }
}

static void test_brotli_decompress_bucket_truncated_input(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char input_data[] = {
        "\x8B\x03\x80\x61\x62\x63\x64\x65\x66\x67\x68\x03"
    };

    /* Truncate our otherwise valid input. */
    input = serf_bucket_simple_create(input_data, 5, NULL, NULL, alloc);
    bkt = serf_bucket_brotli_decompress_create(input, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);
        CuAssertIntEquals(tc, SERF_ERROR_DECOMPRESSION_FAILED, status);
    }

    serf_bucket_destroy(bkt);
}

static void test_brotli_decompress_bucket_read_bytewise(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    apr_status_t status;
    apr_size_t total_read = 0;
    /* Brotli-encoded sequence of 100,000 zeroes. */
    const char input_data[] = {
        "\x5B\x9F\x86\x01\x40\x02\x26\x1E\x0B\x24\xCB\x2F\x00"
    };

    input = serf_bucket_simple_create(input_data, sizeof(input_data) - 1,
                                      NULL, NULL, alloc);
    bkt = serf_bucket_brotli_decompress_create(input, alloc);

    do {
        const char *data;
        apr_size_t len;

        status = serf_bucket_read(bkt, 1, &data, &len);
        if (SERF_BUCKET_READ_ERROR(status))
            CuFail(tc, "Got error during bucket reading.");

        if (len > 1) {
            CuFail(tc, "Unexpected read with len > 1.");
        }
        else if (len == 1) {
            CuAssertIntEquals(tc, '0', data[0]);
            total_read += len;
        }
    } while (status != APR_EOF);

    CuAssertIntEquals(tc, 100000, (int)total_read);

    serf_bucket_destroy(bkt);
}

static void test_brotli_decompress_bucket_chunked_input(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    /* What if the encoded data spans over multiple chunks?
     * (And let's throw in a couple of empty chunks as well...) */
    input = serf_bucket_aggregate_create(alloc);
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("\x8B\x03", alloc));
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("\x80\x61\x62", alloc));
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("", alloc));
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("\x63\x64\x65\x66\x67\x68\x03", alloc));
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("", alloc));

    bkt = serf_bucket_brotli_decompress_create(input, alloc);
    read_and_check_bucket(tc, bkt, "abcdefgh");
    serf_bucket_destroy(bkt);
}

static void test_brotli_decompress_bucket_chunked_input2(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    /* Try an edge case where the valid encoded data (empty string) is
     * followed by an empty chunk. */
    input = serf_bucket_aggregate_create(alloc);
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("\x3B", alloc));
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING("", alloc));

    bkt = serf_bucket_brotli_decompress_create(input, alloc);
    read_and_check_bucket(tc, bkt, "");
    serf_bucket_destroy(bkt);
}

static void test_brotli_decompress_bucket_garbage_at_end(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);
    const char input_data[] = {
        "\x8B\x03\x80\x61\x62\x63\x64\x65\x66\x67\x68\x03garbage"
    };

    input = serf_bucket_simple_create(input_data, sizeof(input_data) - 1,
                                      NULL, NULL, alloc);
    bkt = serf_bucket_brotli_decompress_create(input, alloc);

    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);
        CuAssertIntEquals(tc, SERF_ERROR_DECOMPRESSION_FAILED, status);
    }
}

static void test_brotli_decompress_response_body(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    input = SERF_BUCKET_SIMPLE_STRING(
        "HTTP/1.1 200 OK" CRLF
        "Content-Type: text/html" CRLF
        "Content-Length: 12" CRLF
        "Content-Encoding: br" CRLF
        CRLF
        "\x8B\x03\x80\x61\x62\x63\x64\x65\x66\x67\x68\x03",
        alloc);

    bkt = serf_bucket_response_create(input, alloc);
    read_and_check_bucket(tc, bkt, "abcdefgh");
    serf_bucket_destroy(bkt);
}

static void test_deflate_bucket_truncated_data(CuTest *tc)
{
    test_baton_t *tb = tc->testBaton;
    serf_bucket_t *input;
    serf_bucket_t *bkt;
    serf_bucket_alloc_t *alloc = test__create_bucket_allocator(tc, tb->pool);

    /* This is a valid, but truncated gzip data (in two chunks). */
    input = serf_bucket_aggregate_create(alloc);
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING_LEN("\x1F\x8B\x08\x00\x00", 5, alloc));
    serf_bucket_aggregate_append(input,
        SERF_BUCKET_SIMPLE_STRING_LEN("\x00\x00\x00\x00\x03", 5, alloc));

    bkt = serf_bucket_deflate_create(input, alloc, SERF_DEFLATE_GZIP);
    {
        char buf[1024];
        apr_size_t len;
        apr_status_t status;

        status = read_all(bkt, buf, sizeof(buf), &len);
        CuAssertIntEquals(tc, SERF_ERROR_DECOMPRESSION_FAILED, status);
    }

    serf_bucket_destroy(bkt);
}

CuSuite *test_buckets(void)
{
    CuSuite *suite = CuSuiteNew();

    CuSuiteSetSetupTeardownCallbacks(suite, test_setup, test_teardown);

    SUITE_ADD_TEST(suite, test_simple_bucket_readline);
    SUITE_ADD_TEST(suite, test_response_bucket_read);
    SUITE_ADD_TEST(suite, test_response_bucket_headers);
    SUITE_ADD_TEST(suite, test_response_bucket_chunked_read);
    SUITE_ADD_TEST(suite, test_response_body_too_small_cl);
    SUITE_ADD_TEST(suite, test_response_body_too_small_chunked);
    SUITE_ADD_TEST(suite, test_response_body_chunked_no_crlf);
    SUITE_ADD_TEST(suite, test_response_body_chunked_incomplete_crlf);
    SUITE_ADD_TEST(suite, test_response_body_chunked_gzip_small);
    SUITE_ADD_TEST(suite, test_response_body_chunked_bogus_crlf);
    SUITE_ADD_TEST(suite, test_response_body_chunked_invalid_len);
    SUITE_ADD_TEST(suite, test_response_body_chunked_overflow_len);
    SUITE_ADD_TEST(suite, test_response_bucket_peek_at_headers);
    SUITE_ADD_TEST(suite, test_response_bucket_iis_status_code);
    SUITE_ADD_TEST(suite, test_response_bucket_no_reason);
    SUITE_ADD_TEST(suite, test_response_continue);
    SUITE_ADD_TEST(suite, test_bucket_header_set);
    SUITE_ADD_TEST(suite, test_bucket_header_do);
    SUITE_ADD_TEST(suite, test_iovec_buckets);
    SUITE_ADD_TEST(suite, test_aggregate_buckets);
    SUITE_ADD_TEST(suite, test_aggregate_bucket_readline);
    SUITE_ADD_TEST(suite, test_header_buckets);
    SUITE_ADD_TEST(suite, test_copy_bucket);
    SUITE_ADD_TEST(suite, test_linebuf_crlf_split);
    SUITE_ADD_TEST(suite, test_response_no_body_expected);
    SUITE_ADD_TEST(suite, test_random_eagain_in_response);
    SUITE_ADD_TEST(suite, test_linebuf_fetch_crlf);
    SUITE_ADD_TEST(suite, test_dechunk_buckets);
    SUITE_ADD_TEST(suite, test_deflate_buckets);
    SUITE_ADD_TEST(suite, test_prefix_buckets);
    SUITE_ADD_TEST(suite, test_limit_buckets);
    SUITE_ADD_TEST(suite, test_split_buckets);
    SUITE_ADD_TEST(suite, test_deflate_compress_buckets);
    SUITE_ADD_TEST(suite, test_http2_unframe_buckets);
    SUITE_ADD_TEST(suite, test_http2_unpad_buckets);
    SUITE_ADD_TEST(suite, test_hpack_huffman_decode);
    SUITE_ADD_TEST(suite, test_hpack_huffman_encode);
    SUITE_ADD_TEST(suite, test_hpack_header_encode);
    SUITE_ADD_TEST(suite, test_http2_frame_bucket_basic);
    if (serf_bucket_is_brotli_supported()) {
        SUITE_ADD_TEST(suite, test_brotli_decompress_bucket_basic);
        SUITE_ADD_TEST(suite, test_brotli_decompress_bucket_truncated_input);
        SUITE_ADD_TEST(suite, test_brotli_decompress_bucket_read_bytewise);
        SUITE_ADD_TEST(suite, test_brotli_decompress_bucket_chunked_input);
        SUITE_ADD_TEST(suite, test_brotli_decompress_bucket_chunked_input2);
        SUITE_ADD_TEST(suite, test_brotli_decompress_bucket_garbage_at_end);
        SUITE_ADD_TEST(suite, test_brotli_decompress_response_body);
    }
#ifdef SERF_TEST_DEFLATE_4GBPLUS_BUCKETS
    /* This test for issue #152 takes a lot of time generating 4GB+ of random
       data so it's disabled by default. */
    SUITE_ADD_TEST(suite, test_deflate_4GBplus_buckets);
#endif
    SUITE_ADD_TEST(suite, test_deflate_bucket_truncated_data);



    return suite;
}
