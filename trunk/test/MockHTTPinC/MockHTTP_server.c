/* Copyright 2014 Lieven Govaerts
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

/* This file includes code originally submitted to the Serf project, covered by
 * the Apache License, Version 2.0, copyright Justin Erenkrantz & Greg Stein.
 */
#include <apr_thread_proc.h>
#include <apr_strings.h>

#include <stdlib.h>

#include "MockHTTP.h"
#include "MockHTTP_private.h"

/* Copied from serf.  */
#if defined(APR_VERSION_AT_LEAST) && defined(WIN32)
#if APR_VERSION_AT_LEAST(1,4,0)
#define BROKEN_WSAPOLL
#endif
#endif

#define BUFSIZE 32768
struct _mhClientCtx_t {
    apr_pool_t *pool;
    apr_socket_t *skt;
    char buf[BUFSIZE];
    apr_size_t buflen;
    apr_size_t bufrem;
    mhRequest_t *req;
    apr_int16_t reqevents;
    char *respBody;
    apr_size_t respRem;
    apr_array_header_t *respQueue;  /*  test will queue a response */
    mhResponse_t *currResp; /* response in progress */
    bool closeConn;
};

static apr_status_t setupTCPServer(mhServCtx_t *ctx, bool blocking);

static void * APR_THREAD_FUNC start_thread(apr_thread_t *tid, void *baton)
{
    mhServCtx_t *ctx = baton;

    setupTCPServer(ctx, YES);

    while (1) {
        _mhRunServerLoop(ctx);
    }

    return NULL;
}

static apr_status_t cleanupServer(void *baton)
{
    mhServCtx_t *ctx = baton;
    apr_status_t status;

    /*    apr_thread_exit(tid, APR_SUCCESS);*/
    if (ctx->pollset)
        apr_pollset_destroy(ctx->pollset);
    if (ctx->skt)
        STATUSERR(apr_socket_close(ctx->skt));

    ctx->skt = NULL;
    ctx->pollset = NULL;

    return APR_SUCCESS;
}

static apr_status_t setupTCPServer(mhServCtx_t *ctx, bool blocking)
{
    apr_sockaddr_t *serv_addr;
    apr_pool_t *pool = ctx->pool;
    apr_status_t status;

    STATUSERR(apr_sockaddr_info_get(&serv_addr, ctx->hostname,
                                    APR_UNSPEC, ctx->port, 0,
                                    pool));

    /* Create server socket */
    /* Note: this call requires APR v1.0.0 or higher */
    STATUSERR(apr_socket_create(&ctx->skt, serv_addr->family,
                                SOCK_STREAM, 0, pool));

    STATUSERR(apr_socket_opt_set(ctx->skt, APR_SO_NONBLOCK, 1));
    STATUSERR(apr_socket_timeout_set(ctx->skt, 0));
    STATUSERR(apr_socket_opt_set(ctx->skt, APR_SO_REUSEADDR, 1));

    /* TODO: try the next port until bind succeeds */
    STATUSERR(apr_socket_bind(ctx->skt, serv_addr));

    /* Listen for clients */
    STATUSERR(apr_socket_listen(ctx->skt, SOMAXCONN));

    /* Create a new pollset, avoid broken WSAPoll implemenation on Windows. */
#ifdef BROKEN_WSAPOLL
    STATUSERR(apr_pollset_create_ex(&ctx->pollset, 32, pool, 0,
                                    APR_POLLSET_SELECT));
#else
    STATUSERR(apr_pollset_create(&ctx->pollset, 32, pool, 0));
#endif

    {
        apr_pollfd_t pfd = { 0 };

        pfd.desc_type = APR_POLL_SOCKET;
        pfd.desc.s = ctx->skt;
        pfd.reqevents = APR_POLLIN;

        STATUSERR(apr_pollset_add(ctx->pollset, &pfd));
    }

    return APR_SUCCESS;
}

mhServCtx_t *
_mhInitTestServer(const MockHTTP *mh, const char *hostname, apr_port_t port)
{
    apr_pool_t *pool = mh->pool;

    mhServCtx_t *ctx = apr_pcalloc(pool, sizeof(mhServCtx_t));
    ctx->pool = pool;
    ctx->mh = mh;
    ctx->hostname = apr_pstrdup(pool, hostname);
    ctx->port = port;
    ctx->reqQueue = mh->reqQueue;

    apr_pool_cleanup_register(pool, ctx,
                              cleanupServer,
                              apr_pool_cleanup_null);

    return ctx;
}

mhError_t _mhStartServer(mhServCtx_t *ctx)
{
    apr_thread_t *thread;

    /* TODO: second thread doesn't work. */
    if (0) { /* second thread */
        /* Setup a non-blocking TCP server in a separate thread */
        apr_thread_create(&thread, NULL, start_thread, ctx, ctx->pool);
    } else {
        apr_status_t status;
        /* Setup a non-blocking TCP server */
        status = setupTCPServer(ctx, NO);
        if (status)
            return MOCKHTTP_SETUP_FAILED;
    }

    return MOCKHTTP_NO_ERROR;
}

/******************************************************************************/
/* Parse a request structure from incoming data                               */
/******************************************************************************/

/* *len will be non-0 if a line ending with CRLF was found. buf will be copied 
   in mem allocatod from cctx->pool, cctx->buf ptrs will be moved. */
static void readLine(_mhClientCtx_t *cctx, const char **buf, apr_size_t *len)
{
    const char *ptr = cctx->buf;

    *len = 0;
    while (*ptr && ptr - cctx->buf < cctx->buflen) {
        if (*ptr == '\r' && *(ptr+1) == '\n') {
            *len = ptr - cctx->buf + 2;
            *buf = apr_pstrndup(cctx->pool, cctx->buf, *len);

            cctx->buflen -= *len; /* eat line */
            cctx->bufrem += *len;
            memcpy(cctx->buf, cctx->buf + *len, cctx->buflen);

            break;
        }
        ptr++;
    }
}

/* APR_EAGAIN if no line ready, APR_SUCCESS + done = YES if request line parsed */
static apr_status_t readReqLine(_mhClientCtx_t *cctx, mhRequest_t *req, bool *done)
{
    const char *start, *ptr, *version;
    const char *buf;
    apr_size_t len;

    *done = FALSE;

    readLine(cctx, &buf, &len);
    if (!len) return APR_EAGAIN;

    /* TODO: add checks for incomplete request line */
    start = ptr = buf;
    while (*ptr != ' ' && *ptr != '\r') ptr++;
    req->method = apr_pstrndup(cctx->pool, start, ptr-start);

    ptr++; start = ptr;
    while (*ptr != ' ' && *ptr != '\r') ptr++;
    req->url = apr_pstrndup(cctx->pool, start, ptr-start);

    ptr++; start = ptr;
    while (*ptr != ' ' && *ptr != '\r') ptr++;
    version = apr_pstrndup(cctx->pool, start, ptr-start);
    req->version = (version[5] - '0') * 100 +
    version[7] - '0';

    *done = TRUE;

    return APR_SUCCESS;
}

/* APR_EAGAIN if no line ready, APR_SUCCESS + done = YES when LAST header was
   parsed */
static apr_status_t readHeader(_mhClientCtx_t *cctx, mhRequest_t *req, bool *done)
{
    const char *buf;
    apr_size_t len;

    *done = NO;

    readLine(cctx, &buf, &len);
    if (!len) return APR_EAGAIN;

    if (len == 2 && *buf == '\r' && *(buf+1) == '\n') {
        *done = YES;
        return APR_SUCCESS;
    } else {
        const char *start = buf, *ptr = buf;
        const char *hdr, *val;
        while (*ptr != ':' && *ptr != '\r') ptr++;
        hdr = apr_pstrndup(cctx->pool, start, ptr-start);

        ptr++; while (*ptr == ' ') ptr++; start = ptr;
        while (*ptr != '\r') ptr++;
        val = apr_pstrndup(cctx->pool, start, ptr-start);

        setHeader(cctx->pool, cctx->req->hdrs, hdr, val);
    }
    return APR_SUCCESS;
}

/* APR_EAGAIN if not all data is ready, APR_SUCCESS + done = YES if body
   completely received. */
static apr_status_t readBody(_mhClientCtx_t *cctx, mhRequest_t *req, bool *done)
{
    const char *clstr;
    long cl;
    apr_size_t len;

    req->chunked = NO;

    clstr = getHeader(cctx->pool, cctx->req->hdrs, "Content-Length");
    cl = atol(clstr);
    if (cctx->req->body == NULL) {
        cctx->req->body = apr_palloc(cctx->pool, cl + 1);
    }

    len = (cctx->buflen < (cl - cctx->req->bodyLen)) ?
                    cctx->buflen : /* partial body */
                    cl;            /* full body */
    memcpy(cctx->req->body + cctx->req->bodyLen, cctx->buf, len);
    cctx->req->bodyLen += len;
    *(cctx->req->body + cctx->req->bodyLen) = '\0';

    cctx->buflen -= len; /* eat body */
    cctx->bufrem += len;
    memcpy(cctx->buf, cctx->buf + len, cctx->buflen);
    if (cctx->req->bodyLen < cl)
        return APR_EAGAIN;

    *done = YES;
    return APR_SUCCESS;
}

static apr_status_t readChunk(_mhClientCtx_t *cctx, mhRequest_t *req, bool *done)
{
    const char *buf;
    apr_size_t len, chlen;

    *done = NO;

    /* TODO: state2 for partial chunks **/
    readLine(cctx, &buf, &len);
    if (!len) return APR_EAGAIN;
    
    chlen = apr_strtoi64(buf, NULL, 16); /* read hex chunked length */

    if (chlen) {
        char *chunk;

        if (cctx->req->chunks == NULL) {
            cctx->req->chunks = apr_array_make(cctx->pool, 5,
                                               sizeof(const char *));
        }
        chunk = apr_palloc(cctx->pool, chlen + 1);
        len = (cctx->buflen < (chlen - cctx->req->bodyLen)) ?
                        cctx->buflen : /* partial chunk */
                        chlen;         /* full chunk */
        memcpy(chunk, cctx->buf, len);
        *(chunk + len) = '\0';

        *((const char **)apr_array_push(cctx->req->chunks)) = chunk;

        cctx->buflen -= len; /* eat chunk */
        cctx->bufrem += len;
        memcpy(cctx->buf, cctx->buf + len, cctx->buflen);

        if (len < chlen) /* TODO: fix */
            return APR_EAGAIN;
    }

    readLine(cctx, &buf, &len);
    if (len < 2) return APR_EAGAIN;

    if (len == 2 && *buf == '\r' && *(buf+1) == '\n') {
        if (chlen == 0) /* body ends with chunk of length 0 */
            *done = YES;
        return APR_SUCCESS;
    } else {
        return APR_EGENERAL; /* TODO: error code */
    }

    return APR_SUCCESS;
}

static apr_status_t readChunked(_mhClientCtx_t *cctx, mhRequest_t *req, bool *done)
{
    apr_status_t status;

    *done = NO;
    req->chunked = YES;

    while (*done == NO)
        STATUSERR(readChunk(cctx, req, done));

    return status;
}

/* New request data was made available, read status line/hdrs/body (chunks) */
static apr_status_t processData(_mhClientCtx_t *cctx, mhRequest_t **preq)
{
    bool done;
    mhRequest_t *req = *preq;
    apr_status_t status = APR_SUCCESS;

    if (cctx->buflen == 0)
        return APR_EAGAIN; /* more data needed */

    if (req == NULL) {
        req = *preq = apr_pcalloc(cctx->pool, sizeof(mhRequest_t));
        req->hdrs = apr_hash_make(cctx->pool);
    }

    done = NO;
    switch(cctx->req->readState) {
        case 0: /* status line */
            STATUSREADERR(readReqLine(cctx, req, &done));
            break;
        case 1: /* headers */
            STATUSREADERR(readHeader(cctx, req, &done));
            break;
        case 2: /* body */
        {
            const char *clstr, *chstr;
            chstr = getHeader(cctx->pool, req->hdrs,
                              "Transfer-Encoding");
            /* TODO: chunked can be one of more encodings */
            /* Read Transfer-Encoding first, ignore C-L when T-E is set */
            if (chstr && apr_strnatcasecmp(chstr, "chunked") == 0) {
                STATUSREADERR(readChunked(cctx, req, &done));
            } else {
                clstr = getHeader(cctx->pool, req->hdrs,
                                  "Content-Length");
                if (clstr)
                    STATUSREADERR(readBody(cctx, req, &done));
            }
            break;
        }
        case 3: /* finished */
            _mhLog(MH_VERBOSE, __FILE__, "Server received request: %s %s\n",
                   req->method, req->url);
            status = APR_EOF;
            break;
    }
    if (done) req->readState++;

/*    printf("buflen: %ld\n", cctx->buflen);*/

    if (cctx->buflen == 0)
        return APR_EOF;

    return status;
}

static apr_status_t readRequest(_mhClientCtx_t *cctx, mhRequest_t **preq)
{
    apr_status_t status;
    apr_size_t len;

    len = cctx->bufrem;
    if (len == 0) return APR_EGENERAL; /* should clear buffer */

    STATUSREADERR(apr_socket_recv(cctx->skt, cctx->buf + cctx->buflen, &len));
    if (len) {
        _mhLog(MH_VERBOSE, __FILE__, "recvd with status %d:\n%.*s\n---- %d ----\n",
               status, (unsigned int)len, cctx->buf + cctx->buflen,
               (unsigned int)len);

        cctx->buflen += len;
        cctx->bufrem -= len;
    } else {
        status = APR_EAGAIN;
    }

    while (cctx->buflen) {
        STATUSERR(processData(cctx, preq));
    }

    return status;
}

static const char *codeToString(unsigned int code)
{
    switch(code) {
        case 100: return "Continue"; break;
        case 101: return "Switching Protocols"; break;
        case 200: return "OK"; break;
        case 201: return "Created"; break;
        case 202: return "Accepted"; break;
        case 203: return "Non-Authoritative Information"; break;
        case 204: return "No Content"; break;
        case 205: return "Reset Content"; break;
        case 206: return "Partial Content"; break;
        case 300: return "Multiple Choices"; break;
        case 301: return "Moved Permanently"; break;
        case 302: return "Found"; break;
        case 303: return "See Other"; break;
        case 304: return "Not Modified"; break;
        case 305: return "Use Proxy"; break;
        case 307: return "Temporary Redirect"; break;
        case 400: return "Bad Request"; break;
        case 401: return "Unauthorized"; break;
        case 402: return "Payment Required"; break;
        case 403: return "Forbidden"; break;
        case 404: return "Not Found"; break;
        case 405: return "Method Not Allowed"; break;
        case 406: return "Not Acceptable"; break;
        case 407: return "Proxy Authentication Required"; break;
        case 408: return "Request Timeout"; break;
        case 409: return "Conflict"; break;
        case 410: return "Gone"; break;
        case 411: return "Length Required"; break;
        case 412: return "Precondition Failed"; break;
        case 413: return "Request Entity Too Large"; break;
        case 414: return "Request-URI Too Long"; break;
        case 415: return "Unsupported Media Type"; break;
        case 416: return "Requested Range Not Satisfiable"; break;
        case 417: return "Expectation Failed"; break;
        case 500: return "Internal Server Error"; break;
        case 501: return "Not Implemented"; break;
        case 502: return "Bad Gateway"; break;
        case 503: return "Service Unavailable"; break;
        case 504: return "Gateway Timeout"; break;
        case 505: return "HTTP Version Not Supported"; break;
        default: return "<not defined>";
    }
}

/******************************************************************************/
/* Send a response                                                            */
/******************************************************************************/

static char *respToString(apr_pool_t *pool, mhResponse_t *resp)
{
    char *str;
    apr_hash_index_t *hi;
    void *val;
    const void *key;
    apr_ssize_t klen;

    /* status line */
    str = apr_psprintf(pool, "HTTP/1.1 %d %s\r\n", resp->code,
                       codeToString(resp->code));

    if (resp->chunked == YES) {
        /* TODO: add to existing header */
        apr_hash_set(resp->hdrs, "Transfer-Encoding", APR_HASH_KEY_STRING,
                     "chunked");
    } else {
        apr_hash_set(resp->hdrs, "Content-Length", APR_HASH_KEY_STRING,
                     apr_itoa(pool, strlen(resp->body)));
    }

    for (hi = apr_hash_first(pool, resp->hdrs); hi; hi = apr_hash_next(hi)) {
        apr_hash_this(hi, &key, &klen, &val);

        str = apr_psprintf(pool, "%s%s: %s\r\n", str,
                                 (const char *) key, (const char *)val);
    }
    str = apr_psprintf(pool, "%s\r\n", str);

    if (resp->chunked == NO) {
        str = apr_psprintf(pool, "%s%s", str, resp->body);
    } else {
        int i;
        apr_size_t len;
        for (i = 0 ; i < resp->chunks->nelts; i++) {
            const char *chunk;

            chunk = APR_ARRAY_IDX(resp->chunks, i, const char *);
            len = strlen(chunk);
            str = apr_psprintf(pool, "%s%" APR_UINT64_T_HEX_FMT "\r\n%s\r\n",
                               str, (apr_uint64_t)len, chunk);
        }
        if (len != 0) /* Add 0 chunk only if last chunk wasn't empty already */
            str = apr_psprintf(pool, "%s0\r\n\r\n", str);
    }
    return str;
}

static apr_status_t writeResponse(_mhClientCtx_t *cctx, mhResponse_t *resp)
{
    apr_pool_t *pool = cctx->pool;
    apr_size_t len;
    apr_status_t status;

    if (!cctx->respRem) {
        _mhResponseBuild(resp);
        cctx->respBody = respToString(pool, resp);
        cctx->respRem = strlen(cctx->respBody);
    }

    len = cctx->respRem;
    STATUSREADERR(apr_socket_send(cctx->skt, cctx->respBody, &len));
    _mhLog(MH_VERBOSE, __FILE__, "sent with status %d:\n%.*s\n---- %d ----\n",
           status, (unsigned int)len, cctx->respBody, (unsigned int)len);

    if (len < cctx->respRem) {
        memcpy(cctx->respBody, cctx->respBody + len, cctx->respRem - len + 1);
        cctx->respRem -= len;
        cctx->currResp = resp;
    } else {
        cctx->respBody = NULL;
        cctx->respRem = 0;
        cctx->currResp = 0;
        return APR_EOF;
    }
    return status;
}

static mhResponse_t *cloneResponse(apr_pool_t *pool, mhResponse_t *resp)
{
    mhResponse_t *clone;
    clone = apr_pmemdup(pool, resp, sizeof(mhResponse_t));
    clone->hdrs = apr_hash_copy(pool, resp->hdrs);
    return clone;
}

static apr_status_t process(mhServCtx_t *ctx, _mhClientCtx_t *cctx,
                            const apr_pollfd_t *desc)
{
    apr_status_t status = APR_SUCCESS;

    /* First sent any pending responses before reading the next request. */
    if (desc->rtnevents & APR_POLLOUT && cctx->respQueue->nelts) {
        mhResponse_t **presp, *resp;

        /* TODO: response in progress */
        presp = apr_array_pop(cctx->respQueue);
        resp = *presp;
        if (resp) {
            _mhLog(MH_VERBOSE, __FILE__, "Sending response to client.\n");

            status = writeResponse(cctx, resp);
            if (status == APR_EOF) {
                ctx->mh->verifyStats->requestsResponded++;
                if (resp->closeConn) {
                    _mhLog(MH_VERBOSE, __FILE__,
                           "Actively closing connection.\n");
                    apr_socket_close(cctx->skt);
                    ctx->cctx = NULL;
                    return APR_EOF;
                }
                status = APR_SUCCESS;
            } else {
                cctx->currResp = resp;
                status = APR_EAGAIN;
            }
        } else {
            return APR_EGENERAL;
        }
    }
    if (desc->rtnevents & APR_POLLIN || cctx->buflen) {
        STATUSREADERR(readRequest(cctx, &cctx->req));
        if (status == APR_EOF && cctx->req) {
            mhResponse_t *resp;
            ctx->mh->verifyStats->requestsReceived++;
            apr_queue_push(ctx->reqQueue, cctx->req);
            if (_mhMatchRequest(ctx->mh, cctx->req, &resp) == YES) {
                ctx->mh->verifyStats->requestsMatched++;
                if (resp) {
                    _mhLog(MH_VERBOSE, __FILE__,
                           "Request matched, queueing response.\n");
                } else {
                    _mhLog(MH_VERBOSE, __FILE__,
                           "Request matched, queueing default response.\n");
                    resp = cloneResponse(ctx->pool, ctx->mh->defResponse);
                }
            } else {
                ctx->mh->verifyStats->requestsNotMatched++;
                _mhLog(MH_VERBOSE, __FILE__,
                       "Request found no match, queueing error response.\n");
                resp = cloneResponse(ctx->pool, ctx->mh->defErrorResponse);
            }

            resp->req = cctx->req;
            *((mhResponse_t **)apr_array_push(cctx->respQueue)) = resp;
            cctx->req = NULL;
            status = APR_SUCCESS;
        }
    }

    return status;
}

/******************************************************************************/
/* Process socket events                                                      */
/******************************************************************************/
apr_status_t _mhRunServerLoop(mhServCtx_t *ctx)
{
    apr_int32_t num;
    const apr_pollfd_t *desc;
    _mhClientCtx_t *cctx;
    apr_pollfd_t pfd = { 0 };
    apr_status_t status;

    cctx = ctx->cctx;

    /* something to write */
    if (cctx) {
        pfd.desc_type = APR_POLL_SOCKET;
        pfd.desc.s = cctx->skt;
        pfd.reqevents = cctx->reqevents;
        pfd.client_data = cctx;
        apr_pollset_remove(ctx->pollset, &pfd);

        cctx->reqevents = APR_POLLIN;
        if (cctx->respQueue->nelts > 0)
            cctx->reqevents |= APR_POLLOUT;
        pfd.reqevents = ctx->cctx->reqevents;
        STATUSERR(apr_pollset_add(ctx->pollset, &pfd));
    }

    STATUSERR(apr_pollset_poll(ctx->pollset, APR_USEC_PER_SEC >> 1,
                               &num, &desc));

    /* The same socket can be returned multiple times by apr_pollset_poll() */
    while (num--) {
        if (desc->desc.s == ctx->skt) {
            apr_socket_t *cskt;
            apr_pollfd_t pfd = { 0 };

            _mhLog(MH_VERBOSE, __FILE__, "Accepting client connection.\n");

            _mhClientCtx_t *cctx = apr_pcalloc(ctx->pool, sizeof(_mhClientCtx_t));

            STATUSERR(apr_socket_accept(&cskt, ctx->skt, ctx->pool));

            STATUSERR(apr_socket_opt_set(cskt, APR_SO_NONBLOCK, 1));
            STATUSERR(apr_socket_timeout_set(cskt, 0));

            pfd.desc_type = APR_POLL_SOCKET;
            pfd.desc.s = cskt;
            pfd.reqevents = APR_POLLIN;
            pfd.client_data = cctx;

            STATUSERR(apr_pollset_add(ctx->pollset, &pfd));

            cctx->pool = ctx->pool;
            cctx->skt = cskt;
            cctx->buflen = 0;
            cctx->bufrem = BUFSIZE;
            cctx->reqevents = pfd.reqevents;
            cctx->closeConn = NO;
            cctx->respQueue = apr_array_make(ctx->pool, 5,
                                             sizeof(mhResponse_t *));
            cctx->currResp = NULL;
            ctx->cctx = cctx;
        } else {
            /* one of the client sockets */
            _mhClientCtx_t *cctx = desc->client_data;

            STATUSREADERR(process(ctx, cctx, desc));
        }
        desc++;
    }

    return status;
}

int mhServerPortNr(const MockHTTP *mh)
{
    return mh->servCtx->port;
}
