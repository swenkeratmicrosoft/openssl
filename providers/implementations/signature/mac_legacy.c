/*
 * Copyright 2019-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/err.h>
#include "prov/implementations.h"
#include "prov/provider_ctx.h"
#include "prov/macsignature.h"

static OSSL_FUNC_signature_newctx_fn mac_hmac_newctx;
static OSSL_FUNC_signature_digest_sign_init_fn mac_digest_sign_init;
static OSSL_FUNC_signature_digest_sign_update_fn mac_digest_sign_update;
static OSSL_FUNC_signature_digest_sign_final_fn mac_digest_sign_final;
static OSSL_FUNC_signature_freectx_fn mac_freectx;
static OSSL_FUNC_signature_dupctx_fn mac_dupctx;

typedef struct {
    OPENSSL_CTX *libctx;
    char *propq;
    MAC_KEY *key;
    EVP_MAC_CTX *macctx;
} PROV_MAC_CTX;

static void *mac_newctx(void *provctx, const char *propq, const char *macname)
{
    PROV_MAC_CTX *pmacctx = OPENSSL_zalloc(sizeof(PROV_MAC_CTX));
    EVP_MAC *mac = NULL;

    if (pmacctx == NULL)
        return NULL;

    pmacctx->libctx = PROV_LIBRARY_CONTEXT_OF(provctx);
    if (propq != NULL && (pmacctx->propq = OPENSSL_strdup(propq)) == NULL) {
        ERR_raise(ERR_LIB_PROV, ERR_R_MALLOC_FAILURE);
        goto err;
    }

    mac = EVP_MAC_fetch(pmacctx->libctx, macname, propq);
    if (mac == NULL)
        goto err;

    pmacctx->macctx = EVP_MAC_CTX_new(mac);
    if (pmacctx->macctx == NULL)
        goto err;

    EVP_MAC_free(mac);

    return pmacctx;

 err:
    OPENSSL_free(pmacctx);
    EVP_MAC_free(mac);
    return NULL;
}

#define MAC_NEWCTX(funcname, macname) \
    static void *mac_##funcname##_newctx(void *provctx, const char *propq) \
    { \
        return mac_newctx(provctx, propq, macname); \
    }

MAC_NEWCTX(hmac, "HMAC")
MAC_NEWCTX(siphash, "SIPHASH")

static int mac_digest_sign_init(void *vpmacctx, const char *mdname, void *vkey)
{
    PROV_MAC_CTX *pmacctx = (PROV_MAC_CTX *)vpmacctx;
    OSSL_PARAM params[4], *p = params;

    if (pmacctx == NULL || vkey == NULL || !mac_key_up_ref(vkey))
        return 0;


    mac_key_free(pmacctx->key);
    pmacctx->key = vkey;

    /* Read only so cast away of const is safe */
    if (mdname != NULL)
        *p++ = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                                (char *)mdname, 0);
    *p++ = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY,
                                             pmacctx->key->priv_key,
                                             pmacctx->key->priv_key_len);
    *p = OSSL_PARAM_construct_end();

    if (!EVP_MAC_CTX_set_params(pmacctx->macctx, params))
        return 0;

    if (!EVP_MAC_init(pmacctx->macctx))
        return 0;

    return 1;
}

int mac_digest_sign_update(void *vpmacctx, const unsigned char *data,
                           size_t datalen)
{
    PROV_MAC_CTX *pmacctx = (PROV_MAC_CTX *)vpmacctx;

    if (pmacctx == NULL || pmacctx->macctx == NULL)
        return 0;

    return EVP_MAC_update(pmacctx->macctx, data, datalen);
}

int mac_digest_sign_final(void *vpmacctx, unsigned char *mac, size_t *maclen,
                          size_t macsize)
{
    PROV_MAC_CTX *pmacctx = (PROV_MAC_CTX *)vpmacctx;

    if (pmacctx == NULL || pmacctx->macctx == NULL)
        return 0;

    return EVP_MAC_final(pmacctx->macctx, mac, maclen, macsize);
}

static void mac_freectx(void *vpmacctx)
{
    PROV_MAC_CTX *ctx = (PROV_MAC_CTX *)vpmacctx;

    OPENSSL_free(ctx->propq);
    EVP_MAC_CTX_free(ctx->macctx);
    mac_key_free(ctx->key);
    OPENSSL_free(ctx);
}

static void *mac_dupctx(void *vpmacctx)
{
    PROV_MAC_CTX *srcctx = (PROV_MAC_CTX *)vpmacctx;
    PROV_MAC_CTX *dstctx;

    dstctx = OPENSSL_zalloc(sizeof(*srcctx));
    if (dstctx == NULL)
        return NULL;

    *dstctx = *srcctx;
    dstctx->key = NULL;
    dstctx->macctx = NULL;

    if (srcctx->key != NULL && !mac_key_up_ref(srcctx->key))
        goto err;
    dstctx->key = srcctx->key;

    if (srcctx->macctx != NULL) {
        dstctx->macctx = EVP_MAC_CTX_dup(srcctx->macctx);
        if (dstctx->macctx == NULL)
            goto err;
    }

    return dstctx;
 err:
    mac_freectx(dstctx);
    return NULL;
}

#define MAC_SIGNATURE_FUNCTIONS(funcname) \
    const OSSL_DISPATCH mac_##funcname##_signature_functions[] = { \
        { OSSL_FUNC_SIGNATURE_NEWCTX, (void (*)(void))mac_##funcname##_newctx }, \
        { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_INIT, \
        (void (*)(void))mac_digest_sign_init }, \
        { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_UPDATE, \
        (void (*)(void))mac_digest_sign_update }, \
        { OSSL_FUNC_SIGNATURE_DIGEST_SIGN_FINAL, \
        (void (*)(void))mac_digest_sign_final }, \
        { OSSL_FUNC_SIGNATURE_FREECTX, (void (*)(void))mac_freectx }, \
        { OSSL_FUNC_SIGNATURE_DUPCTX, (void (*)(void))mac_dupctx }, \
        { 0, NULL } \
    };

MAC_SIGNATURE_FUNCTIONS(hmac)
MAC_SIGNATURE_FUNCTIONS(siphash)
