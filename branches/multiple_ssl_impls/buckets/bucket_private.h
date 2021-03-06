/* Copyright 2013 Justin Erenkrantz and Greg Stein
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

#ifndef _BUCKET_PRIVATE_H_
#define _BUCKET_PRIVATE_H_

typedef struct serf_ssl_bucket_type_t serf_ssl_bucket_type_t;

struct serf_ssl_bucket_type_t {
    void * (*decrypt_create)(serf_bucket_t *bucket,
                             serf_bucket_t *stream,
                             void *impl_ctx,
                             serf_bucket_alloc_t *allocator);

    void * (*decrypt_context_get)(serf_bucket_t *bucket);

    void * (*encrypt_create)(serf_bucket_t *bucket,
                             serf_bucket_t *stream,
                             void *impl_ctx,
                             serf_bucket_alloc_t *allocator);

    void * (*encrypt_context_get)(serf_bucket_t *bucket);

    /**
     * Allow SNI indicators to be sent to the server.
     */
    apr_status_t (*set_hostname)(void *impl_ctx, const char *hostname);

    void (*client_cert_provider_set)(void *impl_ctx,
                                     serf_ssl_need_client_cert_t callback,
                                     void *data,
                                     void *cache_pool);

    void (*identity_provider_set)(void *impl_ctx,
                                  serf_ssl_need_identity_t callback,
                                  void *data,
                                  void *cache_pool);

    void (*client_cert_password_set)(void *impl_ctx,
                                     serf_ssl_need_cert_password_t callback,
                                     void *data,
                                     void *cache_pool);
    /**
     * Set a callback to override the default SSL server certificate validation
     * algorithm.
     */
    void (*server_cert_callback_set)(void *impl_ctx,
                                     serf_ssl_need_server_cert_t callback,
                                     void *data);
    /**
     * Set callbacks to override the default SSL server certificate validation
     * algorithm for the current certificate or the entire certificate chain.
     */
    void (*server_cert_chain_callback_set)(void *impl_ctx,
                                           serf_ssl_need_server_cert_t cert_callback,
                                           serf_ssl_server_cert_chain_cb_t cert_chain_callback,
                                           void *data);
    /**
     * Use the default root CA certificates as included with the OpenSSL library.
     * TODO: fix comment!
     */
    apr_status_t (*use_default_certificates)(void *impl_ctx);
    
    /**
     * Load a CA certificate file from a path @a file_path. If the file was
     * loaded and parsed correctly, a certificate @a cert will be created and
     * returned.
     * This certificate object will be alloced in @a pool.
     */
    apr_status_t (*load_CA_cert_from_file)(serf_ssl_certificate_t **cert,
                                           const char *file_path,
                                           apr_pool_t *pool);

    apr_status_t (*load_identity_from_file)(void *impl_ctx,
                                            const serf_ssl_identity_t **identity,
                                            const char *file_path,
                                            apr_pool_t *pool);

    /**
     * Adds the certificate @a cert to the list of trusted certificates in
     * @a ssl_ctx that will be used for verification.
     * See also @a serf_ssl_load_cert_file.
     */
    apr_status_t (*trust_cert)(void *impl_ctx,
                               serf_ssl_certificate_t *cert);

    /**
     * Extract the fields of the issuer in a table with keys (E, CN, OU, O, L,
     * ST and C). The returned table will be allocated in @a pool.
     */
    apr_hash_t * (*cert_issuer)(const serf_ssl_certificate_t *cert,
                                apr_pool_t *pool);

    /**
     * Extract the fields of the subject in a table with keys (E, CN, OU, O, L,
     * ST and C). The returned table will be allocated in @a pool.
     */
    apr_hash_t * (*cert_subject)(const serf_ssl_certificate_t *cert,
                                 apr_pool_t *pool);

    /**
     * Extract the fields of the certificate in a table with keys (sha1,
     * notBefore, notAfter, array of subjectAltName's). The returned table will
     * be allocated in @a pool.
     */
    apr_hash_t * (*cert_certificate)(const serf_ssl_certificate_t *cert,
                                     apr_pool_t *pool);
    
    /**
     * Export a certificate to base64-encoded, zero-terminated string.
     * The returned string is allocated in @a pool. Returns NULL on failure.
     */
    const char * (*cert_export)(const serf_ssl_certificate_t *cert,
                                apr_pool_t *pool);
    /**
     * Enable or disable SSL compression on a SSL session.
     * @a enabled = 1 to enable compression, 0 to disable compression.
     * Default = disabled.
     */
    apr_status_t (*use_compression)(void *impl_ctx,
                                    int enabled);

    /**
     * Exports @a session to continous memory block.
     */
    apr_status_t (*session_export)(void **data,
                                   apr_size_t *len,
                                   const serf_ssl_session_t *session,
                                   apr_pool_t *pool);

    /**
     * Restores previously saved session from continuous memory block @a data
     * with @a len length.
     */
    apr_status_t (*session_import)(const serf_ssl_session_t **session,
                                   void *data,
                                   apr_size_t len,
                                   apr_pool_t *pool);

    /**
     * TODO: comment
     */
    void (*new_session_callback_set)(void *impl_ctx,
                                     serf_ssl_new_session_t new_session_cb,
                                     void *baton);

    /* Configure @a ssl_ctx to attempt resume exisiting @a ssl_session. */
    apr_status_t (*resume_session)(void *impl_ctx,
                                   const serf_ssl_session_t *ssl_session,
                                   apr_pool_t *pool);
};

/* Implementation independent certificate object. */
struct serf_ssl_certificate_t {
    /** bucket implementation that can parse this certificate. */
    const serf_ssl_bucket_type_t *type;

    /** implementation specific certificate data */
    /* Note: non-const, as required by OpenSSL. */
    void *impl_cert;

    /** Depth in the chain where an error was found. */
    int depth_of_error;
};

/* Loads a certificate from a memory buffer.
   Defined here so it can be used by the test suite. */
apr_status_t
load_CA_cert_from_buffer(serf_ssl_certificate_t **cert,
                         const char *buf,
                         apr_size_t len,
                         apr_pool_t *pool);

/* Creates a serf_ssl_certificate_t object, caller takes ownership. */
serf_ssl_certificate_t *
serf__create_certificate(serf_bucket_alloc_t *allocator,
                         const serf_ssl_bucket_type_t *type,
                         void *impl_cert,
                         int depth);

/* Implementation independent identity object. An identity is a combination
   of a certificate and a private key, typically stored in a .p12 file. */
struct serf_ssl_identity_t {
    /** bucket implementation that can parse this identity. */
    const serf_ssl_bucket_type_t *type;

    /** implementation specific client certificate */
    void *impl_cert;

    /** implementation specific private key */
    void *impl_pkey;
};

/* Creates a serf_ssl_identity_t object, caller takes ownership. */
serf_ssl_identity_t *
serf__create_identity(const serf_ssl_bucket_type_t *type,
                      void *impl_cert,
                      void *impl_pkey,
                      apr_pool_t *pool);

void *serf__ssl_get_impl_context(serf_ssl_context_t *ssl_ctx);

/* Implementation independent serialized SSL session object */
struct serf_ssl_session_t {
#if 0
    /* Not needed as long as each API takes a serf_ssl_context_t. */
    /** bucket implementation that can parse this session object. */
    const serf_ssl_bucket_type_t *type;
#endif

    /** implementation specific serialized SSL session object. */
    void *impl_session_obj;
};


/* macosxssl_bucket internal functions */
#ifdef SERF_HAVE_MACOSXSSL

#include <Security/SecCertificate.h>

/* macosxssl_bucket private certificate structure. Wrapper around the 
   SecCertificateRef ptr, with content the cached parsed information from the
   certificate. */
typedef struct macosxssl_certificate_t {
    SecCertificateRef certref;

    apr_hash_t *content;
} macosxssl_certificate_t;

apr_status_t
serf__macosxssl_read_X509_DER_DN(apr_hash_t **o, CFDataRef ptr,
                                 apr_pool_t *pool);

apr_status_t
serf__macosxssl_read_X509_DER_certificate(apr_hash_t **o,
                                          const macosxssl_certificate_t *cert,
                                          apr_pool_t *pool);

#endif

/* ==================================================================== */

#ifdef SERF_HAVE_OPENSSL

extern const serf_ssl_bucket_type_t serf_ssl_bucket_type_openssl;

#endif /* SERF_HAVE_OPENSSL */

/* ==================================================================== */

#if SERF_HAVE_MACOSXSSL

extern const serf_ssl_bucket_type_t serf_ssl_bucket_type_macosxssl;

#endif /* SERF_HAVE_MACOSXSSL */

/* ==================================================================== */



#endif