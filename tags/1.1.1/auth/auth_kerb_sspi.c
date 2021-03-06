/* Copyright 2010 Justin Erenkrantz and Greg Stein
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

#include "auth_kerb.h"

#ifdef SERF_USE_SSPI
#include <apr.h>
#include <apr_strings.h>

#define SECURITY_WIN32
#include <sspi.h>

struct serf__kerb_context_t
{
    CredHandle sspi_credentials;
    CtxtHandle sspi_context;
    BOOL initalized;
};

/* Cleans the SSPI context object, when the pool used to create it gets
   cleared or destroyed. */
static apr_status_t
cleanup_ctx(void *data)
{
    serf__kerb_context_t *ctx = data;

    if (SecIsValidHandle(&ctx->sspi_context)) {
        DeleteSecurityContext(&ctx->sspi_context);
        SecInvalidateHandle(&ctx->sspi_context);
    }

    if (SecIsValidHandle(&ctx->sspi_credentials)) {
        FreeCredentialsHandle(&ctx->sspi_context);
        SecInvalidateHandle(&ctx->sspi_context);
    }

    return APR_SUCCESS;
}

static apr_status_t
cleanup_sec_buffer(void *data)
{
    FreeContextBuffer(data);

    return APR_SUCCESS;
}

apr_status_t
serf__kerb_create_sec_context(serf__kerb_context_t **ctx_p,
                              apr_pool_t *scratch_pool,
                              apr_pool_t *result_pool)
{
    SECURITY_STATUS sspi_status;
    serf__kerb_context_t *ctx;

    ctx = apr_pcalloc(result_pool, sizeof(*ctx));

    SecInvalidateHandle(&ctx->sspi_context);
    SecInvalidateHandle(&ctx->sspi_credentials);
    ctx->initalized = FALSE;

    apr_pool_cleanup_register(result_pool, ctx,
                              cleanup_ctx,
                              apr_pool_cleanup_null);

    sspi_status = AcquireCredentialsHandle(
        NULL, "Negotiate", SECPKG_CRED_OUTBOUND,
        NULL, NULL, NULL, NULL,
        &ctx->sspi_credentials, NULL);

    if (FAILED(sspi_status)) {
        return APR_EGENERAL;
    }

    *ctx_p = ctx;

    return APR_SUCCESS;
}

static apr_status_t
get_canonical_hostname(const char **canonname,
                       const char *hostname,
                       apr_pool_t *pool)
{
    struct addrinfo hints;
    struct addrinfo *addrinfo;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_flags = AI_CANONNAME;

    if (getaddrinfo(hostname, NULL, &hints, &addrinfo)) {
        return apr_get_netos_error();
    }

    if (addrinfo) {
        *canonname = apr_pstrdup(pool, addrinfo->ai_canonname);
    }
    else {
        *canonname = apr_pstrdup(pool, hostname);
    }

    freeaddrinfo(addrinfo);
    return APR_SUCCESS;
}

apr_status_t
serf__kerb_init_sec_context(serf__kerb_context_t *ctx,
                            const char *service,
                            const char *hostname,
                            serf__kerb_buffer_t *input_buf,
                            serf__kerb_buffer_t *output_buf,
                            apr_pool_t *scratch_pool,
                            apr_pool_t *result_pool
                            )
{
    SECURITY_STATUS status;
    ULONG actual_attr;
    SecBuffer sspi_in_buffer;
    SecBufferDesc sspi_in_buffer_desc;
    SecBuffer sspi_out_buffer;
    SecBufferDesc sspi_out_buffer_desc;
    char *target_name;
    apr_status_t apr_status;
    const char *canonname;

    apr_status = get_canonical_hostname(&canonname, hostname, scratch_pool);
    if (apr_status) {
        return apr_status;
    }
    target_name = apr_pstrcat(scratch_pool, service, "/", canonname, NULL);

    /* Prepare input buffer description. */
    sspi_in_buffer.BufferType = SECBUFFER_TOKEN;
    sspi_in_buffer.pvBuffer = input_buf->value;
    sspi_in_buffer.cbBuffer = input_buf->length; 

    sspi_in_buffer_desc.cBuffers = 1;
    sspi_in_buffer_desc.pBuffers = &sspi_in_buffer;
    sspi_in_buffer_desc.ulVersion = SECBUFFER_VERSION;

    /* Output buffers. Output buffer will be allocated by system. */
    sspi_out_buffer.BufferType = SECBUFFER_TOKEN;
    sspi_out_buffer.pvBuffer = NULL; 
    sspi_out_buffer.cbBuffer = 0;

    sspi_out_buffer_desc.cBuffers = 1;
    sspi_out_buffer_desc.pBuffers = &sspi_out_buffer;
    sspi_out_buffer_desc.ulVersion = SECBUFFER_VERSION;

    status = InitializeSecurityContext(
        &ctx->sspi_credentials,
        ctx->initalized ? &ctx->sspi_context : NULL,
        target_name,
        ISC_REQ_ALLOCATE_MEMORY
        | ISC_REQ_MUTUAL_AUTH
        | ISC_REQ_CONFIDENTIALITY,
        0,                          /* Reserved1 */
        SECURITY_NETWORK_DREP,
        &sspi_in_buffer_desc,
        0,                          /* Reserved2 */
        &ctx->sspi_context,
        &sspi_out_buffer_desc,
        &actual_attr,
        NULL);

    if (sspi_out_buffer.cbBuffer > 0) {
        apr_pool_cleanup_register(result_pool, sspi_out_buffer.pvBuffer,
                                  cleanup_sec_buffer,
                                  apr_pool_cleanup_null);
    }

    ctx->initalized = TRUE;

    /* Finish authentication if SSPI requires so. */
    if (status == SEC_I_COMPLETE_NEEDED
        || status == SEC_I_COMPLETE_AND_CONTINUE)
    {
        CompleteAuthToken(&ctx->sspi_context, &sspi_out_buffer_desc);
    }

    output_buf->value = sspi_out_buffer.pvBuffer;
    output_buf->length = sspi_out_buffer.cbBuffer;

    switch(status) {
    case SEC_I_COMPLETE_AND_CONTINUE:
    case SEC_I_CONTINUE_NEEDED:
        return APR_EAGAIN;

    case SEC_I_COMPLETE_NEEDED:
    case SEC_E_OK:
        return APR_SUCCESS;

    default:
        return APR_EGENERAL;
    }
}

#endif /* SERF_USE_SSPI */