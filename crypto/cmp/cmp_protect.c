/*
 * Copyright 2007-2020 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright Nokia 2007-2019
 * Copyright Siemens AG 2015-2019
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "cmp_local.h"

/* explicit #includes not strictly needed since implied by the above: */
#include <openssl/asn1t.h>
#include <openssl/cmp.h>
#include <openssl/crmf.h>
#include <openssl/err.h>
#include <openssl/x509.h>

DEFINE_STACK_OF(X509)

/*
 * This function is also used by the internal verify_PBMAC() in cmp_vfy.c.
 *
 * Calculate protection for given PKImessage according to
 * the algorithm and parameters in the message header's protectionAlg
 * using the credentials, library context, and property criteria in the ctx.
 *
 * returns ASN1_BIT_STRING representing the protection on success, else NULL
 */
ASN1_BIT_STRING *ossl_cmp_calc_protection(const OSSL_CMP_CTX *ctx,
                                          const OSSL_CMP_MSG *msg)
{
    ASN1_BIT_STRING *prot = NULL;
    OSSL_CMP_PROTECTEDPART prot_part;
    const ASN1_OBJECT *algorOID = NULL;
    const void *ppval = NULL;
    int pptype = 0;

    if (!ossl_assert(ctx != NULL && msg != NULL))
        return NULL;

    /* construct data to be signed */
    prot_part.header = msg->header;
    prot_part.body = msg->body;

    if (msg->header->protectionAlg == NULL) {
        CMPerr(0, CMP_R_UNKNOWN_ALGORITHM_ID);
        return NULL;
    }
    X509_ALGOR_get0(&algorOID, &pptype, &ppval, msg->header->protectionAlg);

    if (OBJ_obj2nid(algorOID) == NID_id_PasswordBasedMAC) {
        int len;
        size_t prot_part_der_len;
        unsigned char *prot_part_der = NULL;
        size_t sig_len;
        unsigned char *protection = NULL;
        OSSL_CRMF_PBMPARAMETER *pbm = NULL;
        ASN1_STRING *pbm_str = NULL;
        const unsigned char *pbm_str_uc = NULL;

        if (ctx->secretValue == NULL) {
            CMPerr(0, CMP_R_MISSING_PBM_SECRET);
            return NULL;
        }
        if (ppval == NULL) {
            CMPerr(0, CMP_R_ERROR_CALCULATING_PROTECTION);
            return NULL;
        }

        len = i2d_OSSL_CMP_PROTECTEDPART(&prot_part, &prot_part_der);
        if (len < 0 || prot_part_der == NULL) {
            CMPerr(0, CMP_R_ERROR_CALCULATING_PROTECTION);
            goto end;
        }
        prot_part_der_len = (size_t)len;

        pbm_str = (ASN1_STRING *)ppval;
        pbm_str_uc = pbm_str->data;
        pbm = d2i_OSSL_CRMF_PBMPARAMETER(NULL, &pbm_str_uc, pbm_str->length);
        if (pbm == NULL) {
            CMPerr(0, CMP_R_WRONG_ALGORITHM_OID);
            goto end;
        }

        if (!OSSL_CRMF_pbm_new(ctx->libctx, ctx->propq,
                               pbm, prot_part_der, prot_part_der_len,
                               ctx->secretValue->data, ctx->secretValue->length,
                               &protection, &sig_len))
            goto end;

        if ((prot = ASN1_BIT_STRING_new()) == NULL)
            return NULL;
        /* OpenSSL defaults all bit strings to be encoded as ASN.1 NamedBitList */
        prot->flags &= ~(ASN1_STRING_FLAG_BITS_LEFT | 0x07);
        prot->flags |= ASN1_STRING_FLAG_BITS_LEFT;
        if (!ASN1_BIT_STRING_set(prot, protection, sig_len)) {
            ASN1_BIT_STRING_free(prot);
            prot = NULL;
        }
    end:
        OSSL_CRMF_PBMPARAMETER_free(pbm);
        OPENSSL_free(protection);
        OPENSSL_free(prot_part_der);
        return prot;
    } else {
        int md_nid;
        const EVP_MD *md = NULL;

        if (ctx->pkey == NULL) {
            CMPerr(0, CMP_R_MISSING_KEY_INPUT_FOR_CREATING_PROTECTION);
            return NULL;
        }
        if (!OBJ_find_sigid_algs(OBJ_obj2nid(algorOID), &md_nid, NULL)
                || (md = EVP_get_digestbynid(md_nid)) == NULL) {
            CMPerr(0, CMP_R_UNKNOWN_ALGORITHM_ID);
            return NULL;
        }

        if ((prot = ASN1_BIT_STRING_new()) == NULL)
            return NULL;
        if (ASN1_item_sign_with_libctx(ASN1_ITEM_rptr(OSSL_CMP_PROTECTEDPART),
                                       NULL, NULL, prot, &prot_part, NULL,
                                       ctx->pkey, md, ctx->libctx, ctx->propq))
            return prot;
        ASN1_BIT_STRING_free(prot);
        return NULL;
    }
}

int ossl_cmp_msg_add_extraCerts(OSSL_CMP_CTX *ctx, OSSL_CMP_MSG *msg)
{
    if (!ossl_assert(ctx != NULL && msg != NULL))
        return 0;

    if (msg->extraCerts == NULL
            && (msg->extraCerts = sk_X509_new_null()) == NULL)
        return 0;

    if (ctx->cert != NULL && ctx->pkey != NULL) {
        /* make sure that our own cert is included in the first position */
        if (!X509_add_cert(msg->extraCerts, ctx->cert,
                           X509_ADD_FLAG_UP_REF | X509_ADD_FLAG_NO_DUP
                           | X509_ADD_FLAG_PREPEND))
            return 0;
        /* if we have untrusted certs, try to add intermediate certs */
        if (ctx->untrusted_certs != NULL) {
            STACK_OF(X509) *chain =
                ossl_cmp_build_cert_chain(ctx->libctx, ctx->propq,
                                          ctx->untrusted_certs, ctx->cert);
            int res = X509_add_certs(msg->extraCerts, chain,
                                     X509_ADD_FLAG_UP_REF | X509_ADD_FLAG_NO_DUP
                                     | X509_ADD_FLAG_NO_SS);

            sk_X509_pop_free(chain, X509_free);
            if (res == 0)
                return 0;
        }
    }

    /* add any additional certificates from ctx->extraCertsOut */
    if (!X509_add_certs(msg->extraCerts, ctx->extraCertsOut,
                        X509_ADD_FLAG_UP_REF | X509_ADD_FLAG_NO_DUP))
        return 0;

    /* if none was found avoid empty ASN.1 sequence */
    if (sk_X509_num(msg->extraCerts) == 0) {
        sk_X509_free(msg->extraCerts);
        msg->extraCerts = NULL;
    }
    return 1;
}

/*
 * Create an X509_ALGOR structure for PasswordBasedMAC protection based on
 * the pbm settings in the context
 */
static int set_pbmac_algor(const OSSL_CMP_CTX *ctx, X509_ALGOR **alg)
{
    OSSL_CRMF_PBMPARAMETER *pbm = NULL;
    unsigned char *pbm_der = NULL;
    int pbm_der_len;
    ASN1_STRING *pbm_str = NULL;

    if (!ossl_assert(ctx != NULL))
        return 0;

    pbm = OSSL_CRMF_pbmp_new(ctx->libctx, ctx->pbm_slen,
                             EVP_MD_type(ctx->pbm_owf), ctx->pbm_itercnt,
                             ctx->pbm_mac);
    pbm_str = ASN1_STRING_new();
    if (pbm == NULL || pbm_str == NULL)
        goto err;

    if ((pbm_der_len = i2d_OSSL_CRMF_PBMPARAMETER(pbm, &pbm_der)) < 0)
        goto err;

    if (!ASN1_STRING_set(pbm_str, pbm_der, pbm_der_len))
        goto err;
    if (*alg == NULL && (*alg = X509_ALGOR_new()) == NULL)
        goto err;
    OPENSSL_free(pbm_der);

    X509_ALGOR_set0(*alg, OBJ_nid2obj(NID_id_PasswordBasedMAC),
                    V_ASN1_SEQUENCE, pbm_str);
    OSSL_CRMF_PBMPARAMETER_free(pbm);
    return 1;

 err:
    ASN1_STRING_free(pbm_str);
    OPENSSL_free(pbm_der);
    OSSL_CRMF_PBMPARAMETER_free(pbm);
    return 0;
}

static int set_sig_algor(const OSSL_CMP_CTX *ctx, X509_ALGOR **alg)
{
    int nid = 0;
    ASN1_OBJECT *algo = NULL;

    if (!OBJ_find_sigid_by_algs(&nid, EVP_MD_type(ctx->digest),
                                EVP_PKEY_id(ctx->pkey))) {
        CMPerr(0, CMP_R_UNSUPPORTED_KEY_TYPE);
        return 0;
    }
    if ((algo = OBJ_nid2obj(nid)) == NULL)
        return 0;
    if (*alg == NULL && (*alg = X509_ALGOR_new()) == NULL)
        return 0;

    if (X509_ALGOR_set0(*alg, algo, V_ASN1_UNDEF, NULL))
        return 1;
    ASN1_OBJECT_free(algo);
    return 0;
}

static int set_senderKID(const OSSL_CMP_CTX *ctx, OSSL_CMP_MSG *msg,
                         const ASN1_OCTET_STRING *id)
{
    if (id == NULL)
        id = ctx->referenceValue; /* standard for PBM, fallback for sig-based */
    return id == NULL || ossl_cmp_hdr_set1_senderKID(msg->header, id);
}

int ossl_cmp_msg_protect(OSSL_CMP_CTX *ctx, OSSL_CMP_MSG *msg)
{
    if (!ossl_assert(ctx != NULL && msg != NULL))
        return 0;

    /*
     * For the case of re-protection remove pre-existing protection.
     * TODO: Consider also removing any pre-existing extraCerts.
     */
    X509_ALGOR_free(msg->header->protectionAlg);
    msg->header->protectionAlg = NULL;
    ASN1_BIT_STRING_free(msg->protection);
    msg->protection = NULL;

    if (ctx->unprotectedSend)
        return 1;

    /* use PasswordBasedMac according to 5.1.3.1 if secretValue is given */
    if (ctx->secretValue != NULL) {
        if (!set_pbmac_algor(ctx, &msg->header->protectionAlg))
            goto err;
        if (!set_senderKID(ctx, msg, NULL))
            goto err;

        /*
         * will add any additional certificates from ctx->extraCertsOut
         * while not needed to validate the protection certificate,
         * the option to do this might be handy for certain use cases
         */
    } else if (ctx->cert != NULL && ctx->pkey != NULL) {
        /* use MSG_SIG_ALG according to 5.1.3.3 if client cert and key given */

        /* make sure that key and certificate match */
        if (!X509_check_private_key(ctx->cert, ctx->pkey)) {
            CMPerr(0, CMP_R_CERT_AND_KEY_DO_NOT_MATCH);
            goto err;
        }

        if (!set_sig_algor(ctx, &msg->header->protectionAlg))
            goto err;
        /* set senderKID to keyIdentifier of the cert according to 5.1.1 */
        if (!set_senderKID(ctx, msg, X509_get0_subject_key_id(ctx->cert)))
            goto err;

        /*
         * will add ctx->cert followed, if possible, by its chain built
         * from ctx->untrusted_certs, and then ctx->extraCertsOut
         */
    } else {
        CMPerr(0, CMP_R_MISSING_KEY_INPUT_FOR_CREATING_PROTECTION);
        goto err;
    }
    if ((msg->protection = ossl_cmp_calc_protection(ctx, msg)) == NULL)
        goto err;

    /*
     * If present, add ctx->cert followed by its chain as far as possible.
     * Finally add any additional certificates from ctx->extraCertsOut;
     * even if not needed to validate the protection
     * the option to do this might be handy for certain use cases.
     */
    if (!ossl_cmp_msg_add_extraCerts(ctx, msg))
        goto err;

    /*
     * As required by RFC 4210 section 5.1.1., if the sender name is not known
     * to the client it set to NULL-DN. In this case for identification at least
     * the senderKID must be set, where we took the referenceValue as fallback.
     */
    if (ossl_cmp_general_name_is_NULL_DN(msg->header->sender)
            && msg->header->senderKID == NULL)
        CMPerr(0, CMP_R_MISSING_SENDER_IDENTIFICATION);
    else
        return 1;

 err:
    CMPerr(0, CMP_R_ERROR_PROTECTING_MESSAGE);
    return 0;
}
