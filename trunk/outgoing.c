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

#include <apr_pools.h>
#include <apr_poll.h>
#include <apr_version.h>
#include <apr_portable.h>
#include <apr_strings.h>

#include "serf.h"
#include "serf_bucket_util.h"

#include "serf_private.h"

/* forward definitions */
static apr_status_t read_from_connection(serf_connection_t *conn);
static apr_status_t write_to_connection(serf_connection_t *conn);
static apr_status_t hangup_connection(serf_connection_t *conn);

/* cleanup for sockets */
static apr_status_t clean_skt(void *data)
{
    serf_connection_t *conn = data;
    apr_status_t status = APR_SUCCESS;

    if (conn->skt) {
        status = apr_socket_close(conn->skt);
        conn->skt = NULL;
        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "closed socket, status %d\n", status);
        serf_config_remove_value(conn->config, SERF_CONFIG_CONN_LOCALIP);
        serf_config_remove_value(conn->config, SERF_CONFIG_CONN_REMOTEIP);
    }

    return status;
}

/* cleanup for conns */
static apr_status_t clean_conn(void *data)
{
    serf_connection_t *conn = data;

    serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
              "cleaning up connection 0x%x\n", conn);
    serf_connection_close(conn);

    return APR_SUCCESS;
}

/* Check if there is data waiting to be sent over the socket. This can happen
   in two situations:
   - The connection queue has atleast one request with unwritten data.
   - All requests are written and the ssl layer wrote some data while reading
     the response. This can happen when the server triggers a renegotiation,
     e.g. after the first and only request on that connection was received.
   Returns 1 if data is pending on CONN, NULL if not.
   If NEXT_REQ is not NULL, it will be filled in with the next available request
   with unwritten data. */
static int
request_or_data_pending(serf_request_t **next_req, serf_connection_t *conn)
{
    int reqs_in_progress;

    reqs_in_progress = conn->completed_requests - conn->completed_responses;

    /* Prepare the next request */
    if (conn->framing_type != SERF_CONNECTION_FRAMING_TYPE_NONE
        && (conn->pipelining || (!conn->pipelining && reqs_in_progress == 0)))
    {
        /* Skip all requests that have been written completely but we're still
         waiting for a response. */
        serf_request_t *request = conn->unwritten_reqs;

        if (next_req)
            *next_req = request;

        if (request != NULL) {
            return 1;
        }
    }

    if (next_req)
        *next_req = NULL;

    if (conn->ostream_head) {
        const char *dummy;
        apr_size_t len;
        apr_status_t status;

        status = serf_bucket_peek(conn->ostream_head, &dummy,
                                  &len);
        if (!SERF_BUCKET_READ_ERROR(status) && len) {
            serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                      "Extra data to be written after sending complete requests.\n");
            return 1;
        }
    }

    return 0;
}

/* Update the pollset for this connection. We tweak the pollset based on
 * whether we want to read and/or write, given conditions within the
 * connection. If the connection is not (yet) in the pollset, then it
 * will be added.
 */
apr_status_t serf__conn_update_pollset(serf_connection_t *conn)
{
    serf_context_t *ctx = conn->ctx;
    apr_status_t status;
    apr_pollfd_t desc = { 0 };

    if (!conn->skt) {
        return APR_SUCCESS;
    }

    /* Remove the socket from the poll set. */
    desc.desc_type = APR_POLL_SOCKET;
    desc.desc.s = conn->skt;
    desc.reqevents = conn->reqevents;

    status = ctx->pollset_rm(ctx->pollset_baton,
                             &desc, &conn->baton);
    if (status && !APR_STATUS_IS_NOTFOUND(status))
        return status;

    /* Now put it back in with the correct read/write values. */
    desc.reqevents = APR_POLLHUP | APR_POLLERR;
    if ((conn->written_reqs || conn->unwritten_reqs) &&
        conn->state != SERF_CONN_INIT) {
        /* If there are any outstanding events, then we want to read. */
        /* ### not true. we only want to read IF we have sent some data */
        desc.reqevents |= APR_POLLIN;

        /* Don't write if OpenSSL told us that it needs to read data first. */
        if (! conn->stop_writing) {

            /* If the connection is not closing down and
             *   has unwritten data or
             *   there are any requests that still have buckets to write out,
             *     then we want to write.
             */
            if (conn->vec_len &&
                conn->state != SERF_CONN_CLOSING)
                desc.reqevents |= APR_POLLOUT;
            else {

                if ((conn->probable_keepalive_limit &&
                     conn->completed_requests > conn->probable_keepalive_limit) ||
                    (conn->max_outstanding_requests &&
                     conn->completed_requests - conn->completed_responses >=
                     conn->max_outstanding_requests)) {
                        /* we wouldn't try to write any way right now. */
                }
                else if (request_or_data_pending(NULL, conn)) {
                    desc.reqevents |= APR_POLLOUT;
                }
            }
        }
    }

    /* If we can have async responses, always look for something to read. */
    if (conn->framing_type != SERF_CONNECTION_FRAMING_TYPE_HTTP1
        || conn->async_responses)
    {
        desc.reqevents |= APR_POLLIN;
    }

    /* save our reqevents, so we can pass it in to remove later. */
    conn->reqevents = desc.reqevents;

    /* Note: even if we don't want to read/write this socket, we still
     * want to poll it for hangups and errors.
     */
    return ctx->pollset_add(ctx->pollset_baton,
                            &desc, &conn->baton);
}

#ifdef SERF_DEBUG_BUCKET_USE

/* Make sure all response buckets were drained. */
static void check_buckets_drained(serf_connection_t *conn)
{
    serf_request_t *request = conn->requests;

    for ( ; request ; request = request->next ) {
        if (request->resp_bkt != NULL) {
            /* ### crap. can't do this. this allocator may have un-drained
             * ### REQUEST buckets.
             */
            /* serf_debug__entered_loop(request->resp_bkt->allocator); */
            /* ### for now, pretend we closed the conn (resets the tracking) */
            serf_debug__closed_conn(request->resp_bkt->allocator);
        }
    }
}

#endif

static void destroy_ostream(serf_connection_t *conn)
{
    if (conn->ostream_head != NULL) {
        serf_bucket_destroy(conn->ostream_head);
        conn->ostream_head = NULL;
        conn->ostream_tail = NULL;
    }
}

static apr_status_t detect_eof(void *baton, serf_bucket_t *aggregate_bucket)
{
    serf_connection_t *conn = baton;
    conn->hit_eof = 1;
    return APR_EAGAIN;
}

static apr_status_t do_conn_setup(serf_connection_t *conn)
{
    apr_status_t status;
    serf_bucket_t *ostream;

    /* ### dunno what the hell this is about. this latency stuff got
       ### added, and who knows whether it should stay...  */
    conn->latency = apr_time_now() - conn->connect_time;

    if (conn->ostream_head == NULL) {
        conn->ostream_head = serf_bucket_aggregate_create(conn->allocator);
    }

    if (conn->ostream_tail == NULL) {
        conn->ostream_tail = serf__bucket_stream_create(conn->allocator,
                                                        detect_eof,
                                                        conn);
    }

    ostream = conn->ostream_tail;

    status = (*conn->setup)(conn->skt,
                            &conn->stream,
                            &ostream,
                            conn->setup_baton,
                            conn->pool);
    if (status) {
        /* extra destroy here since it wasn't added to the head bucket yet. */
        serf_bucket_destroy(conn->ostream_tail);
        destroy_ostream(conn);
        return status;
    }

    /* Share the configuration with all the buckets in the newly created output
     chain (see PLAIN or ENCRYPTED scenario's), including the request buckets
     created by the application (ostream_tail will handle this for us). */
    serf_bucket_set_config(conn->ostream_head, conn->config);

    /* Share the configuration with the ssl_decrypt and socket buckets. The
     response buckets wrapping the ssl_decrypt/socket buckets won't get the
     config automatically because they are upstream. */
    serf_bucket_set_config(conn->stream, conn->config);

    serf_bucket_aggregate_append(conn->ostream_head,
                                 ostream);

    /* We typically have one of two scenarios, based on whether the
       application decided to encrypt this connection:

       PLAIN:

         conn->stream = SOCKET(skt)
         conn->ostream_head = AGGREGATE(ostream_tail)
         conn->ostream_tail = STREAM(<detect_eof>, REQ1, REQ2, ...)

       ENCRYPTED:

         conn->stream = DECRYPT(SOCKET(skt))
         conn->ostream_head = AGGREGATE(ENCRYPT(ostream_tail))
         conn->ostream_tail = STREAM(<detect_eof>, REQ1, REQ2, ...)

       where STREAM is an internal variant of AGGREGATE.
    */

    return status;
}

/* Set up the input and output stream buckets.
 When a tunnel over an http proxy is needed, create a socket bucket and
 empty aggregate bucket for sending and receiving unencrypted requests
 over the socket.

 After the tunnel is there, or no tunnel was needed, ask the application
 to create the input and output buckets, which should take care of the
 [en/de]cryption.
 */

static apr_status_t prepare_conn_streams(serf_connection_t *conn,
                                         serf_bucket_t **ostreamt,
                                         serf_bucket_t **ostreamh)
{
    apr_status_t status;

    /* Do we need a SSL tunnel first? */
    if (conn->state == SERF_CONN_CONNECTED) {
        /* If the connection does not have an associated bucket, then
         * call the setup callback to get one.
         */
        if (conn->stream == NULL) {
            status = do_conn_setup(conn);
            if (status) {
                return status;
            }
        }
        *ostreamt = conn->ostream_tail;
        *ostreamh = conn->ostream_head;
    } else {
        /* state == SERF_CONN_SETUP_SSLTUNNEL  */

        /* SSL tunnel needed and not set up yet, get a direct unencrypted
         stream for this socket */
        if (conn->stream == NULL) {
            conn->stream = serf_context_bucket_socket_create(conn->ctx,
                                                             conn->skt,
                                                             conn->allocator);
        }

        /* Don't create the ostream bucket chain including the ssl_encrypt
         bucket yet. This ensure the CONNECT request is sent unencrypted
         to the proxy. */
        *ostreamt = *ostreamh = conn->ssltunnel_ostream;
    }

    return APR_SUCCESS;
}

static void store_ipaddresses_in_config(serf_config_t *config,
                                        apr_socket_t *skt)
{
     apr_sockaddr_t *sa;

    if (apr_socket_addr_get(&sa, APR_LOCAL, skt) == APR_SUCCESS) {
        char buf[32];
        apr_sockaddr_ip_getbuf(buf, 32, sa);
        serf_config_set_stringf(config, SERF_CONFIG_CONN_LOCALIP,
                                "%s:%d", buf, sa->port);
    }
    if (apr_socket_addr_get(&sa, APR_REMOTE, skt) == APR_SUCCESS) {
        char buf[32];
        apr_sockaddr_ip_getbuf(buf, 32, sa);
        serf_config_set_stringf(config, SERF_CONFIG_CONN_REMOTEIP,
                               "%s:%d", buf, sa->port);
    }
}

/* Create and connect sockets for any connections which don't have them
 * yet. This is the core of our lazy-connect behavior.
 */
apr_status_t serf__open_connections(serf_context_t *ctx)
{
    int i;

    for (i = ctx->conns->nelts; i--; ) {
        serf_connection_t *conn = GET_CONN(ctx, i);
        apr_status_t status;
        apr_socket_t *skt;

        conn->seen_in_pollset = 0;

        if (conn->skt != NULL) {
#ifdef SERF_DEBUG_BUCKET_USE
            check_buckets_drained(conn);
#endif
            continue;
        }

        /* Delay opening until we have something to deliver! */
        if (conn->unwritten_reqs == NULL) {
            continue;
        }

        apr_pool_clear(conn->skt_pool);
        apr_pool_cleanup_register(conn->skt_pool, conn, clean_skt,
                                  apr_pool_cleanup_null);

        status = apr_socket_create(&skt, conn->address->family,
                                   SOCK_STREAM,
#if APR_MAJOR_VERSION > 0
                                   APR_PROTO_TCP,
#endif
                                   conn->skt_pool);
        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "created socket for conn 0x%x, status %d\n", conn, status);
        if (status != APR_SUCCESS)
            return status;

        /* Set the socket to be non-blocking */
        if ((status = apr_socket_timeout_set(skt, 0)) != APR_SUCCESS)
            return status;

        /* Disable Nagle's algorithm */
        if ((status = apr_socket_opt_set(skt,
                                         APR_TCP_NODELAY, 1)) != APR_SUCCESS)
            return status;

        /* Configured. Store it into the connection now. */
        conn->skt = skt;

        /* Remember time when we started connecting to server to calculate
           network latency. */
        conn->connect_time = apr_time_now();

        /* Now that the socket is set up, let's connect it. This should
         * return immediately.
         */
        status = apr_socket_connect(skt, conn->address);
        store_ipaddresses_in_config(conn->config, skt);

        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "connected socket for conn 0x%x, status %d\n", conn, status);
        if (status != APR_SUCCESS) {
            if (!APR_STATUS_IS_EINPROGRESS(status))
                return status;
        }

        status = serf_config_set_string(conn->config,
                     SERF_CONFIG_CONN_PIPELINING,
                     (conn->max_outstanding_requests != 1 &&
                      conn->pipelining == 1) ? "Y" : "N");
        if (status)
            return status;

        /* Flag our pollset as dirty now that we have a new socket. */
        conn->dirty_conn = 1;
        ctx->dirty_pollset = 1;

        /* If the authentication was already started on another connection,
           prepare this connection (it might be possible to skip some
           part of the handshaking). */
        if (ctx->proxy_address) {
            status = serf__auth_setup_connection(PROXY, conn);
            if (status){
                return status;
            }
        }

        status = serf__auth_setup_connection(HOST, conn);
        if (status)
            return status;

        /* Does this connection require a SSL tunnel over the proxy? */
        if (ctx->proxy_address && strcmp(conn->host_info.scheme, "https") == 0)
            serf__ssltunnel_connect(conn);
        else {
            conn->state = SERF_CONN_CONNECTED;
            status = do_conn_setup(conn);
            if (status)
                return status;
        }
    }

    return APR_SUCCESS;
}

static apr_status_t no_more_writes(serf_connection_t *conn)
{
    /* Note that we should hold new requests until we open our new socket. */
    conn->state = SERF_CONN_CLOSING;
    serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
              "stop writing on conn 0x%x\n", conn);

    /* Clear our iovec. */
    conn->vec_len = 0;

    /* Update the pollset to know we don't want to write on this socket any
     * more.
     */
    conn->dirty_conn = 1;
    conn->ctx->dirty_pollset = 1;
    return APR_SUCCESS;
}

/* Read the 'Connection' header from the response. Return SERF_ERROR_CLOSING if
 * the header contains value 'close' indicating the server is closing the
 * connection right after this response.
 * Otherwise returns APR_SUCCESS.
 */
static apr_status_t is_conn_closing(serf_bucket_t *response)
{
    serf_bucket_t *hdrs;
    const char *val;

    hdrs = serf_bucket_response_get_headers(response);
    val = serf_bucket_headers_get(hdrs, "Connection");
    if (val && strcasecmp("close", val) == 0)
        {
            return SERF_ERROR_CLOSING;
        }

    return APR_SUCCESS;
}

static apr_status_t remove_connection(serf_context_t *ctx,
                                      serf_connection_t *conn)
{
    apr_pollfd_t desc = { 0 };

    desc.desc_type = APR_POLL_SOCKET;
    desc.desc.s = conn->skt;
    desc.reqevents = conn->reqevents;

    return ctx->pollset_rm(ctx->pollset_baton,
                           &desc, &conn->baton);
}

/* A socket was closed, inform the application. */
static void handle_conn_closed(serf_connection_t *conn, apr_status_t status)
{
    (*conn->closed)(conn, conn->closed_baton, status,
                    conn->pool);
}

static apr_status_t reset_connection(serf_connection_t *conn,
                                     int requeue_requests)
{
    serf_context_t *ctx = conn->ctx;
    apr_status_t status;
    serf_request_t *old_reqs;

    serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
              "reset connection 0x%x\n", conn);

    conn->probable_keepalive_limit = conn->completed_responses;
    conn->completed_requests = 0;
    conn->completed_responses = 0;

    /* Clear the unwritten_reqs queue, so the application can requeue cancelled
       requests on it for the new socket. */
    old_reqs = conn->unwritten_reqs;
    conn->unwritten_reqs = NULL;
    conn->unwritten_reqs_tail = NULL;

    /* First, cancel all written requests for which we haven't received a 
       response yet. Inform the application that the request is cancelled, 
       so it can requeue them if needed. */
    while (conn->written_reqs) {
        serf__cancel_request(conn->written_reqs, &conn->written_reqs,
                             requeue_requests);
    }
    conn->written_reqs_tail = NULL;

    /* Handle all outstanding unwritten requests.
       TODO: what about a partially written request? */
    while (old_reqs) {
        /* If we haven't started to write the connection, bring it over
         * unchanged to our new socket.
         * Do not copy a CONNECT request to the new connection, the ssl tunnel
         * setup code will create a new CONNECT request already.
         */
        if (requeue_requests && !old_reqs->writing_started &&
            !old_reqs->ssltunnel) {

            serf_request_t *req = old_reqs;
            old_reqs = old_reqs->next;
            req->next = NULL;
            serf__link_requests(&conn->unwritten_reqs,
                                &conn->unwritten_reqs_tail,
                                req);
        }
        else {
            /* We don't want to requeue the request or this request was partially
               written. Inform the application that the request is cancelled. */
            serf__cancel_request(old_reqs, &old_reqs, requeue_requests);
        }
    }

    /* Requests queue has been prepared for a new socket, close the old one. */
    if (conn->skt != NULL) {
        remove_connection(ctx, conn);
        status = clean_skt(conn);
        if (conn->closed != NULL) {
            handle_conn_closed(conn, status);
        }
    }

    if (conn->stream != NULL) {
        serf_bucket_destroy(conn->stream);
        conn->stream = NULL;
    }

    destroy_ostream(conn);

    /* Don't try to resume any writes */
    conn->vec_len = 0;

    conn->dirty_conn = 1;
    conn->ctx->dirty_pollset = 1;
    conn->state = SERF_CONN_INIT;

    conn->hit_eof = 0;
    conn->connect_time = 0;
    conn->latency = -1;
    conn->stop_writing = 0;
    conn->write_now = 0;
    /* conn->pipelining */

    conn->framing_type = SERF_CONNECTION_FRAMING_TYPE_HTTP1;

    if (conn->protocol_baton) {
        conn->perform_teardown(conn);
        conn->protocol_baton = NULL;
    }

    conn->perform_read = read_from_connection;
    conn->perform_write = write_to_connection;
    conn->perform_hangup = hangup_connection;
    conn->perform_teardown = NULL;

    conn->status = APR_SUCCESS;

    /* Let our context know that we've 'reset' the socket already. */
    conn->seen_in_pollset |= APR_POLLHUP;

    /* Recalculate the current list length */
    conn->nr_of_written_reqs = 0;
    conn->nr_of_unwritten_reqs = serf__req_list_length(conn->unwritten_reqs);

    /* Found the connection. Closed it. All done. */
    return APR_SUCCESS;
}

static apr_status_t socket_writev(serf_connection_t *conn)
{
    apr_size_t written;
    apr_status_t status;

    status = apr_socket_sendv(conn->skt, conn->vec,
                              conn->vec_len, &written);
    if (status && !APR_STATUS_IS_EAGAIN(status))
        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "socket_sendv error %d\n", status);

    /* did we write everything? */
    if (written) {
        apr_size_t len = 0;
        int i;

        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "--- socket_sendv: %d bytes. --\n", written);

        for (i = 0; i < conn->vec_len; i++) {
            len += conn->vec[i].iov_len;
            if (written < len) {
                serf__log_nopref(LOGLVL_DEBUG, LOGCOMP_RAWMSG, conn->config,
                                 "%.*s", conn->vec[i].iov_len - (len - written),
                                 conn->vec[i].iov_base);
                if (i) {
                    memmove(conn->vec, &conn->vec[i],
                            sizeof(struct iovec) * (conn->vec_len - i));
                    conn->vec_len -= i;
                }
                conn->vec[0].iov_base = (char *)conn->vec[0].iov_base + (conn->vec[0].iov_len - (len - written));
                conn->vec[0].iov_len = len - written;
                break;
            } else {
                serf__log_nopref(LOGLVL_DEBUG, LOGCOMP_RAWMSG, conn->config,
                                 "%.*s",
                                 conn->vec[i].iov_len, conn->vec[i].iov_base);
            }
        }
        if (len == written) {
            conn->vec_len = 0;
        }
        serf__log_nopref(LOGLVL_DEBUG, LOGCOMP_RAWMSG, conn->config, "\n");

        /* Log progress information */
        serf__context_progress_delta(conn->ctx, 0, written);
    }

    return status;
}

apr_status_t serf__connection_flush(serf_connection_t *conn,
                                    int pump)
{
    apr_status_t status = APR_SUCCESS;
    apr_status_t read_status = APR_SUCCESS;

    if (pump) {
        serf_bucket_t *ostreamt, *ostreamh;

        status = prepare_conn_streams(conn, &ostreamt, &ostreamh);
        if (status) {
            return status;
        }

        /* ### optimize at some point by using read_for_sendfile */
        /* TODO: now that read_iovec will effectively try to return as much
           data as available, we probably don't want to read ALL_AVAIL, but
           a lower number, like the size of one or a few TCP packets, the
           available TCP buffer size ... */
        conn->hit_eof = 0;
        read_status = serf_bucket_read_iovec(ostreamh,
                                             SERF_READ_ALL_AVAIL,
                                             IOV_MAX,
                                             conn->vec,
                                             &conn->vec_len);

        if (read_status == SERF_ERROR_WAIT_CONN) {
            /* The bucket told us that it can't provide more data until
            more data is read from the socket. This normally happens
            during a SSL handshake.

            We should avoid looking for writability for a while so
            that (hopefully) something will appear in the bucket so
            we can actually write something. otherwise, we could
            end up in a CPU spin: socket wants something, but we
            don't have anything (and keep returning EAGAIN) */
            conn->stop_writing = 1;
            conn->dirty_conn = 1;
            conn->ctx->dirty_pollset = 1;

            read_status = APR_EAGAIN;
        }
        else if (APR_STATUS_IS_EAGAIN(read_status)) {

            /* We read some stuff, but did we read everything ? */
            if (conn->hit_eof)
                read_status = APR_SUCCESS;
        }
        else if (SERF_BUCKET_READ_ERROR(read_status)) {

            /* Something bad happened. Propagate any errors. */
            return read_status;
        }
    }

    while (conn->vec_len && !status) {
        status = socket_writev(conn);

        /* If the write would have blocked, then we're done. Don't try
         * to write anything else to the socket.
         */
        if (APR_STATUS_IS_EPIPE(status)
            || APR_STATUS_IS_ECONNRESET(status)
            || APR_STATUS_IS_ECONNABORTED(status))
            return no_more_writes(conn);

    }

    return status ? status : read_status;
}

/* write data out to the connection */
static apr_status_t write_to_connection(serf_connection_t *conn)
{
    if (conn->probable_keepalive_limit &&
        conn->completed_requests > conn->probable_keepalive_limit) {

        conn->dirty_conn = 1;
        conn->ctx->dirty_pollset = 1;

        /* backoff for now. */
        return APR_SUCCESS;
    }

    /* Keep reading and sending until we run out of stuff to read, or
     * writing would block.
     */
    while (1) {
        serf_request_t *request;
        apr_status_t status;
        apr_status_t read_status;
        serf_bucket_t *ostreamt;
        serf_bucket_t *ostreamh;
        int reqs_in_progress;

        reqs_in_progress = conn->completed_requests - conn->completed_responses;

        /* If we're setting up an ssl tunnel, we can't send real requests
           at yet, as they need to be encrypted and our encrypt buckets
           aren't created yet as we still need to read the unencrypted
           response of the CONNECT request. */
        if (conn->state == SERF_CONN_SETUP_SSLTUNNEL && reqs_in_progress > 0)
        {
            return APR_SUCCESS;
        }

        /* We try to limit the number of in-flight requests so that we
           don't have to repeat too many if the connection drops.  */
        if (conn->max_outstanding_requests
            && (reqs_in_progress >= conn->max_outstanding_requests))
        {
            /* backoff for now. */
            return APR_SUCCESS;
        }

        /* If we have unwritten data, then write what we can. */
        status = serf__connection_flush(conn, FALSE);
        if (APR_STATUS_IS_EAGAIN(status))
            return APR_SUCCESS;
        else if (status)
            return status;

        /* ### can we have a short write, yet no EAGAIN? a short write
           ### would imply unwritten_len > 0 ... */
        /* assert: unwritten_len == 0. */

        /* We may need to move forward to a request which has something
         * to write.
         */
        if (!request_or_data_pending(&request, conn)) {
            /* No more requests (with data) are registered with the
             * connection, and no data is pending on the outgoing stream.
             * Let's update the pollset so that we don't try to write to this
             * socket again.
             */
            conn->dirty_conn = 1;
            conn->ctx->dirty_pollset = 1;
            return APR_SUCCESS;
        }

        status = prepare_conn_streams(conn, &ostreamt, &ostreamh);
        if (status) {
            return status;
        }

        if (request) {
            if (request->req_bkt == NULL) {
                read_status = serf__setup_request(request);
                if (read_status) {
                    /* Something bad happened. Propagate any errors. */
                    return read_status;
                }
            }

            if (!request->writing_started) {
                request->writing_started = 1;
                serf_bucket_aggregate_append(ostreamt, request->req_bkt);
            }
        }

        /* If we got some data, then deliver it. */
        /* ### what to do if we got no data?? is that a problem? */
        status = serf__connection_flush(conn, TRUE);
        if (APR_STATUS_IS_EAGAIN(status))
            return APR_SUCCESS;
        else if (status)
            return status;

        if (request && conn->hit_eof && conn->vec_len == 0) {
            /* If we hit the end of the request bucket and all of its data has
             * been written, then clear it out to signify that we're done
             * sending the request. On the next iteration through this loop:
             * - if there are remaining bytes they will be written, and as the 
             * request bucket will be completely read it will be destroyed then.
             * - we'll see if there are other requests that need to be sent 
             * ("pipelining").
             */
            serf_bucket_destroy(request->req_bkt);
            request->req_bkt = NULL;

            /* Move the request to the written queue */
            serf__link_requests(&conn->written_reqs, &conn->written_reqs_tail,
                                request);
            conn->nr_of_written_reqs++;
            conn->unwritten_reqs = conn->unwritten_reqs->next;
            conn->nr_of_unwritten_reqs--;
            request->next = NULL;

            /* If our connection has async responses enabled, we're not
             * going to get a reply back, so kill the request.
             */
            if (conn->async_responses) {
                conn->unwritten_reqs = request->next;
                conn->nr_of_unwritten_reqs--;
                serf__destroy_request(request);
            }

            conn->completed_requests++;

            if (conn->probable_keepalive_limit &&
                conn->completed_requests > conn->probable_keepalive_limit) {
                /* backoff for now. */
                return APR_SUCCESS;
            }
        }
    }
    /* NOTREACHED */
}



/* An async response message was received from the server. */
static apr_status_t handle_async_response(serf_connection_t *conn,
                                          apr_pool_t *pool)
{
    apr_status_t status;

    if (conn->current_async_response == NULL) {
        conn->current_async_response =
            (*conn->async_acceptor)(NULL, conn->stream,
                                    conn->async_acceptor_baton, pool);
    }

    status = (*conn->async_handler)(NULL, conn->current_async_response,
                                    conn->async_handler_baton, pool);

    if (APR_STATUS_IS_EOF(status)) {
        serf_bucket_destroy(conn->current_async_response);
        conn->current_async_response = NULL;
        status = APR_SUCCESS;
    }

    return status;
}

/* read data from the connection */
static apr_status_t read_from_connection(serf_connection_t *conn)
{
    apr_status_t status;
    apr_pool_t *tmppool;
    apr_status_t close_connection = APR_SUCCESS;

    /* Whatever is coming in on the socket corresponds to the first request
     * on our chain.
     */
    serf_request_t *request = conn->written_reqs;
    if (!request) {
        /* Request wasn't completely written yet! */
        request = conn->unwritten_reqs;
    }

    /* If the stop_writing flag was set on the connection, reset it now because
       there is some data to read. */
    if (conn->stop_writing) {
        conn->stop_writing = 0;
        conn->dirty_conn = 1;
        conn->ctx->dirty_pollset = 1;
    }

    /* assert: request != NULL */

    if ((status = apr_pool_create(&tmppool, conn->pool)) != APR_SUCCESS)
        return status;

    /* Invoke response handlers until we have no more work. */
    while (1) {
        serf_bucket_t *dummy1, *dummy2;

        apr_pool_clear(tmppool);

        /* Only interested in the input stream here. */
        status = prepare_conn_streams(conn, &dummy1, &dummy2);
        if (status) {
            goto error;
        }

        /* We have a different codepath when we can have async responses. */
        if (conn->async_responses) {
            /* TODO What about socket errors? */
            status = handle_async_response(conn, tmppool);
            if (APR_STATUS_IS_EAGAIN(status)) {
                status = APR_SUCCESS;
                goto error;
            }
            if (status) {
                goto error;
            }
            continue;
        }

        /* We are reading a response for a request we haven't
         * written yet!
         *
         * This shouldn't normally happen EXCEPT:
         *
         * 1) when the other end has closed the socket and we're
         *    pending an EOF return.
         * 2) Doing the initial SSL handshake - we'll get EAGAIN
         *    as the SSL buckets will hide the handshake from us
         *    but not return any data.
         * 3) When the server sends us an SSL alert.
         *
         * In these cases, we should not receive any actual user data.
         *
         * 4) When the server sends a error response, like 408 Request timeout.
         *    This response should be passed to the application.
         *
         * If we see an EOF (due to either an expired timeout or the server
         * sending the SSL 'close notify' shutdown alert), we'll reset the
         * connection and open a new one.
         */
        if (request->req_bkt || !request->writing_started) {
            const char *data;
            apr_size_t len;

            status = serf_bucket_peek(conn->stream, &data, &len);

            if (APR_STATUS_IS_EOF(status)) {
                reset_connection(conn, 1);
                status = APR_SUCCESS;
                goto error;
            }
            else if (APR_STATUS_IS_EAGAIN(status) && !len) {
                status = APR_SUCCESS;
                goto error;
            } else if (status && !APR_STATUS_IS_EAGAIN(status)) {
                /* Read error */
                goto error;
            }

            /* Unexpected response from the server */
            if (conn->write_now) {
                conn->write_now = 0;
                status = conn->perform_write(conn);

                if (!SERF_BUCKET_READ_ERROR(status))
                    status = APR_SUCCESS;
            }
        }

        if (conn->framing_type != SERF_CONNECTION_FRAMING_TYPE_HTTP1)
            break;

        /* If the request doesn't have a response bucket, then call the
         * acceptor to get one created.
         */
        if (request->resp_bkt == NULL) {
            if (! request->acceptor) {
                /* Request wasn't even setup.
                   Server replying before it received anything? */
              return SERF_ERROR_BAD_HTTP_RESPONSE;
            }

            request->resp_bkt = (*request->acceptor)(request, conn->stream,
                                                     request->acceptor_baton,
                                                     tmppool);
            apr_pool_clear(tmppool);

            /* Share the configuration with the response bucket(s) */
            serf_bucket_set_config(request->resp_bkt, conn->config);
        }

        status = serf__handle_response(request, tmppool);

        /* If we received APR_SUCCESS, run this loop again. */
        if (!status) {
            continue;
        }

        /* Some systems will not generate a HUP poll event so we have to
         * handle the ECONNRESET issue and ECONNABORT here.
         */
        if (APR_STATUS_IS_ECONNRESET(status) ||
            APR_STATUS_IS_ECONNABORTED(status) ||
            status == SERF_ERROR_REQUEST_LOST) {
            /* If the connection had ever been good, be optimistic & try again.
             * If it has never tried again (incl. a retry), fail.
             */
            if (conn->completed_responses) {
                reset_connection(conn, 1);
                status = APR_SUCCESS;
            }
            else if (status == SERF_ERROR_REQUEST_LOST) {
                status = SERF_ERROR_ABORTED_CONNECTION;
            }
            goto error;
        }

        /* This connection uses HTTP pipelining and the server asked for a 
           renegotiation (e.g. to access the requested resource a specific
           client certificate is required).
           Because of a known problem in OpenSSL this won't work most of the 
           time, so as a workaround, when the server asks for a renegotiation
           on a connection using HTTP pipelining, we reset the connection,
           disable pipelining and reconnect to the server. */
        if (status == SERF_ERROR_SSL_NEGOTIATE_IN_PROGRESS) {
            serf__log(LOGLVL_WARNING, LOGCOMP_CONN, __FILE__, conn->config,
                      "The server requested renegotiation. Disable HTTP "
                      "pipelining and reset the connection.\n", conn);

            serf__connection_set_pipelining(conn, 0);
            reset_connection(conn, 1);
            status = APR_SUCCESS;
            goto error;
        }

        /* If our response handler says it can't do anything more, we now
         * treat that as a success.
         */
        if (APR_STATUS_IS_EAGAIN(status)) {
            /* It is possible that while reading the response, the ssl layer
               has prepared some data to send. If this was the last request,
               serf will not check for socket writability, so force this here.
             */
            if (request_or_data_pending(&request, conn) && !request) {
                conn->dirty_conn = 1;
                conn->ctx->dirty_pollset = 1;
            }
            status = APR_SUCCESS;
            goto error;
        }

        close_connection = is_conn_closing(request->resp_bkt);

        if (!APR_STATUS_IS_EOF(status) &&
            close_connection != SERF_ERROR_CLOSING) {
            /* Whether success, or an error, there is no more to do unless
             * this request has been completed.
             */
            goto error;
        }

        /* The response has been fully-read, so that means the request has
         * either been fully-delivered (most likely), or that we don't need to
         * write the rest of it anymore, e.g. when a 408 Request timeout was
         $ received.
         * Remove it from our queue and loop to read another response.
         */
        if (request == conn->written_reqs) {
            conn->written_reqs = request->next;
            conn->nr_of_written_reqs--;
        } else {
            conn->unwritten_reqs = request->next;
            conn->nr_of_unwritten_reqs--;
        }

        serf__destroy_request(request);

        request = conn->written_reqs;
        if (!request) {
            /* Received responses for all written requests */
            conn->written_reqs_tail = NULL;
            /* Request wasn't completely written yet! */
            request = conn->unwritten_reqs;
            if (!request)
                conn->unwritten_reqs_tail = NULL;
        }

        conn->completed_responses++;

        /* We have received a response. If there are no more outstanding
           requests on this connection, we should stop polling for READ events
           for now. */
        if (!conn->written_reqs && !conn->unwritten_reqs) {
            conn->dirty_conn = 1;
            conn->ctx->dirty_pollset = 1;
        }

        /* This means that we're being advised that the connection is done. */
        if (close_connection == SERF_ERROR_CLOSING) {
            reset_connection(conn, 1);
            if (APR_STATUS_IS_EOF(status))
                status = APR_SUCCESS;
            goto error;
        }

        /* The server is suddenly deciding to serve more responses than we've
         * seen before.
         *
         * Let our requests go.
         */
        if (conn->probable_keepalive_limit &&
            conn->completed_responses > conn->probable_keepalive_limit) {
            conn->probable_keepalive_limit = 0;
        }

        /* If we just ran out of requests or have unwritten requests, then
         * update the pollset. We don't want to read from this socket any
         * more. We are definitely done with this loop, too.
         */
        if (request == NULL || !request->writing_started) {
            conn->dirty_conn = 1;
            conn->ctx->dirty_pollset = 1;
            status = APR_SUCCESS;
            goto error;
        }
    }

error:
    apr_pool_destroy(tmppool);
    return status;
}

/* The connection got reset by the server. On Windows this can happen
   when all data is read, so just cleanup the connection and open a new one.

   If we haven't had any successful responses on this connection,
   then error out as it is likely a server issue. */
static apr_status_t hangup_connection(serf_connection_t *conn)
{
    if (conn->completed_responses) {
        return reset_connection(conn, 1);
    }

    return SERF_ERROR_ABORTED_CONNECTION;
}

/* process all events on the connection */
apr_status_t serf__process_connection(serf_connection_t *conn,
                                      apr_int16_t events)
{
    apr_status_t status;

    /* POLLHUP/ERR should come after POLLIN so if there's an error message or
     * the like sitting on the connection, we give the app a chance to read
     * it before we trigger a reset condition.
     */
    if ((events & APR_POLLIN) != 0) {
        if ((status = conn->perform_read(conn)) != APR_SUCCESS)
            return status;

        /* If we decided to reset our connection, return now as we don't
         * want to write.
         */
        if ((conn->seen_in_pollset & APR_POLLHUP) != 0) {
            return APR_SUCCESS;
        }
    }
    if ((events & APR_POLLHUP) != 0) {
        /* The connection got reset by the server. */
        return conn->perform_hangup(conn);
    }
    if ((events & APR_POLLERR) != 0) {
        /* We might be talking to a buggy HTTP server that doesn't
         * do lingering-close.  (httpd < 2.1.8 does this.)
         *
         * See:
         *
         * http://issues.apache.org/bugzilla/show_bug.cgi?id=35292
         */
        if (conn->completed_requests && !conn->probable_keepalive_limit) {
            return reset_connection(conn, 1);
        }
#ifdef SO_ERROR
        /* If possible, get the error from the platform's socket layer and
           convert it to an APR status code. */
        {
            apr_os_sock_t osskt;
            if (!apr_os_sock_get(&osskt, conn->skt)) {
                int error;
                apr_socklen_t l = sizeof(error);

                if (!getsockopt(osskt, SOL_SOCKET, SO_ERROR, (char*)&error,
                                &l)) {
                    status = APR_FROM_OS_ERROR(error);

                    /* Handle fallback for multi-homed servers.
                     
                       ### Improve algorithm to find better than just 'next'?

                       Current Windows versions already handle re-ordering for
                       api users by using statistics on the recently failed
                       connections to order the list of addresses. */
                    if (conn->completed_requests == 0
                        && conn->address->next != NULL
                        && (APR_STATUS_IS_ECONNREFUSED(status)
                            || APR_STATUS_IS_TIMEUP(status)
                            || APR_STATUS_IS_ENETUNREACH(status))) {

                        conn->address = conn->address->next;
                        return reset_connection(conn, 1);
                    }

                    return status;
                  }
            }
        }
#endif
        return APR_EGENERAL;
    }
    if ((events & APR_POLLOUT) != 0) {
        if ((status = conn->perform_write(conn)) != APR_SUCCESS)
            return status;
    }
    return APR_SUCCESS;
}

serf_connection_t *serf_connection_create(
    serf_context_t *ctx,
    apr_sockaddr_t *address,
    serf_connection_setup_t setup,
    void *setup_baton,
    serf_connection_closed_t closed,
    void *closed_baton,
    apr_pool_t *pool)
{
    serf_connection_t *conn = apr_pcalloc(pool, sizeof(*conn));

    conn->ctx = ctx;
    conn->status = APR_SUCCESS;
    /* Ignore server address if proxy was specified. */
    conn->address = ctx->proxy_address ? ctx->proxy_address : address;
    conn->setup = setup;
    conn->setup_baton = setup_baton;
    conn->closed = closed;
    conn->closed_baton = closed_baton;
    conn->pool = pool;
    conn->allocator = serf_bucket_allocator_create(pool, NULL, NULL);
    conn->stream = NULL;
    conn->ostream_head = NULL;
    conn->ostream_tail = NULL;
    conn->baton.type = SERF_IO_CONN;
    conn->baton.u.conn = conn;
    conn->hit_eof = 0;
    conn->state = SERF_CONN_INIT;
    conn->latency = -1; /* unknown */
    conn->stop_writing = 0;
    conn->write_now = 0;
    conn->pipelining = 1;
    conn->framing_type = SERF_CONNECTION_FRAMING_TYPE_HTTP1;
    conn->perform_read = read_from_connection;
    conn->perform_write = write_to_connection;
    conn->perform_hangup = hangup_connection;
    conn->perform_teardown = NULL;
    conn->protocol_baton = NULL;

    /* Create a subpool for our connection. */
    apr_pool_create(&conn->skt_pool, conn->pool);

    /* register a cleanup */
    apr_pool_cleanup_register(conn->pool, conn, clean_conn,
                              apr_pool_cleanup_null);

    /* Add the connection to the context. */
    *(serf_connection_t **)apr_array_push(ctx->conns) = conn;

    return conn;
}

apr_status_t serf_connection_create2(
    serf_connection_t **conn,
    serf_context_t *ctx,
    apr_uri_t host_info,
    serf_connection_setup_t setup,
    void *setup_baton,
    serf_connection_closed_t closed,
    void *closed_baton,
    apr_pool_t *pool)
{
    apr_status_t status = APR_SUCCESS;
    serf_config_t *config;
    serf_connection_t *c;
    apr_sockaddr_t *host_address = NULL;

    /* Set the port number explicitly, needed to create the socket later. */
    if (!host_info.port) {
        host_info.port = apr_uri_port_of_scheme(host_info.scheme);
    }

    /* Only lookup the address of the server if no proxy server was
       configured. */
    if (!ctx->proxy_address) {
        status = apr_sockaddr_info_get(&host_address,
                                       host_info.hostname,
                                       APR_UNSPEC, host_info.port, 0, pool);
        if (status)
            return status;
    }

    c = serf_connection_create(ctx, host_address, setup, setup_baton,
                               closed, closed_baton, pool);

    /* We're not interested in the path following the hostname. */
    c->host_url = apr_uri_unparse(c->pool,
                                  &host_info,
                                  APR_URI_UNP_OMITPATHINFO |
                                  APR_URI_UNP_OMITUSERINFO);

    /* Store the host info without the path on the connection. */
    (void)apr_uri_parse(c->pool, c->host_url, &(c->host_info));
    if (!c->host_info.port) {
        c->host_info.port = apr_uri_port_of_scheme(c->host_info.scheme);
    }

    /* Store the connection specific info in the configuration store */
    status = serf__config_store_get_config(ctx, c, &config, pool);
    if (status)
        return status;
    c->config = config;
    serf_config_set_stringc(config, SERF_CONFIG_HOST_NAME,
                            c->host_info.hostname);
    serf_config_set_stringc(config, SERF_CONFIG_HOST_PORT,
                           apr_itoa(ctx->pool, c->host_info.port));

    *conn = c;

    serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, c->config,
              "created connection 0x%x\n", c);

    return status;
}

apr_status_t serf_connection_reset(
    serf_connection_t *conn)
{
    return reset_connection(conn, 0);
}


apr_status_t serf_connection_close(
    serf_connection_t *conn)
{
    int i;
    serf_context_t *ctx = conn->ctx;
    apr_status_t status;

    for (i = ctx->conns->nelts; i--; ) {
        serf_connection_t *conn_seq = GET_CONN(ctx, i);

        if (conn_seq == conn) {
            /* The application asked to close the connection, no need to notify
               it for each cancelled request. */
            while (conn->written_reqs) {
                serf__cancel_request(conn->written_reqs, &conn->written_reqs, 0);
            }
            while (conn->unwritten_reqs) {
                serf__cancel_request(conn->unwritten_reqs, &conn->unwritten_reqs, 0);
            }
            if (conn->skt != NULL) {
                remove_connection(ctx, conn);
                status = clean_skt(conn);
                if (conn->closed != NULL) {
                    handle_conn_closed(conn, status);
                }
            }
            if (conn->stream != NULL) {
                serf_bucket_destroy(conn->stream);
                conn->stream = NULL;
            }

            destroy_ostream(conn);

            if (conn->protocol_baton) {
                conn->perform_teardown(conn);
                conn->protocol_baton = NULL;
            }

            /* Remove the connection from the context. We don't want to
             * deal with it any more.
             */
            if (i < ctx->conns->nelts - 1) {
                /* move later connections over this one. */
                memmove(
                    &GET_CONN(ctx, i),
                    &GET_CONN(ctx, i + 1),
                    (ctx->conns->nelts - i - 1) * sizeof(serf_connection_t *));
            }
            --ctx->conns->nelts;

            serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                      "closed connection 0x%x\n", conn);

            /* Found the connection. Closed it. All done. */
            return APR_SUCCESS;
        }
    }

    /* We didn't find the specified connection. */
    /* ### doc talks about this w.r.t poll structures. use something else? */
    return APR_NOTFOUND;
}


void serf_connection_set_max_outstanding_requests(
    serf_connection_t *conn,
    unsigned int max_requests)
{
    if (max_requests == 0)
        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "Set max. nr. of outstanding requests for this "
                  "connection to unlimited.\n");
    else
        serf__log(LOGLVL_DEBUG, LOGCOMP_CONN, __FILE__, conn->config,
                  "Limit max. nr. of outstanding requests for this "
                  "connection to %u.\n", max_requests);

    conn->max_outstanding_requests = max_requests;
}

/* Disable HTTP pipelining, ensure that only one request is outstanding at a 
   time. This is an internal method, an application that wants to disable
   HTTP pipelining can achieve this by calling:
     serf_connection_set_max_outstanding_requests(conn, 1) .
 */
void serf__connection_set_pipelining(serf_connection_t *conn, int enabled)
{
    conn->pipelining = enabled;
}

void serf_connection_set_async_responses(
    serf_connection_t *conn,
    serf_response_acceptor_t acceptor,
    void *acceptor_baton,
    serf_response_handler_t handler,
    void *handler_baton)
{
    conn->async_responses = 1;
    conn->async_acceptor = acceptor;
    conn->async_acceptor_baton = acceptor_baton;
    conn->async_handler = handler;
    conn->async_handler_baton = handler_baton;
}

void serf_connection_set_framing_type(
    serf_connection_t *conn,
    serf_connection_framing_type_t framing_type)
{
    conn->framing_type = framing_type;

    if (conn->skt) {
        conn->dirty_conn = 1;
        conn->ctx->dirty_pollset = 1;
        conn->stop_writing = 0;
        conn->write_now = 1;

        /* Close down existing protocol */
        if (conn->protocol_baton) {
            conn->perform_teardown(conn);
            conn->protocol_baton = NULL;
        }

        /* Reset to default */
        conn->perform_read = read_from_connection;
        conn->perform_write = write_to_connection;
        conn->perform_hangup = hangup_connection;
        conn->perform_teardown = NULL;

        switch (framing_type) {
            case SERF_CONNECTION_FRAMING_TYPE_HTTP2:
                serf__http2_protocol_init(conn);
                break;
            default:
                break;
        }
    }
}

static serf_request_t *
create_request(serf_connection_t *conn,
               serf_request_setup_t setup,
               void *setup_baton,
               int priority,
               int ssltunnel)
{
    serf_request_t *request;

    request = serf_bucket_mem_alloc(conn->allocator, sizeof(*request));
    request->conn = conn;
    request->setup = setup;
    request->setup_baton = setup_baton;
    request->handler = NULL;
    request->respool = NULL;
    request->req_bkt = NULL;
    request->resp_bkt = NULL;
    request->priority = priority;
    request->writing_started = 0;
    request->ssltunnel = ssltunnel;
    request->next = NULL;
    request->auth_baton = NULL;

    return request;
}


apr_interval_time_t serf_connection_get_latency(serf_connection_t *conn)
{
    if (conn->ctx->proxy_address) {
        /* Detecting network latency for proxied connection is not implemented
           yet. */
        return -1;
    }

    return conn->latency;
}

unsigned int serf_connection_queued_requests(serf_connection_t *conn)
{
    return conn->nr_of_unwritten_reqs;
}

unsigned int serf_connection_pending_requests(serf_connection_t *conn)
{
    return conn->nr_of_unwritten_reqs + conn->nr_of_written_reqs;
}
