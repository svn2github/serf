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

/* This code implements the ssl bucket API using the services provided by Apple
   on Mac OS X 10.7+:
   - Secure Transport
   - Keychain Services
   - Certificate, Key and Trust Services.
 
   Reference documentation can be found on http://developer.apple.com .
 
   Source code can be found on http://www.opensource.apple.com . Search for the
   Security-xxxxxx package, where xxxxxx is a version number.

   Note: unfortunately, the reference documentation seems to be a specification
   more than a correct representation of the actual implementation. So
   analysis of the source code of the services is needed to understand its exact
   behavior.

   Furthermore, the implementation of the services has some bugs that needed
   workarounds. This page is very helpful in identifying these bugs:
   https://github.com/lorentey/LKSecurity/blob/master/Framework%20Bugs.markdown
 
 
   A macosxssl_bucket implements following API's
   - a serf_bucket_t for encryption
   - a serf_bucket_t for decryption
   - a serf_ssl_bucket_type_t for certificate handling, general SSL/TLS stuff.
   - extra serf_macosxssl_ functions to integrate with Keychain.
 */

#ifdef SERF_HAVE_MACOSXSSL

#include "serf.h"
#include "serf_private.h"
#include "serf_bucket_util.h"
#include "bucket_private.h"

#include <apr_strings.h>
#include <apr_base64.h>
#include <apr_file_io.h>

#include <Security/SecureTransport.h>
#include <Security/SecPolicy.h>
#include <Security/SecImportExport.h>
#include <Security/SecIdentity.h>
#include <Security/SecItem.h>
#include <objc/runtime.h>
#include <objc/message.h>

/* The minimum amount of data we try to read and decrypt in one pass. */
#define READ_BUFSIZE 8096

typedef struct macosxssl_ssl_stream_t {
    /* For an encrypt stream: data encrypted & not yet written to the network.
       For a decrypt stream: data decrypted & not yet read by the application.*/
    serf_bucket_t *pending;

    /* For an encrypt stream: the outgoing data provided by the application.
       For a decrypt stream: encrypted data read from the network. */
    serf_bucket_t *stream;
} macosxssl_ssl_stream_t;


/* States for the different stages in the lifecyle of an SSL session. */
typedef enum {
    SERF_MACOSXSSL_INIT,       /* no SSL handshake yet */
    SERF_MACOSXSSL_HANDSHAKE,  /* SSL handshake in progress */
    SERF_MACOSXSSL_CONNECTED,  /* SSL handshake successfully finished */
    SERF_MACOSXSSL_CLOSING,    /* SSL session closing */
} macosxssl_session_state_t;

typedef struct macosxssl_context_t {
    /* How many open buckets refer to this context. */
    int refcount;

    /* Pool that stays alive during the whole ssl session */
    apr_pool_t *pool;

    /* Pool that is alive only during the hanshake phase. */
    apr_pool_t *handshake_pool;

    serf_bucket_alloc_t *allocator;

    SSLContextRef st_ctxr;

    /* Temporary keychain created when importing a client certificate. */
    SecKeychainRef tempKeyChainRef;
    char *keychain_temp_file;

    /* Trust object created during validation of the server certificate. */
    SecTrustRef trust;

    /* stream of (to be) encrypted data, outgoing to the network. */
    macosxssl_ssl_stream_t encrypt;

    /* stream of (to be) decrypted data, read from the network. */
    macosxssl_ssl_stream_t decrypt;

    macosxssl_session_state_t state;

    /* name of the peer, used with TLS's Server Name Indication extension. */
    char *hostname;

    /* Client cert callbacks */
    serf_ssl_need_client_cert_t client_cert_callback;
    serf_ssl_need_identity_t identity_callback;
    void *identity_userdata;
#if 0
    apr_pool_t *client_cert_cache_pool;
    const char *cert_file_success;
#endif

    /* Client cert PW callbacks */
    serf_ssl_need_cert_password_t identity_pw_callback;
    void *identity_pw_userdata;
#if 0
    apr_pool_t *cert_pw_cache_pool;
    const char *cert_pw_success;
#endif

    /* Server cert callbacks */
    serf_ssl_need_server_cert_t server_cert_callback;
    serf_ssl_server_cert_chain_cb_t server_cert_chain_callback;
    void *server_cert_userdata;

    /* Flag, if set use the root certificates stored in the System keychain
       when evaluating server certificates. */
    int use_system_roots;

    /* cache of the trusted certificates, added via serf_ssl_trust_cert(). */
    apr_array_header_t *anchor_certs;
    CFArrayRef anchor_certrefs;

    /* Flag set when an asynchronous evaluation of the server certificate chain
       is ongoing. */
    int evaluate_in_progress;

    /* Result of the evaluation of the server certificate chain. */
    SecTrustResultType result;
} macosxssl_context_t;

/* Some forward declarations */
static apr_status_t
serf__macosxssl_load_identity_from_file(void *impl_ctx,
                                        const serf_ssl_identity_t **identity,
                                        const char *file_path,
                                        apr_pool_t *pool);


/* Copies the unicode string from a CFStringRef to a new buffer allocated
 from pool. */
static const char *
CFStringToChar(CFStringRef str, apr_pool_t *pool)
{
    const char *ptr = CFStringGetCStringPtr(str, kCFStringEncodingMacRoman);

    if (ptr == NULL) {
        const int strlen = CFStringGetLength(str) * 2;
        char *buf = apr_pcalloc(pool, strlen);
        if (CFStringGetCString(str, buf, strlen, kCFStringEncodingMacRoman))
            return buf;
    } else {
        return apr_pstrdup(pool, ptr);
    }

    return NULL;
}

static apr_status_t
translate_macosxssl_status(OSStatus osstatus)
{
    apr_status_t status;

    switch (osstatus)
    {
        case noErr:
            return APR_SUCCESS;
        case errSSLWouldBlock:
            return APR_EAGAIN;
        case errSSLClosedGraceful:
            /* Server sent a */
            status = APR_EOF;
            break;
        case errSSLClosedAbort:
            status = APR_ECONNABORTED;
            break;
        default:
            status = APR_EGENERAL;
    }

#if SSL_VERBOSE
    {
        apr_pool_t *temppool;
        CFStringRef errref = SecCopyErrorMessageString(osstatus, NULL);
        apr_pool_create(&temppool, NULL);

        serf__log(SSL_VERBOSE, __FILE__,
                  "Unknown Secure Transport error: %d,%s.\n",
                  osstatus, CFStringToChar(errref, temppool));
        apr_pool_destroy(temppool);
    }
#endif

    return status;
}

static apr_status_t cfrelease_ref(void *data)
{
    CFTypeRef tr = data;

    if (tr)
        CFRelease(tr);

    return APR_SUCCESS;
}

static apr_status_t cfrelease_trust(void *data)
{
    macosxssl_context_t *ssl_ctx = data;

    if (ssl_ctx->trust)
        CFRelease(ssl_ctx->trust);
    ssl_ctx->trust = NULL;

    return APR_SUCCESS;
}

/**
 * Note for both read and write callback functions, from SecureTransport.h:
 * "Data's memory is allocated by caller; on entry to these two functions
 *  the *length argument indicates both the size of the available data and the
 *  requested byte count. Number of bytes actually transferred is returned in
 *  *length."
 **/

/** Secure Transport callback function.
    Reads encrypted data from the network. **/
static OSStatus
macosxssl_read_cb(SSLConnectionRef connection,
                  void *data,
                  size_t *dataLength)
{
    const macosxssl_context_t *ssl_ctx = connection;
    apr_status_t status = 0;
    const char *buf;
    char *outbuf = data;
    size_t requested = *dataLength, buflen = 0;

    serf__log(SSL_VERBOSE, __FILE__, "macosxssl_read_cb called for "
              "%d bytes.\n", requested);

    *dataLength = 0;
    while (!status && requested) {
        status = serf_bucket_read(ssl_ctx->decrypt.stream, requested,
                                  &buf, &buflen);

        if (SERF_BUCKET_READ_ERROR(status)) {
            serf__log(SSL_VERBOSE, __FILE__, "Returned status %d.\n", status);
            return -1;
        }

        serf__log(SSL_VERBOSE, __FILE__, "Read %d bytes with status %d.\n",
                  buflen, status);

        if (buflen) {
            /* Copy the data in the buffer provided by the caller. */
            memcpy(outbuf, buf, buflen);
            outbuf += buflen;
            requested -= buflen;
            (*dataLength) += buflen;
        }
    }

    if (APR_STATUS_IS_EAGAIN(status))
        return errSSLWouldBlock;

    if (!status)
        return noErr;

    /* TODO: map apr status to Mac OS X error codes(??) */
    return -1;
}

/** Secure Transport callback function.
    Writes encrypted data to the network. **/
static OSStatus
macosxssl_write_cb(SSLConnectionRef connection,
                   const void *data,
                   size_t *dataLength)
{
    serf_bucket_t *tmp;
    const macosxssl_context_t *ctx = connection;

    serf__log(SSL_VERBOSE, __FILE__, "macosxssl_write_cb called for "
              "%d bytes.\n", *dataLength);

    tmp = serf_bucket_simple_copy_create(data, *dataLength,
                                         ctx->encrypt.pending->allocator);

    serf_bucket_aggregate_append(ctx->encrypt.pending, tmp);

    return noErr;
}

#pragma mark VALIDATE SERVER CERTIFICATES
#pragma mark -

/* Creates a macosxssl_certificate_t allocated on pool. */
static apr_status_t
create_macosxssl_certificate(macosxssl_certificate_t **out_macosxssl_cert,
                             SecCertificateRef certref,
                             int parse_content,
                             apr_pool_t *pool)
{
    macosxssl_certificate_t *macosxssl_cert;
    apr_status_t status = APR_SUCCESS;

    macosxssl_cert = apr_pcalloc(pool, sizeof(macosxssl_certificate_t));
    macosxssl_cert->certref = certref;

    if (parse_content)
        status = serf__macosxssl_read_X509_DER_certificate(
                           &macosxssl_cert->content,
                           macosxssl_cert,
                           pool);
    *out_macosxssl_cert = macosxssl_cert;

    return status;
}

/* Creates a serf_ssl_certificate_t at depth allocated on pool. */
static serf_ssl_certificate_t *
create_ssl_certificate(SecCertificateRef certref,
                       int depth,
                       apr_pool_t *pool)
{
    macosxssl_certificate_t *macosxssl_cert;
    serf_bucket_alloc_t *allocator;

    /* Since we're not asking to parse the content we can ignore the status. */
    (void) create_macosxssl_certificate(&macosxssl_cert, certref, 0, pool);

    allocator = serf_bucket_allocator_create(pool, NULL, NULL);
    return serf__create_certificate(allocator,
                                    &serf_ssl_bucket_type_macosxssl,
                                    macosxssl_cert,
                                    depth);
}

/* Read the contents of a file in memory in a CFDataRef buffer. */
static apr_status_t
load_data_from_file(const char *file_path, CFDataRef *databuf, apr_pool_t *pool)
{
    apr_file_t *fp;
    apr_finfo_t file_info;
    apr_size_t len;
    char *buf;
    apr_status_t status;

    status = apr_file_open(&fp, file_path,
                           APR_FOPEN_READ | APR_FOPEN_BINARY,
                           APR_FPROT_OS_DEFAULT, pool);
    if (status)
        return status;

    /* Read the file in memory */
    apr_file_info_get(&file_info, APR_FINFO_SIZE, fp);
    buf = apr_palloc(pool, file_info.size);

    status = apr_file_read_full(fp, buf, file_info.size, &len);
    if (status)
        return status;

    *databuf = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                           (unsigned char *)buf,
                                           file_info.size,
                                           kCFAllocatorNull);

    apr_file_close(fp);

    return APR_SUCCESS;
}

/* Use Keychain Services to extract one or multiple SecCertificateRef's from
   a data buffer.
 */
static apr_status_t
load_certificate_from_databuf(CFDataRef databuf,
                              CFArrayRef *items,
                              apr_pool_t *pool)
{
    SecExternalItemType itemType;
    OSStatus osstatus;
    apr_status_t status = APR_SUCCESS;

    osstatus = SecItemImport(databuf, NULL,
                             kSecFormatUnknown,
                             &itemType,
                             0,    /* SecItemImportExportFlags */
                             NULL, /* SecItemImportExportKeyParameters */
                             NULL, /* SecKeychainRef */
                             items);
    if (osstatus != errSecSuccess)
    {
        /* TODO: should be handled in translate_... */
        status = SERF_ERROR_SSL_CERT_FAILED;
    }
    
    return status;
}

/* Logs the issuer and subject of cert. */
static void
log_certificate(macosxssl_certificate_t *cert, const char *msg)
{
#if SSL_VERBOSE
    apr_hash_t *subject, *issuer;
    apr_pool_t *tmppool;

    apr_pool_create(&tmppool, NULL);
    if (!cert->content) {
        apr_status_t status;
        status = serf__macosxssl_read_X509_DER_certificate(&cert->content,
                                                           cert,
                                                           tmppool);
        if (status)
            goto cleanup;
    }

    subject = (apr_hash_t *)apr_hash_get(cert->content,
                                         "subject", APR_HASH_KEY_STRING);

    serf__log(SSL_VERBOSE, __FILE__, msg);
    serf__log(SSL_VERBOSE, __FILE__, "Subject:\n");
    serf__log(SSL_VERBOSE, __FILE__, " CN:%s,",
                     apr_hash_get(subject, "CN", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " OU:%s,",
                     apr_hash_get(subject, "OU", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " O:%s,",
                     apr_hash_get(subject, "O", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " L:%s,",
                     apr_hash_get(subject, "L", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " ST:%s,",
                     apr_hash_get(subject, "ST", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " C:%s,",
                     apr_hash_get(subject, "C", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " E:%s\n",
                     apr_hash_get(subject, "E", APR_HASH_KEY_STRING));

    issuer = (apr_hash_t *)apr_hash_get(cert->content,
                                        "issuer", APR_HASH_KEY_STRING);

    serf__log(SSL_VERBOSE, __FILE__, "Issuer:\n");
    serf__log(SSL_VERBOSE, __FILE__, " CN:%s,",
              apr_hash_get(issuer, "CN", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " OU:%s,",
                     apr_hash_get(issuer, "OU", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " O:%s,",
                     apr_hash_get(issuer, "O", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " L:%s,",
                     apr_hash_get(issuer, "L", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " ST:%s,",
                     apr_hash_get(issuer, "ST", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " C:%s,",
                     apr_hash_get(issuer, "C", APR_HASH_KEY_STRING));
    serf__log_nopref(SSL_VERBOSE, " E:%s\n",
                     apr_hash_get(issuer, "E", APR_HASH_KEY_STRING));

cleanup:
    apr_pool_destroy(tmppool);
#endif
}

/* Finds the issuer certificate of cert in the provided list of 
   SecCertificateRef's. *outcert is allocated in pool. */
static apr_status_t
find_issuer_cert_in_array(serf_ssl_certificate_t **outcert,
                          macosxssl_certificate_t *cert,
                          CFArrayRef certref_list,
                          apr_pool_t *pool)
{
    CFDataRef issuer;
    apr_pool_t *tmppool;
    serf_bucket_alloc_t *tmpalloc;
    int i;
    apr_status_t status;

    apr_pool_create(&tmppool, pool);
    tmpalloc = serf_bucket_allocator_create(tmppool, NULL, NULL);

    /* Get the issuer DER encoded data buffer of the provided certificate. */
    log_certificate(cert, "Search for issuer of this cert:\n");
    issuer = apr_hash_get(cert->content, "_issuer_der", APR_HASH_KEY_STRING);

    /* Get the subject DER encoded data buffer of each cert in the list and
       compare it with the issuer data buffer. */
    for (i = 0; i < CFArrayGetCount(certref_list); i++)
    {
        macosxssl_certificate_t *list_cert;
        CFDataRef subject;
        SecCertificateRef certref;

        certref = (SecCertificateRef)CFArrayGetValueAtIndex(certref_list, i);
        status = create_macosxssl_certificate(&list_cert, certref, 1,
                                              tmppool);
        if (status)
            goto cleanup;

        subject = apr_hash_get(list_cert->content, "_subject_der",
                               APR_HASH_KEY_STRING);

        if (CFEqual(subject, issuer))
        {
            CFTypeRef outcertref;

            /* This is the one. */
            outcertref = CFArrayGetValueAtIndex(certref_list, i);

            *outcert = create_ssl_certificate((SecCertificateRef)outcertref,
                                              i,
                                              pool);
            status = APR_SUCCESS;
            goto cleanup;
        }
    }

    /* Nothing found. */
    status = SERF_ERROR_SSL_CERT_FAILED;

cleanup:
    apr_pool_destroy(tmppool);
    return status;
}

/* Certificate validation errors are only available as string. Convert them
   to serf's failure codes. */
static int
convert_certerr_to_failure(const char *errstr)
{
    if (strcmp(errstr, "CSSMERR_TP_INVALID_ANCHOR_CERT") == 0)
        return SERF_SSL_CERT_SELF_SIGNED;
    if (strcmp(errstr, "CSSMERR_TP_CERT_EXPIRED") == 0)
        return SERF_SSL_CERT_EXPIRED;
    if (strcmp(errstr, "CSSMERR_TP_CERT_NOT_VALID_YET") == 0)
        return SERF_SSL_CERT_NOTYETVALID;
    if ((strcmp(errstr, "CSSMERR_TP_NOT_TRUSTED") == 0) ||
        (strcmp(errstr, "CSSMERR_TP_VERIFICATION_FAILURE") == 0))
        return SERF_SSL_CERT_UNKNOWNCA;

    return SERF_SSL_CERT_UNKNOWN_FAILURE;
}

/* Validate a server certificate. Call back to the application if needed.
   Returns APR_SUCCESS if the server certificate is accepted.
   Otherwise returns an error.
 */
static int
validate_server_certificate(macosxssl_context_t *ssl_ctx)
{
    CFArrayRef anchor_certrefs = NULL;
    size_t depth_of_error, chain_depth;
    int failures = 0;
    OSStatus osstatus;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__, "validate_server_certificate called.\n");
    osstatus = SSLCopyPeerTrust(ssl_ctx->st_ctxr, &ssl_ctx->trust);
    if (osstatus != noErr) {
        status = translate_macosxssl_status(osstatus);
        goto cleanup;
    }
    apr_pool_cleanup_register(ssl_ctx->handshake_pool, ssl_ctx,
                              cfrelease_trust, cfrelease_trust);

    if (!ssl_ctx->evaluate_in_progress)
    {
        CFArrayRef root_certrefs = NULL;
        void *macosxssl_cls;
        id tmp;

        /* If the application provided certificates to trust, use them here. */
        if (ssl_ctx->anchor_certs)
        {
            int anchor_certs = ssl_ctx->anchor_certs->nelts;
            int i;
            SecCertificateRef certs[anchor_certs];

            for (i = 0; i < anchor_certs; i++)
                certs[i] = APR_ARRAY_IDX(ssl_ctx->anchor_certs, i,
                                         SecCertificateRef);

            anchor_certrefs = CFArrayCreate(kCFAllocatorDefault,
                                            (void *)certs,
                                            anchor_certs,
                                            NULL);
            ssl_ctx->anchor_certrefs = anchor_certrefs;
        }

        /* If we can use the system's default root certificates, copy them
           here. */
        if (ssl_ctx->use_system_roots)
        {
            osstatus = SecTrustCopyAnchorCertificates(&root_certrefs);
            if (osstatus != noErr) {
                status = translate_macosxssl_status(osstatus);
                goto cleanup;
            }
        }

        /* Make one list of custom anchor and system root certificates and
           add them to the trust object to use them during evaluation of the
           server certificate chain. */
        if (anchor_certrefs || root_certrefs)
        {
            CFIndex anchors, roots;
            CFMutableArrayRef all_certrefs;

            anchors = anchor_certrefs ? CFArrayGetCount(anchor_certrefs) : 0;
            roots = root_certrefs ? CFArrayGetCount(root_certrefs) : 0;

            all_certrefs = CFArrayCreateMutable(kCFAllocatorDefault,
                                                anchors + roots, NULL);
            if (anchor_certrefs)
                CFArrayAppendArray(all_certrefs, anchor_certrefs,
                                   CFRangeMake(0, anchors));
            if (root_certrefs)
                CFArrayAppendArray(all_certrefs, root_certrefs,
                                   CFRangeMake(0, roots));

            osstatus = SecTrustSetAnchorCertificates(ssl_ctx->trust,
                                                     all_certrefs);
            if (osstatus != noErr) {
                status = translate_macosxssl_status(osstatus);
                goto cleanup;
            }
        }

        ssl_ctx->evaluate_in_progress = 1;

        macosxssl_cls = objc_getClass("macosxssl_Buckets");
        tmp = objc_msgSend(macosxssl_cls,
                           sel_getUid("evaluate:trustResult:"),
                           ssl_ctx->trust,
                           &ssl_ctx->result);
        osstatus = (OSStatus)(SInt64)tmp;
        if (osstatus != noErr) {
            status = translate_macosxssl_status(osstatus);
            goto cleanup;
        }
    }

    if (!ssl_ctx->result) {
        /* No evaluation results received yet. */
        return APR_EAGAIN;
    }

    /* Based on the contents of the user's Keychain, Secure Transport will make
       a first validation of this certificate chain.
       The status set here can in some cases be overridden by the application.*/
    switch (ssl_ctx->result)
    {
        /* kSecTrustResultProceed Indicates you may proceed.
           User trusts this certificate, so continue without calling back to
           the application. */
        case kSecTrustResultProceed:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultProceed.\n");
            status = APR_SUCCESS;
            goto cleanup;

        /* Non-fatal errors, application decides. */

        /* kSecTrustResultUnspecified Indicates user intent is unknown.
           (certificate is valid, but user didn't specify trust) */
        case kSecTrustResultUnspecified:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultUnspecified.\n");
            status = APR_SUCCESS;
            failures = 0;
            break;

        /* kSecTrustResultConfirm Indicates confirmation with the user is
           required before proceeding. */
        case kSecTrustResultConfirm:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultConfirm.\n");
            status = SERF_ERROR_SSL_CERT_FAILED;
            failures = SERF_SSL_CERT_CONFIRM_NEEDED;
            break;

        /* kSecTrustResultRecoverableTrustFailure Indicates a trust framework
           failure; retry after fixing inputs. */
        case kSecTrustResultRecoverableTrustFailure:
            serf__log(SSL_VERBOSE, __FILE__,
                      "kSecTrustResultRecoverableTrustFailure.\n");
            status = SERF_ERROR_SSL_CERT_FAILED;
            failures = 0; /* Failure info will be added later */
            break;

        /* Fatal errors. Since they cannot be overridden, don't call back to the
           application but return with an error. */

        /* kSecTrustResultInvalid Indicates an invalid setting or result. */
        case kSecTrustResultInvalid:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultInvalid.\n");
            status = SERF_ERROR_SSL_FATAL_CERT_INVALID;
            goto cleanup;

        /* kSecTrustResultDeny Indicates a user-configured deny; do not
           proceed. */
        case kSecTrustResultDeny:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultDeny.\n");
            status = SERF_ERROR_SSL_FATAL_CERT_DENIED_IN_KEYCHAIN;
            goto cleanup;

        /* kSecTrustResultFatalTrustFailure Indicates a trust framework failure;
           no "easy" fix. */
        case kSecTrustResultFatalTrustFailure:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultFatalTrustFailure.\n");
            status = SERF_ERROR_SSL_FATAL_CERT_TRUST_FAILURE;
            goto cleanup;

        /* kSecTrustResultOtherError Indicates a failure other than that of
           trust evaluation. */
        case kSecTrustResultOtherError:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultOtherError.\n");
            status = SERF_ERROR_SSL_FATAL_CERT_FAILED;
            goto cleanup;
        default:
            serf__log(SSL_VERBOSE, __FILE__, "unknown result from trust "
                      "evaluation.\n");
            status = SERF_ERROR_SSL_FATAL_CERT_FAILED;
            goto cleanup;
    }

    /* Secure Transport only reports one error per evaluation. This is stored
       at depth 0 in the result array, so we don't even know at what depth
       the error occurred.
       Get the total chain length (incuding anchor) from
       SecTrustCopyProperties. */
    {
        CFArrayRef props = SecTrustCopyProperties(ssl_ctx->trust);
        CFDictionaryRef dict = CFArrayGetValueAtIndex(props, 0);
        CFStringRef errref = CFDictionaryGetValue(dict, kSecPropertyTypeError);

        chain_depth = CFArrayGetCount(props); /* length of the full chain,
                                                 including anchor cert. */

        if (errref) {
            apr_pool_t *tmppool;
            const char *errstr;

            apr_pool_create(&tmppool, NULL);
            errstr = CFStringToChar(errref, tmppool);

            failures |= convert_certerr_to_failure(errstr);
            serf__log(SSL_VERBOSE, __FILE__,
                      "Certificate ERROR: %s in chain of length %d.\n", errstr,
                      chain_depth);
            apr_pool_destroy(tmppool);
        } else {
            serf__log(SSL_VERBOSE, __FILE__, "No certificate validation errors "
                      "found.\n");
        }

        CFRelease(props);
    }

    /* TODO: 0, oh really? How can we know where the error occurred? */
    depth_of_error = 0;

    /* Ask the application to validate the server certificate at depth 0.
       TODO: any certificate at other depths with failures. */
    if (ssl_ctx->server_cert_callback)
    {
        serf_ssl_certificate_t *cert;
        SecCertificateRef certref;

        serf__log(SSL_VERBOSE, __FILE__, "Call application for server cert "
                  "validation.\n");

        certref = SecTrustGetCertificateAtIndex(ssl_ctx->trust, 0);
        cert = create_ssl_certificate(certref, 0, ssl_ctx->handshake_pool);

        /* Callback for further verification. */
        status = ssl_ctx->server_cert_callback(ssl_ctx->server_cert_userdata,
                                               failures, cert);
        serf__log(SSL_VERBOSE, __FILE__, "Application returned status %d.\n",
                  status);
    }

    /* We need to get the full certificate chain and provide it to the
       application.

       There are 4 scenario's:
       1. The server provided all certificates including the root CA.
       2. The server provided all certificates except the anchor certificate.
          The anchor certificate is stored in a Keychain. (a root CA provided
          by Apple or a certificate imported in a Keychain by the user).
       3. The server provided all certificates except the anchor certificate.
          The anchor certificate was explicitly trusted by the application via
          serf_ssl_trust_cert.
       4. The server provided some certificates but not the root CA. This cert
          is not available in the Keychain nor in the trusted list set by the
          application.

       The Keychain API gives us multiple options to get the full chain in
       scenario 1, 2 and 4. However, when the anchor certificate was provided
       by the application, it's not included in the chain returned by the
       Keychain API.
     
       We get the total chain length from SecTrustCopyProperties. We get the
       certificate chain from the trust object via SecTrustGetCertificateCount
       and SecTrustGetCertificateAtIndex. If the length of the certificate
       chain is one shorter than the expected total chain length, we know we're
       in scenario 3.
     */
    if (ssl_ctx->server_cert_chain_callback)
    {
        serf_ssl_certificate_t **certs;
        int certs_len, actual_len, i;

        serf__log(SSL_VERBOSE, __FILE__, "Call application for server cert "
                  "chain validation.\n");

        /* Room for the total chain length and a trailing NULL.  */
        certs = apr_palloc(ssl_ctx->handshake_pool,
                           sizeof(*certs) * (chain_depth  + 1));

        /* Copy the certificates as provided by the server + Keychain. */
        certs_len = SecTrustGetCertificateCount(ssl_ctx->trust);
        for (i = 0; i < certs_len; ++i)
        {
            SecCertificateRef certref;

            certref = SecTrustGetCertificateAtIndex(ssl_ctx->trust, i);
            certs[i] = create_ssl_certificate(certref, i,
                                              ssl_ctx->handshake_pool);
        }

        actual_len = certs_len;
        if (chain_depth > certs_len)
        {
            /* The chain relies on (root) CA certificates not provided by the
               server or a Keychain (scenario 3). We have to find them in the
               list of trusted anchor certificates.
             */
            SecCertificateRef certref;
            macosxssl_certificate_t *cert;

            serf__log(SSL_VERBOSE, __FILE__, "Chain length (%d) is longer than "
                      "what we received from the server (%d). Search the "
                      "remaining anchor certificate.\n",
                      chain_depth, certs_len);

            /* Take the last known certificate and search its issuer in the
               list of trusted anchor certificates. */
            certref = SecTrustGetCertificateAtIndex(ssl_ctx->trust,
                                                    certs_len - 1);
            status = create_macosxssl_certificate(&cert, certref, 1,
                                                  ssl_ctx->handshake_pool);

            status = find_issuer_cert_in_array(&certs[certs_len],
                                               cert,
                                               ssl_ctx->anchor_certrefs,
                                               ssl_ctx->handshake_pool);
            if (!status)
                actual_len++;
        }

        status =
            ssl_ctx->server_cert_chain_callback(ssl_ctx->server_cert_userdata,
                failures, 0, /*depth_of_error,*/
                (const serf_ssl_certificate_t * const *)certs,
                actual_len);
    }

    /* Return a specific error if the server certificate is not accepted by
       S.T./Keychain and the application has not set callbacks to override
       this. */
    if (failures &&
        !ssl_ctx->server_cert_chain_callback &&
        !ssl_ctx->server_cert_callback)
    {
        status = SERF_ERROR_SSL_CERT_FAILED;
    }

cleanup:
    if (anchor_certrefs)
        CFRelease(anchor_certrefs);

    return status;
}

/**** Client identity related code ****/
/**************************************/
#pragma mark NEED CLIENT IDENTITY 
#pragma mark -

static apr_status_t delete_temp_keychain(void *data)
{
    macosxssl_context_t *ssl_ctx = data;
    apr_status_t status = APR_SUCCESS;
    OSStatus osstatus;

    if (!ssl_ctx->tempKeyChainRef)
        return APR_SUCCESS;

    osstatus = SecKeychainDelete(ssl_ctx->tempKeyChainRef);
    if (osstatus != errSecSuccess) {
        status = translate_macosxssl_status(osstatus);
    }
    ssl_ctx->tempKeyChainRef = NULL;

    return status;
}

static apr_status_t create_temp_keychain(macosxssl_context_t *ssl_ctx,
                                         apr_pool_t *pool)
{
    apr_file_t *tmpfile;
    const char *temp_dir;
    apr_status_t status;
    OSStatus osstatus;

    if (ssl_ctx->tempKeyChainRef)
        return APR_SUCCESS;

    /* The Keychain API only allows us to load an identity (private key +
       certificate) for use in the SetCertificate call in a keychain.
       We don't want to load this identity in the login or system keychain,
       so we need to create a temporary keychain.

       For the duration of the SSL handshake, this keychain will be visible to
       the user in the Keychain Access tool.
     */

    /* We need a unique filename for a temporary file. So create an empty
       file using APR and close it immediately. */
    status = apr_temp_dir_get(&temp_dir, pool);
    if (status)
        return status;

    status = apr_filepath_merge(&ssl_ctx->keychain_temp_file,
                                temp_dir,
                                "tempfile_XXXXXX",
                                APR_FILEPATH_NATIVE | APR_FILEPATH_NOTRELATIVE,
                                pool);
    if (status)
        return status;

    status = apr_file_mktemp(&tmpfile, ssl_ctx->keychain_temp_file,
                             APR_READ | APR_WRITE | APR_CREATE | APR_EXCL |
                                 APR_DELONCLOSE | APR_BINARY,
                             pool);
    if (status)
        return status;

    status = apr_file_close(tmpfile);
    if (status)
        return status;

    serf__log(SSL_VERBOSE, __FILE__, "Creating temporary keychain in %s.\n",
              ssl_ctx->keychain_temp_file);

    /* TODO: random password */
    /* TODO: standard access rights gives unlimited access to the keychain for
       this application. Other applications can also access the keychain,
       but require confirmation from the user (no pwd needed). Probably better
       if the keychain is locked down so that e.g. searches for identity
       objects don't return any from this keychain. */
    osstatus = SecKeychainCreate(ssl_ctx->keychain_temp_file,
                                 4,
                                 "serf",
                                 FALSE,
                                 NULL, /* Standard access rights */
                                 &ssl_ctx->tempKeyChainRef);
    if (osstatus != errSecSuccess) {
        return translate_macosxssl_status(osstatus);
    }
    apr_pool_cleanup_register(pool, ssl_ctx,
                              delete_temp_keychain, delete_temp_keychain);

    return APR_SUCCESS;
}

/* Find the certificate of the issuer of certref in the keychains. */
static apr_status_t
find_issuer_certificate_in_keychain(macosxssl_certificate_t **out_cert,
                                    SecCertificateRef certref,
                                    apr_pool_t *pool)
{
    CFErrorRef error = NULL;
    CFDataRef issuer;
    apr_pool_t *tmppool;
    apr_status_t status;

    apr_pool_create(&tmppool, pool);

    issuer = SecCertificateCopyNormalizedIssuerContent(certref,
                                                       &error);
    if (error) {
        CFStringRef errstr = CFErrorCopyDescription(error);

        serf__log(SSL_VERBOSE, __FILE__, "Can't get issuer DER buffer from "
                  "certificate, reason: %s.\n",
                  CFStringToChar(errstr, tmppool));
        CFRelease(error);

        return SERF_ERROR_SSL_CERT_FAILED;
    }
    else
    {
        CFDictionaryRef query;
        macosxssl_certificate_t *cert, *issuer_cert;
        SecCertificateRef issuer_certref;
        CFDataRef cert_issuer, issuer_subject;
        OSStatus osstatus;

        const void *keys[] =   { kSecClass, kSecAttrSubject,
                                 kSecMatchLimit, kSecReturnRef };
        const void *values[] = { kSecClassCertificate, issuer,
                                 kSecMatchLimitOne, kCFBooleanTrue };

        /* Find a certificate with issuer in the keychains. */
        query = CFDictionaryCreate(kCFAllocatorDefault,
                                   (const void **)keys, (const void **)values,
                                   4L, NULL, NULL);
        osstatus = SecItemCopyMatching(query, (CFTypeRef*)&issuer_certref);
        CFRelease(query);
        CFRelease(issuer);
        if (osstatus != errSecSuccess) {
            return translate_macosxssl_status(osstatus);
        }

        /* if SecItemCopyMatching doesn't find a matching certificate, it is
           known that it returns another (no kidding), so check that we received
           the right certificate.
         */
        status = create_macosxssl_certificate(&cert,
                                              certref,
                                              1,
                                              pool);
        if (status)
            return status;
        status = create_macosxssl_certificate(&issuer_cert,
                                              issuer_certref,
                                              1,
                                              pool);
        if (status)
            return status;

        cert_issuer =  apr_hash_get(cert->content, "_issuer_der",
                                     APR_HASH_KEY_STRING);
        issuer_subject =  apr_hash_get(issuer_cert->content, "_subject_der",
                                       APR_HASH_KEY_STRING);

        if (CFEqual(cert_issuer, issuer_subject))
        {
            *out_cert = issuer_cert;
            return APR_SUCCESS;
        }

        *out_cert = NULL;

        return APR_SUCCESS;
    }
}

/* Create a list of all certificates in between certref and any anchor
   certificate in the list of anchor_certrefs.
   Caller is responsible for cleanin up *intermediate_ca_certrefs. */
static apr_status_t
find_intermediate_cas(CFArrayRef *intermediate_ca_certrefs,
                      SecCertificateRef certref,
                      CFArrayRef peer_certrefs,
                      apr_pool_t *pool)
{
    macosxssl_certificate_t *prevcert;
    CFMutableArrayRef ca_certrefs;
    apr_pool_t *tmppool;
    apr_status_t status;

    ca_certrefs = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);

    if (peer_certrefs == NULL ||
        CFArrayGetCount(peer_certrefs) == 0)
    {
        /* The server didn't provide any certificate at all?? */
        *intermediate_ca_certrefs = ca_certrefs;

        return APR_SUCCESS;
    }

    apr_pool_create(&tmppool, pool);

    status = create_macosxssl_certificate(&prevcert, certref, 1, tmppool);
    if (status)
        goto cleanup;

    /* Get the issuer DER encoded data buffer of the provided certificate. */
    while (1)
    {
        macosxssl_certificate_t *issuer_cert;
        CFDataRef issuer, subject;
        serf_ssl_certificate_t *dummy_cert;

        /* Find issuer in the list of certificates sent by the server. */
        status = find_issuer_cert_in_array(&dummy_cert,
                                           prevcert,
                                           peer_certrefs,
                                           tmppool);
        if (status == APR_SUCCESS)
            goto cleanup;
#if 0
        /* We have to send the certificate to the server. Find issuer in the
           list of anchor certificates set by the application. */
        status = find_issuer_cert_in_array(&cert,
                                           prevcert,
                                           ssl_ctx->anchor_certs,
                                           tmppool)
        if (status == APR_SUCCESS)
            goto cleanup;
#endif

        /* Issuer certificate was not found in peer_certs, add it to the
           output list. */
        status = find_issuer_certificate_in_keychain(&issuer_cert,
                                                     prevcert->certref,
                                                     pool);
        if (status)
            goto cleanup;

        if (!issuer_cert)
        {
            /* The issuer's certificate was not found in the keychain.
               Send what we have to the server. */
            status = APR_SUCCESS;
            goto cleanup;
        }

        CFArrayAppendValue(ca_certrefs, issuer_cert->certref);

        prevcert = issuer_cert;

        /* break if selfsigned */
        subject = apr_hash_get(issuer_cert->content, "_subject_der",
                               APR_HASH_KEY_STRING);
        issuer = apr_hash_get(issuer_cert->content, "_issuer_der",
                               APR_HASH_KEY_STRING);
        if (CFEqual(subject, issuer))
        {
            status = APR_SUCCESS;
            goto cleanup;
        }
    }

    /* Nothing found. */
    status = SERF_ERROR_SSL_CERT_FAILED;
    
cleanup:
    *intermediate_ca_certrefs = ca_certrefs;
    apr_pool_destroy(tmppool);
    return status;
}

static apr_status_t
callback_for_identity_password(macosxssl_context_t *ssl_ctx,
                               const char *cert_path,
                               const char **passphrase)
{
    if (ssl_ctx->identity_pw_callback)
    {
        apr_status_t status;

        status = ssl_ctx->identity_pw_callback(ssl_ctx->identity_pw_userdata,
                                               cert_path,
                                               passphrase);
        return status;
    }

    return SERF_ERROR_SSL_CLIENT_CERT_PW_FAILED;
}

static apr_status_t
callback_for_identity(macosxssl_context_t *ssl_ctx,
                      apr_hash_t **dnlist,
                      apr_size_t dnlen,
                      SecIdentityRef *identityref,
                      apr_pool_t *pool)
{
    apr_status_t status;
    const serf_ssl_identity_t *identity;

    if (ssl_ctx->client_cert_callback)
    {
        const char *cert_path;

        status = ssl_ctx->client_cert_callback(ssl_ctx->identity_userdata,
                                               &cert_path);
        if (status)
            return status;

        status = serf__macosxssl_load_identity_from_file(ssl_ctx, &identity,
                                                         cert_path, pool);
        if (status)
            return status;
    } else if (ssl_ctx->identity_callback)
    {
        status = ssl_ctx->identity_callback(ssl_ctx->identity_userdata,
                                            dnlist, dnlen, &identity, pool);
    } else
        status = APR_EGENERAL;

    if (status == APR_SUCCESS)
        *identityref = (SecIdentityRef)identity->impl_cert;

    return status;
}

/* Get a client certificate for this server from the application. */
static apr_status_t
provide_client_certificate(macosxssl_context_t *ssl_ctx)
{
    SecIdentityRef identityref = NULL;
    CFArrayRef dnlistrefs = NULL;
    apr_hash_t **dnlist = NULL;
    apr_size_t dnlen = 0;
    apr_pool_t *tmppool;
    apr_status_t status;
    OSStatus osstatus;

    serf__log(SSL_VERBOSE, __FILE__, "provide_client_certificate called.\n");

    /* The server asked for a client certificate but we can't ask the
       application. Consider this a success, the server decides if the
       request was optional or mandatory. */
    if (!ssl_ctx->client_cert_callback &&
        !ssl_ctx->identity_callback)
    {
        serf__log(SSL_VERBOSE, __FILE__, "Server asked for client "
                  "certificate, but the application didn't set the "
                  "necessary callback.\n");
        return APR_SUCCESS;
    }

    /* The server will send us a list of Distinguished Names, indicating that
       client certificates issued by one of these DN's are acceptable. */
    osstatus = SSLCopyDistinguishedNames(ssl_ctx->st_ctxr, &dnlistrefs);
    if (osstatus != noErr) {
        return translate_macosxssl_status(osstatus);
    }

    apr_pool_create(&tmppool, ssl_ctx->pool);
    if (dnlistrefs && CFArrayGetCount(dnlistrefs) > 0)
    {
        int i;

        dnlen = CFArrayGetCount(dnlistrefs);
        dnlist = apr_palloc(tmppool, dnlen * sizeof(apr_hash_t*));

        for (i = 0; i < dnlen; i++) {
            CFDataRef cader = CFArrayGetValueAtIndex(dnlistrefs, i);
            apr_hash_t *ca;

            status = serf__macosxssl_read_X509_DER_DN(&ca, cader, tmppool);
            if (status)
                goto cleanup;

            dnlist[i] = ca;
        }
    }

    /* Call the application to find an identity trusted by one of the DN's
       provided by the server. */
    status = callback_for_identity(ssl_ctx, dnlist, dnlen, &identityref,
                                   ssl_ctx->pool);
    if (status)
        goto cleanup;

    /* If the issuer of the client certificate is not in the list
       of certificates the server provided, we need to send it along.
       Otherwise the server can complain that it doesn't trust the
       client identity.
       Note: this is what happens with the Belgian Personal ID Card on
       site https://https://test.eid.belgium.be, where the "Citizen CA"
       certificate is the issuer of the client certificate, but is not
       sent by the server. */
    if (identityref)
    {
        CFArrayRef intermediate_certrefs, peer_certrefs;
        SecCertificateRef cert;
        CFMutableArrayRef items;

        /* Secure Transport assumes the following:
           The certificate references remain valid for the lifetime of the
           session.
           The identity specified in certRefs[0] is capable of signing. */
        items = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
        CFArrayAppendValue(items, identityref);
        apr_pool_cleanup_register(ssl_ctx->pool, items,
                                  cfrelease_ref, cfrelease_ref);

        osstatus = SecIdentityCopyCertificate(identityref, &cert);
        if (osstatus != noErr) {
            status = translate_macosxssl_status(osstatus);
            goto cleanup;
        }

        osstatus = SSLCopyPeerCertificates(ssl_ctx->st_ctxr,
                                           &peer_certrefs);
        if (osstatus != noErr) {
            status = translate_macosxssl_status(osstatus);
            goto cleanup;
        }

        status = find_intermediate_cas(&intermediate_certrefs,
                                       cert,
                                       peer_certrefs,
                                       ssl_ctx->pool);
        CFRelease(peer_certrefs);
        if (status)
            goto cleanup;

        /* Add the intermediate certificates to the list to send to the
           server. */
        CFArrayAppendArray(items, intermediate_certrefs,
                           CFRangeMake(0,
                                       CFArrayGetCount(intermediate_certrefs)));

        /* This can show a popup to ask the user if the application is
           allowed to use the signing key. */
        osstatus = SSLSetCertificate(ssl_ctx->st_ctxr, items);
        if (osstatus != noErr) {
            status = translate_macosxssl_status(osstatus);
            goto cleanup;
        }

        status = APR_SUCCESS;
    }

cleanup:
    apr_pool_destroy(tmppool);
    if (dnlistrefs)
        CFRelease(dnlistrefs);

    return status;
}

/* Use Keychain Services to extract a SecIndentityRef (client private key +
   certificate) from a data buffer. Databuf needs to be in PKCS12 format.
   Caller is responsible to clean up items.
 */
static apr_status_t
load_identity_from_databuf(macosxssl_context_t *ssl_ctx,
                           const serf_ssl_identity_t **identity,
                           const char *label,
                           CFDataRef databuf,
                           apr_pool_t *pool)
{
    SecExternalFormat format;
    SecItemImportExportKeyParameters keyParams;
    SecExternalItemType itemType;

    /* SecItemImport will crash if keyUsage member is not set to NULL. */
    memset(&keyParams, 0, sizeof(SecItemImportExportKeyParameters));

    format = kSecFormatPKCS12;
    itemType = kSecItemTypeUnknown;

    /* Try importing the identity until it succeeds, fails or the application
       stops setting passphrases. */
    while (1)
    {
        CFArrayRef items;
        OSStatus osstatus;
        apr_status_t status;
        const char *passphrase;

        osstatus = SecItemImport(databuf,
                                 NULL,
                                 &format,
                                 &itemType,
                                 0,    /* SecItemImportExportFlags */
                                 &keyParams,
                                 ssl_ctx->tempKeyChainRef,
                                 &items);
        if (osstatus == errSecSuccess)
        {
            *identity = NULL;

            /* Identity successfully imported in the keychain, return the
               (wrapped) reference to the caller. */
            if (CFArrayGetCount(items) > 0)
            {
                SecIdentityRef identityref;

                identityref = (SecIdentityRef)CFArrayGetValueAtIndex(items, 0);

                if (!identityref)
                    return SERF_ERROR_SSL_CERT_FAILED;

                *identity = serf__create_identity(&serf_ssl_bucket_type_macosxssl,
                                                  identityref, NULL, pool);

                return APR_SUCCESS;
            }

            return SERF_ERROR_SSL_CERT_FAILED;
        } else if (osstatus == errSecPassphraseRequired)
        {
            CFStringRef pwref;

            status = callback_for_identity_password(
                                                    ssl_ctx,
                                                    label,
                                                    &passphrase);
            if (status)
                return status;

            pwref = CFStringCreateWithBytesNoCopy(kCFAllocatorDefault,
                                                  (unsigned char *)passphrase,
                                                  strlen(passphrase),
                                                  kCFStringEncodingMacRoman,
                                                  false,
                                                  kCFAllocatorNull);
            keyParams.passphrase = pwref;
        } else {
            /* TODO: should be handled in translate_... */
            return SERF_ERROR_SSL_CERT_FAILED;
        }
    }
}

/**** Run the SSL handshake. ****/
/********************************/
#pragma mark HANDSHAKE AND INITIALIZATION
#pragma mark -
static apr_status_t do_handshake(macosxssl_context_t *ssl_ctx)
{
    OSStatus osstatus;
    apr_status_t status = APR_SUCCESS;

    if (ssl_ctx->state == SERF_MACOSXSSL_INIT ||
        ssl_ctx->state == SERF_MACOSXSSL_HANDSHAKE)
    {
        ssl_ctx->state = SERF_MACOSXSSL_HANDSHAKE;

        serf__log(SSL_VERBOSE, __FILE__, "do_handshake called.\n");

        if (ssl_ctx->evaluate_in_progress) {
            if (ssl_ctx->result) {
                status = validate_server_certificate(ssl_ctx);
                ssl_ctx->evaluate_in_progress = 0;
                if (!status)
                    return APR_EAGAIN;
                return status;
            } else {
                serf__log(SSL_VERBOSE, __FILE__, "evaluation in progress, but "
                          "no results were received yet.\n");
                return APR_EAGAIN;
            }
        }

        osstatus = SSLHandshake(ssl_ctx->st_ctxr);
        if (osstatus)
            serf__log(SSL_VERBOSE, __FILE__, "do_handshake returned err %d.\n",
                      osstatus);

        switch(osstatus) {
            case noErr:
                status = APR_SUCCESS;
                break;
            case errSSLServerAuthCompleted:
                /* Server's cert validation was disabled, so we can to do this
                 here. */
                status = validate_server_certificate(ssl_ctx);
                if (!status)
                    return APR_EAGAIN;
                break;
            case errSSLClientCertRequested:
                status = provide_client_certificate(ssl_ctx);
                if (!status)
                    return APR_EAGAIN;
                break;
            default:
                status = translate_macosxssl_status(osstatus);
                break;
        }

        if (!status)
        {
            serf__log(SSL_VERBOSE, __FILE__, "ssl/tls handshake successful.\n");
            ssl_ctx->state = SERF_MACOSXSSL_CONNECTED;

            /* We can now safely cleanup the temporary resources created during
               handshake (i.e. the temporary keychain used to load the client
               identity. */
            apr_pool_destroy(ssl_ctx->handshake_pool);
            ssl_ctx->handshake_pool = NULL;

            ssl_ctx->result = 0;
            ssl_ctx->evaluate_in_progress = 0;
        }
    }

    return status;
}

/* Callback function for the encrypt.pending and decrypt.pending stream-type
   aggregate buckets.
 */
static apr_status_t
pending_stream_eof(void *baton,
                   serf_bucket_t *pending)
{
    /* Both pending streams have to stay open so that the Secure Transport
       library can keep appending data buckets. */
    return APR_EAGAIN;
}

static macosxssl_context_t *
macosxssl_init_context(serf_bucket_alloc_t *allocator)
{
    macosxssl_context_t *ssl_ctx;

    ssl_ctx = serf_bucket_mem_calloc(allocator, sizeof(*ssl_ctx));
    ssl_ctx->refcount = 0;

    apr_pool_create(&ssl_ctx->pool, NULL);
    apr_pool_create(&ssl_ctx->handshake_pool, ssl_ctx->pool);

    /* Set up the stream objects. */
    ssl_ctx->encrypt.pending = serf__bucket_stream_create(allocator,
                                                          pending_stream_eof,
                                                          NULL);
    ssl_ctx->decrypt.pending = serf__bucket_stream_create(allocator,
                                                          pending_stream_eof,
                                                          NULL);

    /* Set up a Secure Transport session. */
    ssl_ctx->state = SERF_MACOSXSSL_INIT;

    if (SSLNewContext(FALSE, &ssl_ctx->st_ctxr))
        return NULL;

    if (SSLSetIOFuncs(ssl_ctx->st_ctxr, macosxssl_read_cb, macosxssl_write_cb))
        return NULL;

    /* Ensure the macosxssl_context will be passed to the read and write callback
     functions. */
    if (SSLSetConnection(ssl_ctx->st_ctxr, ssl_ctx))
        return NULL;

    /* We do our own validation of server certificates.
       Note that Secure Transport will not do any validation with this option
       enabled, it's all or nothing. */
    if (SSLSetSessionOption(ssl_ctx->st_ctxr,
                            kSSLSessionOptionBreakOnServerAuth,
                            true))
        return NULL;
    if (SSLSetEnableCertVerify(ssl_ctx->st_ctxr, false))
        return NULL;

    /* If the handshake needs a client identity to continue, break it
       temporarily so we can call back to the application. */
    if (SSLSetSessionOption(ssl_ctx->st_ctxr,
                            kSSLSessionOptionBreakOnCertRequested,
                            true))
        return NULL;

    return ssl_ctx;
}

static apr_status_t
macosxssl_free_context(macosxssl_context_t *ssl_ctx,
                       serf_bucket_alloc_t *allocator)
{
    apr_status_t status = APR_SUCCESS;

    (void)SSLDisposeContext(ssl_ctx->st_ctxr);

    if (ssl_ctx->handshake_pool)
        apr_pool_destroy(ssl_ctx->handshake_pool);
    apr_pool_destroy(ssl_ctx->pool);

    serf_bucket_mem_free(allocator, ssl_ctx);

    if (status) {
        return APR_EGENERAL;
    }
    
    return APR_SUCCESS;
}

/**** SSL_BUCKET API ****/
/************************/
#pragma mark SSL_BUCKET API
#pragma mark -
static void *
serf__macosxssl_decrypt_create(serf_bucket_t *bucket,
                               serf_bucket_t *stream,
                               void *impl_ctx,
                               serf_bucket_alloc_t *allocator)
{
    macosxssl_context_t *ssl_ctx;
    bucket->type = &serf_bucket_type_macosxssl_decrypt;
    bucket->allocator = allocator;

    if (impl_ctx)
        bucket->data = impl_ctx;
    else
        bucket->data = macosxssl_init_context(allocator);

    ssl_ctx = bucket->data;
    ssl_ctx->refcount++;
    ssl_ctx->decrypt.stream = stream;
    ssl_ctx->allocator = allocator;

    return bucket->data;
}

static void *
serf__macosxssl_encrypt_create(serf_bucket_t *bucket,
                               serf_bucket_t *stream,
                               void *impl_ctx,
                               serf_bucket_alloc_t *allocator)
{
    macosxssl_context_t *ssl_ctx;
    bucket->type = &serf_bucket_type_macosxssl_encrypt;
    bucket->allocator = allocator;

    if (impl_ctx)
        bucket->data = impl_ctx;
    else
        bucket->data = macosxssl_init_context(allocator);

    ssl_ctx = bucket->data;
    ssl_ctx->refcount++;
    ssl_ctx->encrypt.stream = stream;
    ssl_ctx->allocator = allocator;

    return bucket->data;
}

static void *
serf__macosxssl_decrypt_context_get(serf_bucket_t *bucket)
{
    return NULL;
}

static void *
serf__macosxssl_encrypt_context_get(serf_bucket_t *bucket)
{
    return NULL;
}


static void
serf__macosxssl_client_cert_provider_set(
        void *impl_ctx,
        serf_ssl_need_client_cert_t callback,
        void *data,
        void *cache_pool)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->client_cert_callback = callback;
    ssl_ctx->identity_userdata = data;
}

static void
serf__macosxssl_identity_provider_set(void *impl_ctx,
                                      serf_ssl_need_identity_t callback,
                                      void *data,
                                      void *cache_pool)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->identity_callback = callback;
    ssl_ctx->identity_userdata = data;
}

static void
serf__macosxssl_client_cert_password_set(
        void *impl_ctx,
        serf_ssl_need_cert_password_t callback,
        void *data,
        void *cache_pool)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->identity_pw_callback = callback;
    ssl_ctx->identity_pw_userdata = data;
}

static void
serf__macosxssl_server_cert_callback_set(
        void *impl_ctx,
        serf_ssl_need_server_cert_t callback,
        void *data)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->server_cert_callback = callback;
    ssl_ctx->server_cert_userdata = data;
}

static void
serf__macosxssl_server_cert_chain_callback_set(
        void *impl_ctx,
        serf_ssl_need_server_cert_t cert_callback,
        serf_ssl_server_cert_chain_cb_t cert_chain_callback,
        void *data)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->server_cert_callback = cert_callback;
    ssl_ctx->server_cert_chain_callback = cert_chain_callback;
    ssl_ctx->server_cert_userdata = data;
}

static apr_status_t
serf__macosxssl_set_hostname(void *impl_ctx, const char * hostname)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;
    OSStatus osstatus;

    ssl_ctx->hostname = serf_bstrdup(ssl_ctx->allocator, hostname);
    osstatus= SSLSetPeerDomainName(ssl_ctx->st_ctxr,
                                   ssl_ctx->hostname,
                                   strlen(hostname));

    return translate_macosxssl_status(osstatus);
}

static apr_status_t
serf__macosxssl_use_default_certificates(void *impl_ctx)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;

    /* When constructing the trust object to validate the server certificate
       chain, extract all root certificates from the System keychain first
       to include them during validation. */
    ssl_ctx->use_system_roots = 1;

    return APR_SUCCESS;
}

apr_status_t
load_CA_cert_from_buffer(serf_ssl_certificate_t **cert,
                         const char *buf,
                         apr_size_t len,
                         apr_pool_t *pool)
{
    CFArrayRef items;
    CFDataRef databuf;
    apr_status_t status;

    databuf = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                          (unsigned char *)buf,
                                          len,
                                          kCFAllocatorNull);

    status = load_certificate_from_databuf(databuf, &items, pool);
    if (status)
        return status;

    if (CFArrayGetCount(items) > 0) {
        SecCertificateRef ssl_cert =
            (SecCertificateRef)CFArrayGetValueAtIndex(items, 0);

        if (ssl_cert) {
            *cert = create_ssl_certificate(ssl_cert,
                                           0,
                                           pool);
            return APR_SUCCESS;
        }
    }

    /* TODO: cleanup databuf needed? */

    return SERF_ERROR_SSL_CERT_FAILED;
}

static apr_status_t
serf__macosxssl_load_CA_cert_from_file(serf_ssl_certificate_t **cert,
                                       const char *file_path,
                                       apr_pool_t *pool)
{
    CFArrayRef items;
    CFDataRef databuf;
    apr_status_t status;

    status = load_data_from_file(file_path, &databuf, pool);
    if (status)
        return status;

    status = load_certificate_from_databuf(databuf, &items, pool);
    if (status)
        return status;

    if (CFArrayGetCount(items) > 0) {
        SecCertificateRef ssl_cert =
            (SecCertificateRef)CFArrayGetValueAtIndex(items, 0);

        if (ssl_cert) {
            *cert = create_ssl_certificate(ssl_cert,
                                           0,
                                           pool);
            return APR_SUCCESS;
        }
    }

    return SERF_ERROR_SSL_CERT_FAILED;
}

static apr_status_t
serf__macosxssl_load_identity_from_file(void *impl_ctx,
                                        const serf_ssl_identity_t **identity,
                                        const char *file_path,
                                        apr_pool_t *pool)
{
    apr_status_t status;
    CFDataRef databuf;
    macosxssl_context_t *ssl_ctx = impl_ctx;

    status = load_data_from_file(file_path, &databuf, pool);
    if (status)
        return status;

    status = create_temp_keychain(ssl_ctx, ssl_ctx->handshake_pool);
    if (status)
        return status;

    status = load_identity_from_databuf(ssl_ctx, identity, file_path,
                                        databuf, pool);

    return status;
}

static apr_status_t
serf__macosxssl_trust_cert(void *impl_ctx,
                           serf_ssl_certificate_t *cert)
{
    macosxssl_context_t *ssl_ctx = impl_ctx;
    macosxssl_certificate_t *macosxssl_cert = cert->impl_cert;

    if (!ssl_ctx->anchor_certs)
        ssl_ctx->anchor_certs = apr_array_make(ssl_ctx->pool, 1,
                                               sizeof(SecCertificateRef));
    APR_ARRAY_PUSH(ssl_ctx->anchor_certs,
                   SecCertificateRef) = macosxssl_cert->certref;

    return APR_SUCCESS;
}

static apr_hash_t *
serf__macosxssl_cert_certificate(const serf_ssl_certificate_t *cert,
                                 apr_pool_t *pool)
{
    apr_hash_t *tgt;
    const char *date_str, *sha1;

    macosxssl_certificate_t *macosxssl_cert = cert->impl_cert;

    if (!macosxssl_cert->content) {
        apr_status_t status;
        status = serf__macosxssl_read_X509_DER_certificate(
                           &macosxssl_cert->content,
                           macosxssl_cert,
                           pool);
        if (status)
            return NULL;
    }

    tgt = apr_hash_make(pool);

    date_str = apr_hash_get(macosxssl_cert->content, "notBefore",
                            APR_HASH_KEY_STRING);
    apr_hash_set(tgt, "notBefore", APR_HASH_KEY_STRING, date_str);

    date_str = apr_hash_get(macosxssl_cert->content, "notAfter",
                            APR_HASH_KEY_STRING);
    apr_hash_set(tgt, "notAfter", APR_HASH_KEY_STRING, date_str);

    sha1 = apr_hash_get(macosxssl_cert->content, "sha1", APR_HASH_KEY_STRING);
    apr_hash_set(tgt, "sha1", APR_HASH_KEY_STRING, sha1);

    /* TODO: array of subjectAltName's */

    return tgt;
}


static apr_hash_t *
serf__macosxssl_cert_issuer(const serf_ssl_certificate_t *cert,
                            apr_pool_t *pool)
{
    macosxssl_certificate_t *macosxssl_cert = cert->impl_cert;

    if (!macosxssl_cert->content) {
        apr_status_t status;
        status = serf__macosxssl_read_X509_DER_certificate(
                           &macosxssl_cert->content,
                           macosxssl_cert,
                           pool);
        if (status)
            return NULL;
    }

    return (apr_hash_t *)apr_hash_get(macosxssl_cert->content,
                                      "issuer", APR_HASH_KEY_STRING);
}

static apr_hash_t *
serf__macosxssl_cert_subject(const serf_ssl_certificate_t *cert,
                             apr_pool_t *pool)
{
    macosxssl_certificate_t *macosxssl_cert = cert->impl_cert;

    if (!macosxssl_cert->content) {
        apr_status_t status;
        status = serf__macosxssl_read_X509_DER_certificate(
                           &macosxssl_cert->content,
                           macosxssl_cert,
                           pool);
        if (status)
            return NULL;
    }

    return (apr_hash_t *)apr_hash_get(macosxssl_cert->content,
                                      "subject", APR_HASH_KEY_STRING);
}

static const char *
serf__macosxssl_cert_export(const serf_ssl_certificate_t *cert,
                            apr_pool_t *pool)
{
    macosxssl_certificate_t *macosxssl_cert = cert->impl_cert;
    SecCertificateRef certref = macosxssl_cert->certref;
    CFDataRef dataref = SecCertificateCopyData(certref);
    const unsigned char *data = CFDataGetBytePtr(dataref);
    char *encoded_cert;

    CFIndex len = CFDataGetLength(dataref);

    if (!len)
        return NULL;

    encoded_cert = apr_palloc(pool, apr_base64_encode_len(len));

    apr_base64_encode(encoded_cert, (char*)data, len);

    return encoded_cert;
}

static apr_status_t
serf__macosxssl_use_compression(void *impl_ctx, int enabled)
{
    if (enabled) {
        serf__log(SSL_VERBOSE, __FILE__,
                  "Secure Transport does not support any type of "
                  "SSL compression.\n");
        return APR_ENOTIMPL;
    } else {
        return APR_SUCCESS;
    }
}

/**** ENCRYPTION BUCKET API *****/
/********************************/
#pragma mark ENCRYPTION BUCKET API
#pragma mark -
static apr_status_t
serf_macosxssl_encrypt_read(serf_bucket_t *bucket,
                            apr_size_t requested,
                            const char **data, apr_size_t *len)
{
    macosxssl_context_t *ssl_ctx = bucket->data;
    apr_status_t status, status_unenc_stream;
    const char *unenc_data;
    struct iovec vecs[32];
    int vecs_used;
    size_t unenc_len;

    serf__log(SSL_VERBOSE, __FILE__, "serf_macosxssl_encrypt_read called for "
              "%d bytes.\n", requested);

    /* Pending handshake? */
    status = do_handshake(ssl_ctx);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;
    if (status) {
        /* Maybe the handshake algorithm put some data in the pending
         outgoing bucket? */
        return serf_bucket_read(ssl_ctx->encrypt.pending, requested, data, len);
    }

    /* Handshake successful. */

    /* First use any pending encrypted data. */
    status = serf_bucket_read(ssl_ctx->encrypt.pending, requested, data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    if (*len) {
        /* status can be either APR_EAGAIN or APR_SUCCESS. In both cases,
           we want the caller to try again as there's probably more data
           to be encrypted. */
        return APR_SUCCESS;
    }

    /* SSLWrite does not write directly to a socket, but to an aggregate bucket
       which can be infinitely big. To ensure that the outgoing data is 
       written to the socket regularely, we only read one block at a time and
       then return to the caller. */
    if (requested == SERF_READ_ALL_AVAIL)
        requested = 65536;

    /* Encrypt more data. */
    status_unenc_stream = serf_bucket_read_iovec(ssl_ctx->encrypt.stream,
                                                 requested,
                                                 32, vecs,
                                                 &vecs_used);
    if (SERF_BUCKET_READ_ERROR(status_unenc_stream))
        return status_unenc_stream;

    unenc_data = serf_bstrcatv(ssl_ctx->encrypt.stream->allocator,
                               vecs, vecs_used, &unenc_len);

    serf__log(SSL_VERBOSE, __FILE__, "ready to encrypt %d bytes with status %d.\n",
              unenc_len, status_unenc_stream);

    if (unenc_len)
    {
        OSStatus osstatus;
        size_t written;

        osstatus = SSLWrite(ssl_ctx->st_ctxr, unenc_data, unenc_len,
                            &written);
        status = translate_macosxssl_status(osstatus);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        serf__log(SSL_MSG_VERBOSE, __FILE__, "%dB ready with status %d, %d "
                  "encrypted and written:\n---%.*s-(%d)-\n", unenc_len,
                  status_unenc_stream, written, written, unenc_data, written);

        /* Less data written than available! This situation can never happen,
           because SSLWrite loops until all data is sent or macosxssl_write_cb
           returns errSSLWouldBlock, and the callback will never return this
           error because it can process all data that it's given. */
        if (written < unenc_len)
        {
            serf__log(SSL_VERBOSE, __FILE__, "Less data written than was "
                      "available! Aborting, this should not be possible.\n");
            return APR_EGENERAL;
        }

        status = serf_bucket_read(ssl_ctx->encrypt.pending, requested,
                                  data, len);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        /* Tell the caller there's more data readily available. */
        if (status == APR_SUCCESS)
            return status;
    }

    /* All encrypted data was returned, if there's more available depends
       on what's pending on the to-be-encrypted stream. */
    return status_unenc_stream;
}

static apr_status_t
serf_macosxssl_encrypt_readline(serf_bucket_t *bucket,
                                int acceptable, int *found,
                                const char **data,
                                apr_size_t *len)
{
    serf__log(SSL_VERBOSE, __FILE__,
              "function serf_macosxssl_encrypt_readline not implemented.\n");
    return APR_ENOTIMPL;
}


static apr_status_t
serf_macosxssl_encrypt_peek(serf_bucket_t *bucket,
                            const char **data,
                            apr_size_t *len)
{
    macosxssl_context_t *ssl_ctx = bucket->data;

    return serf_bucket_peek(ssl_ctx->encrypt.pending, data, len);
}

static void
serf_macosxssl_encrypt_destroy_and_data(serf_bucket_t *bucket)
{
    macosxssl_context_t *ssl_ctx = bucket->data;

    if (!--ssl_ctx->refcount) {
        macosxssl_free_context(ssl_ctx, bucket->allocator);
    }

    serf_bucket_ssl_destroy_and_data(bucket);
}

/**** DECRYPTION BUCKET API *****/
/********************************/
#pragma mark DECRYPTION BUCKET API
#pragma mark -
static apr_status_t
serf_macosxssl_decrypt_peek(serf_bucket_t *bucket,
                            const char **data,
                            apr_size_t *len)
{
    macosxssl_context_t *ssl_ctx = bucket->data;
    
    return serf_bucket_peek(ssl_ctx->decrypt.pending, data, len);
}

/* Ask Secure Transport to decrypt some more data. If anything was received,
   add it to the to decrypt.pending buffer.
   This function will read and decrypt all available data.
 */
static apr_status_t
decrypt_more_data(macosxssl_context_t *ssl_ctx)
{
    /* Decrypt more data. */
    serf_bucket_t *tmp;
    char *dec_data;
    size_t dec_len, available_len;
    OSStatus osstatus;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__,
              "decrypt_more_data called.\n");

    /* Read until the stream has no more data and until SSLRead decrypted
       all data - which can take multiple calls! */
    do {
        /* We have to provide ST with a sufficiently large buffer for the
           decrypted data. We can ask S.T. how many bytes are still buffered,
           but this gives no indication on how many are still waiting to
           be decrypted. Use it only to increase the bufsize over the
           default (minimum) size READ_BUFSIZE. */
        osstatus = SSLGetBufferedReadSize(ssl_ctx->st_ctxr, &available_len);
        if (osstatus != noErr)
            return translate_macosxssl_status(osstatus);
        else {
            if (available_len < READ_BUFSIZE)
                available_len = READ_BUFSIZE;
        }
        /* TODO: SSLRead doesn't always return a lot of data in one go, so
           it makes sense to continue receiving data in the remainder of this
           buffer before allocating a new buffer. */
        dec_data = serf_bucket_mem_alloc(ssl_ctx->decrypt.pending->allocator,
                                         available_len);

        osstatus = SSLRead(ssl_ctx->st_ctxr, dec_data,
                           available_len,
                           &dec_len);
        status = translate_macosxssl_status(osstatus);

        /* SSLRead can put data in dec_data while returning an error status. */
        if (SERF_BUCKET_READ_ERROR(status) && !dec_len)
            return status;

        /* Successfully received and decrypted some data, add to pending. */
        serf__log(SSL_MSG_VERBOSE, __FILE__, " received and decrypted data:"
                  "---\n%.*s\n-(%d)-\n", dec_len, dec_data, dec_len);

        tmp = SERF_BUCKET_SIMPLE_STRING_LEN(dec_data, dec_len,
                                            ssl_ctx->decrypt.pending->allocator);
        serf_bucket_aggregate_append(ssl_ctx->decrypt.pending, tmp);

    } while (status == APR_SUCCESS);

    return status;
}

static apr_status_t
serf_macosxssl_decrypt_read(serf_bucket_t *bucket,
                            apr_size_t requested,
                            const char **data, apr_size_t *len)
{
    macosxssl_context_t *ssl_ctx = bucket->data;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__,
              "serf_macosxssl_decrypt_read called for %d bytes.\n", requested);

    /* Pending handshake? */
    status = do_handshake(ssl_ctx);
    if (status) {
        *len = 0;
        return status;
    }

    /* First use any pending encrypted data. */
    status = serf_bucket_read(ssl_ctx->decrypt.pending,
                              requested, data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    if (*len)
        return status;

    /* Pending buffer empty, decrypt more. */
    status = decrypt_more_data(ssl_ctx);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    /* We should now have more decrypted data in the pending buffer. */
    return serf_bucket_read(ssl_ctx->decrypt.pending, requested, data,
                            len);
}

static apr_status_t
serf_macosxssl_decrypt_readline(serf_bucket_t *bucket,
                                int acceptable, int *found,
                                const char **data,
                                apr_size_t *len)
{
    macosxssl_context_t *ssl_ctx = bucket->data;
    apr_status_t status;

    /* Pending handshake? */
    status = do_handshake(ssl_ctx);
    if (status) {
        *len = 0;
        *found = SERF_NEWLINE_NONE;
        return status;
    }

    /* First use any pending encrypted data. */
    status = serf_bucket_readline(ssl_ctx->decrypt.pending, acceptable, found,
                                  data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    if (*len) {
        /* We read a partial or complete line */
        return status;
    }

    /* Pending buffer empty, decrypt more. */
    status = decrypt_more_data(ssl_ctx);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    /* We have more decrypted data in the pending buffer. */
    status = serf_bucket_readline(ssl_ctx->decrypt.pending, acceptable, found,
                                  data, len);

    return status;
}

static void
serf_macosxssl_decrypt_destroy_and_data(serf_bucket_t *bucket)
{
    macosxssl_context_t *ssl_ctx = bucket->data;

    if (!--ssl_ctx->refcount) {
        macosxssl_free_context(ssl_ctx, bucket->allocator);
    }

    serf_bucket_ssl_destroy_and_data(bucket);
}

/**** Helper function to fetch info from Keychain ****/
/*****************************************************/
#pragma mark KEYCHAIN HELPER FUNCTIONS
#pragma mark -
apr_status_t
serf_macosxssl_show_trust_certificate_panel(serf_ssl_context_t *ctx,
                                            const char *message,
                                            const char *ok_button,
                                            const char *cancel_button)
{
#ifdef SERF_HAVE_MACOSXSSL
    macosxssl_context_t *ssl_ctx = serf__ssl_get_impl_context(ctx);
    SecTrustRef trust = ssl_ctx->trust;
    apr_status_t status;

    void *macosxssl_cls = objc_getClass("macosxssl_Buckets");
    id tmp = objc_msgSend(macosxssl_cls,
                          sel_getUid("showTrustCertificateDialog:message:"
                                     "ok_button:cancel_button:"),
                          trust, message, ok_button, cancel_button);
    status = (apr_status_t)(SInt64)tmp;

    return status;
#else
    return APR_ENOTIMPL;
#endif
}

apr_status_t
serf_macosxssl_show_select_identity_panel(serf_ssl_context_t *ctx,
                                          const serf_ssl_identity_t **identity,
                                          const char *message,
                                          const char *ok_button,
                                          const char *cancel_button,
                                          apr_pool_t *pool)
{
#ifdef SERF_HAVE_MACOSXSSL
    SecIdentityRef identityref;
    OSStatus osstatus;

    void *macosxssl_cls = objc_getClass("MacOSXSSL_Buckets");
    id tmp = objc_msgSend(macosxssl_cls,
                          sel_getUid("showSelectIdentityDialog:message:"
                                     "ok_button:cancel_button:"),
                          &identityref, message, ok_button, cancel_button);
    osstatus = (OSStatus)(SInt64)tmp;
    if (osstatus == errSecItemNotFound)
    {
        /* There is no single identity in the keychains. */
        return SERF_ERROR_SSL_NO_IDENTITIES_AVAILABLE;
    } else if (osstatus != noErr) {
        return translate_macosxssl_status(osstatus);
    }

    if (!identityref)
        return SERF_ERROR_SSL_CERT_FAILED;

    *identity = serf__create_identity(&serf_ssl_bucket_type_macosxssl,
                                      identityref, NULL,
                                      pool);
    return APR_SUCCESS;
#else
    return APR_ENOTIMPL;
#endif
}

apr_status_t
serf_macosxssl_find_preferred_identity_in_keychain(
    serf_ssl_context_t *ctx,
    const serf_ssl_identity_t **identity,
    apr_pool_t *pool)
{
#ifdef SERF_HAVE_MACOSXSSL
    apr_pool_t *tmppool;
    macosxssl_context_t *ssl_ctx = serf__ssl_get_impl_context(ctx);
    SecIdentityRef identityref = NULL;
    CFStringRef labelref;
    const char *label;
    apr_status_t status;

    /* We can get the distinguished names from the server with
       SSLCopyDistinguishedNames to filter matching client certificates,
       but we can't pass this list to the application, and
       SecIdentityCopyPreferred doesn't have this feature implemented
       either. So, don't bother. */

    /* We absolutelely need an item that can sign. Otherwise we will get
       an incomplete identity object, with which SecIdentityCopyCertificate
       will crash. */
    const void *keyUsage[] = { kSecAttrCanSign };
    CFArrayRef keyUsageRef = CFArrayCreate(kCFAllocatorDefault,
                                           (void *)keyUsage,
                                           1,
                                           NULL);

    apr_pool_create(&tmppool, pool);

    /* Find an identity preference using label https://<hostname> */
    label = apr_pstrcat(tmppool, "https://", ssl_ctx->hostname, NULL);
    labelref = CFStringCreateWithBytesNoCopy(kCFAllocatorDefault,
                                             (unsigned char *)label,
                                             strlen(label),
                                             kCFStringEncodingMacRoman,
                                             false,
                                             kCFAllocatorNull);
    identityref = SecIdentityCopyPreferred(labelref,
                                           keyUsageRef, NULL);
    if (identityref)
    {

        *identity = serf__create_identity(&serf_ssl_bucket_type_macosxssl,
                                          identityref, NULL, pool);
        status = APR_SUCCESS;
    } else {
        *identity = NULL;
        status = SERF_ERROR_SSL_CERT_FAILED;
    }

    CFRelease(labelref);
    CFRelease(keyUsageRef);
    apr_pool_destroy(tmppool);
    
    return status;
#else
    return APR_ENOTIMPL;
#endif
}

/*****************************************************************************/
const serf_bucket_type_t serf_bucket_type_macosxssl_encrypt = {
    "MACOSXSSLENCRYPT",
    serf_macosxssl_encrypt_read,
    serf_macosxssl_encrypt_readline,
    serf_default_read_iovec,
    serf_default_read_for_sendfile,
    serf_default_read_bucket,
    serf_macosxssl_encrypt_peek,
    serf_macosxssl_encrypt_destroy_and_data,
};

const serf_bucket_type_t serf_bucket_type_macosxssl_decrypt = {
    "MACOSXSSLDECRYPT",
    serf_macosxssl_decrypt_read,
    serf_macosxssl_decrypt_readline,
    serf_default_read_iovec,
    serf_default_read_for_sendfile,
    serf_default_read_bucket,
    serf_macosxssl_decrypt_peek,
    serf_macosxssl_decrypt_destroy_and_data,
};

const serf_ssl_bucket_type_t serf_ssl_bucket_type_macosxssl = {
    serf__macosxssl_decrypt_create,
    serf__macosxssl_decrypt_context_get,
    serf__macosxssl_encrypt_create,
    serf__macosxssl_encrypt_context_get,
    serf__macosxssl_set_hostname,
    serf__macosxssl_client_cert_provider_set,
    serf__macosxssl_identity_provider_set,
    serf__macosxssl_client_cert_password_set,
    serf__macosxssl_server_cert_callback_set,
    serf__macosxssl_server_cert_chain_callback_set,
    serf__macosxssl_use_default_certificates,
    serf__macosxssl_load_CA_cert_from_file,
    serf__macosxssl_load_identity_from_file,
    serf__macosxssl_trust_cert,
    serf__macosxssl_cert_issuer,
    serf__macosxssl_cert_subject,
    serf__macosxssl_cert_certificate,
    serf__macosxssl_cert_export,
    serf__macosxssl_use_compression,
    NULL,
    NULL,
    NULL,
    NULL,
};

#else

const serf_bucket_type_t serf_bucket_type_macosxssl_encrypt = {
    "MACOSXSSLENCRYPT",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};
const serf_bucket_type_t serf_bucket_type_macosxssl_decrypt = {
    "MACOSXSSLDECRYPT",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

#endif /* SERF_HAVE_MACOSXSSL */
