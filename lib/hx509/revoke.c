/*
 * Copyright (c) 2006 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "hx_locl.h"
RCSID("$Id$");

struct revoke_crl {
    char *path;
    time_t last_modfied;
    CRLCertificateList crl;
    int verified;
};

struct revoke_ocsp {
    char *path;
    time_t last_modfied;
    OCSPBasicOCSPResponse ocsp;
    hx509_certs certs;
    hx509_cert signer;
};


struct hx509_revoke_ctx_data {
    struct {
	struct revoke_crl *val;
	size_t len;
    } crls;
    struct {
	struct revoke_ocsp *val;
	size_t len;
    } ocsps;
};

int
hx509_revoke_init(hx509_context context, hx509_revoke_ctx *revoke)
{
    *revoke = calloc(1, sizeof(**revoke));
    if (*revoke == NULL)
	return ENOMEM;

    (*revoke)->crls.len = 0;
    (*revoke)->crls.val = NULL;
    (*revoke)->ocsps.len = 0;
    (*revoke)->ocsps.val = NULL;

    return 0;
}

void
hx509_revoke_free(hx509_revoke_ctx *revoke)
{
    size_t i ;

    for (i = 0; i < (*revoke)->crls.len; i++) {
	free((*revoke)->crls.val[i].path);
	free_CRLCertificateList(&(*revoke)->crls.val[i].crl);
    }
    for (i = 0; i < (*revoke)->ocsps.len; i++) {
	free((*revoke)->ocsps.val[i].path);
	free_OCSPBasicOCSPResponse(&(*revoke)->ocsps.val[i].ocsp);
	hx509_certs_free(&(*revoke)->ocsps.val[i].certs);
	hx509_cert_free((*revoke)->ocsps.val[i].signer);
    }
    free((*revoke)->crls.val);

    memset(*revoke, 0, sizeof(**revoke));
    free(*revoke);
    *revoke = NULL;
}

static int
verify_ocsp(hx509_context context,
	    struct revoke_ocsp *ocsp,
	    time_t time_now,
	    hx509_certs certs,
	    hx509_cert parent)
{
    hx509_cert signer = NULL;
    hx509_query q;
    int ret;
	
    _hx509_query_clear(&q);
	
    switch(ocsp->ocsp.tbsResponseData.responderID.element) {
    case choice_OCSPResponderID_byName:
	q.match = HX509_QUERY_MATCH_SUBJECT_NAME;
	q.subject_name = &ocsp->ocsp.tbsResponseData.responderID.u.byName;
	break;
    case choice_OCSPResponderID_byKey:
	q.match = HX509_QUERY_MATCH_KEY_HASH_SHA1;
	q.keyhash_sha1 = &ocsp->ocsp.tbsResponseData.responderID.u.byKey;
	break;
    }
	
    ret = hx509_certs_find(context, certs, &q, &signer);
    if (ret && ocsp->certs)
	ret = hx509_certs_find(context, ocsp->certs, &q, &signer);
    if (ret)
	goto out;

    /*
     * If signer certificate isn't the CA certificate, lets check the
     * its the CA that signed the signer certificate and the OCSP EKU
     * is set.
     */
    if (hx509_cert_cmp(signer, parent) != 0) {
	Certificate *p = _hx509_get_cert(parent);
	Certificate *s = _hx509_get_cert(signer);

	ret = _hx509_cert_is_parent_cmp(s, p, 0);
	if (ret != 0) {
	    ret = EINVAL;
	    goto out;
	}

	ret = _hx509_verify_signature_bitstring(p,
						&s->signatureAlgorithm,
						&s->tbsCertificate._save,
						&s->signatureValue);
	if (ret)
	    goto out;

	ret = hx509_cert_check_eku(context, signer, 
				   oid_id_kp_OCSPSigning(), 0);
	if (ret)
	    goto out;
    }

    ret = _hx509_verify_signature_bitstring(_hx509_get_cert(signer), 
					    &ocsp->ocsp.signatureAlgorithm,
					    &ocsp->ocsp.tbsResponseData._save,
					    &ocsp->ocsp.signature);
    if (ret)
	goto out;

    ocsp->signer = signer;
    signer = NULL;
out:
    if (signer)
	hx509_cert_free(signer);

    return ret;
}

/*
 *
 */

static int
load_ocsp(hx509_context context, struct revoke_ocsp *ocsp)
{
    OCSPResponse resp;
    OCSPBasicOCSPResponse basic;
    hx509_certs certs = NULL;
    size_t length, size;
    struct stat sb;
    void *data;
    int ret;

    ret = _hx509_map_file(ocsp->path, &data, &length, &sb);
    if (ret)
	return ret;

    ret = decode_OCSPResponse(data, length, &resp, &size);
    _hx509_unmap_file(data, length);
    if (ret)
	return ret;
    if (length != size) {
	free_OCSPResponse(&resp);
	return EINVAL;
    }

    switch (resp.responseStatus) {
    case successful:
	break;
    default:
	free_OCSPResponse(&resp);
	return EINVAL;
    }

    if (resp.responseBytes == NULL) {
	free_OCSPResponse(&resp);
	return EINVAL;
    }

    ret = heim_oid_cmp(&resp.responseBytes->responseType, 
		       oid_id_pkix_ocsp_basic());
    if (ret != 0) {
	free_OCSPResponse(&resp);
	return EINVAL;
    }

    ret = decode_OCSPBasicOCSPResponse(resp.responseBytes->response.data,
				       resp.responseBytes->response.length,
				       &basic,
				       &size);
    if (ret) {
	free_OCSPResponse(&resp);
	return ret;
    }
    if (size != resp.responseBytes->response.length) {
	free_OCSPResponse(&resp);
	free_OCSPBasicOCSPResponse(&basic);
	return EINVAL;
    }
    free_OCSPResponse(&resp);

    if (basic.certs) {
	int i;

	ret = hx509_certs_init(context, "MEMORY:ocsp-certs", 0, 
			       NULL, &certs);
	if (ret) {
	    free_OCSPBasicOCSPResponse(&basic);
	    return ret;
	}

	for (i = 0; i < basic.certs->len; i++) {
	    hx509_cert c;
	    
	    ret = hx509_cert_init(context, &basic.certs->val[i], &c);
	    if (ret)
		continue;
	    
	    ret = hx509_certs_add(context, certs, c);
	    if (ret)
		continue;
	}
    }

    ocsp->last_modfied = sb.st_mtime;

    free_OCSPBasicOCSPResponse(&ocsp->ocsp);
    hx509_certs_free(&ocsp->certs);
    hx509_cert_free(ocsp->signer);

    ocsp->ocsp = basic;
    ocsp->certs = certs;
    ocsp->signer = NULL;

    return 0;
}

int
hx509_revoke_add_ocsp(hx509_context context,
		      hx509_revoke_ctx revoke,
		      const char *path)
{
    void *data;
    int ret;
    size_t i;

    if (strncmp(path, "FILE:", 5) != 0)
	return EINVAL;

    path += 5;

    for (i = 0; i < revoke->ocsps.len; i++) {
	if (strcmp(revoke->ocsps.val[0].path, path) == 0)
	    return 0;
    }

    data = realloc(revoke->ocsps.val, 
		   (revoke->ocsps.len + 1) * sizeof(revoke->ocsps.val[0]));
    if (data == NULL)
	return ENOMEM;

    revoke->ocsps.val = data;

    memset(&revoke->ocsps.val[revoke->ocsps.len], 0, 
	   sizeof(revoke->ocsps.val[0]));

    revoke->ocsps.val[revoke->ocsps.len].path = strdup(path);
    if (revoke->ocsps.val[revoke->ocsps.len].path == NULL)
	return ENOMEM;

    ret = load_ocsp(context, &revoke->ocsps.val[revoke->ocsps.len]);
    if (ret) {
	free(revoke->ocsps.val[revoke->ocsps.len].path);
	return ret;
    }
    revoke->ocsps.len++;

    return ret;
}

/*
 *
 */

static int
verify_crl(hx509_context context,
	   CRLCertificateList *crl,
	   time_t time_now,
	   hx509_certs certs,
	   hx509_cert parent)
{
    hx509_cert signer;
    hx509_query q;
    time_t t;
    int ret;
	
    t = _hx509_Time2time_t(&crl->tbsCertList.thisUpdate);
    if (t > time_now)
	return HX509_CRL_USED_BEFORE_TIME;

    if (crl->tbsCertList.nextUpdate == NULL)
	return HX509_CRL_INVALID_FORMAT;

    t = _hx509_Time2time_t(crl->tbsCertList.nextUpdate);
    if (t < time_now)
	return HX509_CRL_USED_AFTER_TIME;

    _hx509_query_clear(&q);
	
    q.match = HX509_QUERY_MATCH_SUBJECT_NAME;
    q.subject_name = &crl->tbsCertList.issuer;
	
    ret = hx509_certs_find(context, certs, &q, &signer);
    if (ret)
	return ret;

    /* verify is parent or CRLsigner */
    if (hx509_cert_cmp(signer, parent) != 0) {
	Certificate *p = _hx509_get_cert(parent);
	Certificate *s = _hx509_get_cert(signer);

	ret = _hx509_cert_is_parent_cmp(s, p, 0);
	if (ret != 0) {
	    ret = EINVAL;
	    goto out;
	}

	ret = _hx509_verify_signature_bitstring(p,
						&s->signatureAlgorithm,
						&s->tbsCertificate._save,
						&s->signatureValue);
	if (ret)
	    goto out;

	ret = _hx509_check_key_usage(signer, 1 << 6, TRUE); /* crl */
	if (ret != 0)
	    goto out;
    }

    ret = _hx509_verify_signature_bitstring(_hx509_get_cert(signer), 
					    &crl->signatureAlgorithm,
					    &crl->tbsCertList._save,
					    &crl->signatureValue);
    if (ret)
	goto out;

out:
    hx509_cert_free(signer);

    return ret;
}

static int
load_crl(const char *path, time_t *t, CRLCertificateList *crl)
{
    size_t length, size;
    struct stat sb;
    void *data;
    int ret;

    memset(crl, 0, sizeof(*crl));

    ret = _hx509_map_file(path, &data, &length, &sb);
    if (ret)
	return ret;

    *t = sb.st_mtime;

    ret = decode_CRLCertificateList(data, length, crl, &size);
    _hx509_unmap_file(data, length);
    if (ret)
	return ret;

    /* check signature is aligned */
    if (crl->signatureValue.length & 7) {
	free_CRLCertificateList(crl);
	return EINVAL;
    }
    return 0;
}

int
hx509_revoke_add_crl(hx509_context context,
		     hx509_revoke_ctx revoke,
		     const char *path)
{
    void *data;
    size_t i;
    int ret;

    if (strncmp(path, "FILE:", 5) != 0)
	return EINVAL;
    
    path += 5;

    for (i = 0; i < revoke->crls.len; i++) {
	if (strcmp(revoke->crls.val[0].path, path) == 0)
	    return 0;
    }

    data = realloc(revoke->crls.val, 
		   (revoke->crls.len + 1) * sizeof(revoke->crls.val[0]));
    if (data == NULL)
	return ENOMEM;

    revoke->crls.val = data;

    memset(&revoke->crls.val[revoke->crls.len], 0, sizeof(revoke->crls.val[0]));

    revoke->crls.val[revoke->crls.len].path = strdup(path);
    if (revoke->crls.val[revoke->crls.len].path == NULL)
	return ENOMEM;

    ret = load_crl(path, 
		   &revoke->crls.val[revoke->crls.len].last_modfied,
		   &revoke->crls.val[revoke->crls.len].crl);
    if (ret) {
	free(revoke->crls.val[revoke->crls.len].path);
	return ret;
    }

    revoke->crls.len++;

    return ret;
}


int
hx509_revoke_verify(hx509_context context,
		    hx509_revoke_ctx revoke,
		    hx509_certs certs,
		    time_t now,
		    hx509_cert cert,
		    hx509_cert parent_cert)
{
    const Certificate *c = _hx509_get_cert(cert);
    const Certificate *p = _hx509_get_cert(parent_cert);
    unsigned long i, j, k;
    int ret;

    for (i = 0; i < revoke->ocsps.len; i++) {
	struct revoke_ocsp *ocsp = &revoke->ocsps.val[i];
	struct stat sb;

	/* check this ocsp apply to this cert */

	/* check if there is a newer version of the file */
	ret = stat(ocsp->path, &sb);
	if (ret == 0 && ocsp->last_modfied != sb.st_mtime) {
	    ret = load_ocsp(context, ocsp);
	    if (ret)
		continue;
	}

	/* verify signature in ocsp if not already done */
	if (ocsp->signer == NULL) {
	    ret = verify_ocsp(context, ocsp, now, certs, parent_cert);
	    if (ret)
		continue;
	}

	for (i = 0; i < ocsp->ocsp.tbsResponseData.responses.len; i++) {
	    heim_octet_string os;

	    ret = heim_integer_cmp(&ocsp->ocsp.tbsResponseData.responses.val[i].certID.serialNumber,
				   &c->tbsCertificate.serialNumber);
	    if (ret != 0)
		continue;
	    
	    /* verify issuer hashes hash */
	    ret = _hx509_verify_signature(NULL,
					  &ocsp->ocsp.tbsResponseData.responses.val[i].certID.hashAlgorithm,
					  &c->tbsCertificate.issuer._save,
					  &ocsp->ocsp.tbsResponseData.responses.val[i].certID.issuerNameHash);
	    if (ret != 0)
		continue;

	    os.data = p->tbsCertificate.subjectPublicKeyInfo.subjectPublicKey.data;
	    os.length = p->tbsCertificate.subjectPublicKeyInfo.subjectPublicKey.length / 8;

	    ret = _hx509_verify_signature(NULL,
					  &ocsp->ocsp.tbsResponseData.responses.val[i].certID.hashAlgorithm,
					  &os,
					  &ocsp->ocsp.tbsResponseData.responses.val[i].certID.issuerKeyHash);
	    if (ret != 0)
		continue;

	    switch (ocsp->ocsp.tbsResponseData.responses.val[i].certStatus.element) {
	    case choice_OCSPCertStatus_good:
		break;
	    case choice_OCSPCertStatus_revoked:
	    case choice_OCSPCertStatus_unknown:
		continue;
	    }

	    /* don't allow the update to be in the future */
	    if (ocsp->ocsp.tbsResponseData.responses.val[i].thisUpdate > 
		now + context->ocsp_time_diff)
		continue;

	    /* don't allow the next updte to be in the past */
	    if (ocsp->ocsp.tbsResponseData.responses.val[i].nextUpdate) {
		if (*ocsp->ocsp.tbsResponseData.responses.val[i].nextUpdate < now)
		    continue;
	    } else
		/* Should force a refetch, but can we ? */;

	    return 0;
	}
    }

    for (i = 0; i < revoke->crls.len; i++) {
	struct revoke_crl *crl = &revoke->crls.val[i];
	struct stat sb;

	/* check if cert.issuer == crls.val[i].crl.issuer */
	ret = _hx509_name_cmp(&c->tbsCertificate.issuer, 
			      &crl->crl.tbsCertList.issuer);
	if (ret)
	    continue;

	ret = stat(crl->path, &sb);
	if (ret == 0 && crl->last_modfied != sb.st_mtime) {
	    CRLCertificateList c;

	    ret = load_crl(crl->path, &crl->last_modfied, &c);
	    if (ret == 0) {
		free_CRLCertificateList(&crl->crl);
		crl->crl = c;
		crl->verified = 0;
	    }
	}

	/* verify signature in crl if not already done */
	if (crl->verified == 0) {
	    ret = verify_crl(context, &crl->crl, now, certs, parent_cert);
	    if (ret)
		return ret;
	    crl->verified = 1;
	}
	
	if (crl->crl.tbsCertList.crlExtensions)
	    for (j = 0; j < crl->crl.tbsCertList.crlExtensions->len; j++)
		if (crl->crl.tbsCertList.crlExtensions->val[j].critical)
		    return HX509_CRL_UNKNOWN_EXTENSION;

	if (crl->crl.tbsCertList.revokedCertificates == NULL)
	    return 0;

	/* check if cert is in crl */
	for (j = 0; j < crl->crl.tbsCertList.revokedCertificates->len; j++) {
	    time_t t;

	    ret = heim_integer_cmp(&crl->crl.tbsCertList.revokedCertificates->val[j].userCertificate,
				   &c->tbsCertificate.serialNumber);
	    if (ret != 0)
		continue;

	    t = _hx509_Time2time_t(&crl->crl.tbsCertList.revokedCertificates->val[j].revocationDate);
	    if (t > now)
		continue;
	    
	    if (crl->crl.tbsCertList.revokedCertificates->val[j].crlEntryExtensions)
		for (k = 0; k < crl->crl.tbsCertList.revokedCertificates->val[j].crlEntryExtensions->len; k++)
		    if (crl->crl.tbsCertList.revokedCertificates->val[j].crlEntryExtensions->val[k].critical)
			return HX509_CRL_UNKNOWN_EXTENSION;
	    
	    return HX509_CRL_CERT_REVOKED;
	}
	return 0;
    }


    if (context->flags & HX509_CTX_VERIFY_MISSING_OK)
	return 0;
    return HX509_REVOKE_STATUS_MISSING;
}

struct ocsp_add_ctx {
    OCSPTBSRequest *req;
    hx509_certs certs;
    const AlgorithmIdentifier *digest;
    hx509_cert parent;
};

static int
add_to_req(hx509_context context, void *ptr, hx509_cert cert)
{
    struct ocsp_add_ctx *ctx = ptr;
    OCSPInnerRequest *one;
    hx509_cert parent = NULL;
    Certificate *p, *c = _hx509_get_cert(cert);
    heim_octet_string os;
    int ret;
    hx509_query q;
    void *d;

    d = realloc(ctx->req->requestList.val, 
		sizeof(ctx->req->requestList.val[0]) *
		(ctx->req->requestList.len + 1));
    if (d == NULL)
	return ENOMEM;
    ctx->req->requestList.val = d;
    
    one = &ctx->req->requestList.val[ctx->req->requestList.len];
    memset(one, 0, sizeof(*one));

    _hx509_query_clear(&q);

    q.match |= HX509_QUERY_FIND_ISSUER_CERT;
    q.subject = c;

    ret = hx509_certs_find(context, ctx->certs, &q, &parent);
    if (ret)
	goto out;

    if (ctx->parent) {
	if (hx509_cert_cmp(ctx->parent, parent) != 0) {
	    ret = EINVAL;
	    goto out;
	}
    } else
	ctx->parent = hx509_cert_ref(parent);

    p = _hx509_get_cert(parent);

    ret = copy_AlgorithmIdentifier(ctx->digest, &one->reqCert.hashAlgorithm);
    if (ret)
	goto out;

    ret = _hx509_create_signature(NULL,
				  &one->reqCert.hashAlgorithm,
				  &c->tbsCertificate.issuer._save,
				  NULL,
				  &one->reqCert.issuerNameHash);
    if (ret)
	goto out;

    os.data = p->tbsCertificate.subjectPublicKeyInfo.subjectPublicKey.data;
    os.length = 
	p->tbsCertificate.subjectPublicKeyInfo.subjectPublicKey.length / 8;

    ret = _hx509_create_signature(NULL,
				  &one->reqCert.hashAlgorithm,
				  &os,
				  NULL,
				  &one->reqCert.issuerKeyHash);
    if (ret)
	goto out;

    ret = copy_CertificateSerialNumber(&c->tbsCertificate.serialNumber,
				       &one->reqCert.serialNumber);
    if (ret)
	goto out;

    ctx->req->requestList.len++;
out:
    hx509_cert_free(parent);
    if (ret) {
	free_OCSPInnerRequest(one);
	memset(one, 0, sizeof(*one));
    }

    return ret;
}


int
hx509_ocsp_request(hx509_context context,
		   hx509_certs reqcerts,
		   hx509_certs pool,
		   hx509_cert signer,
		   const AlgorithmIdentifier *digest,
		   heim_octet_string *request,
		   heim_octet_string *nonce)
{
    OCSPRequest req;
    size_t size;
    int ret;
    struct ocsp_add_ctx ctx;
    Extensions *es;

    memset(&req, 0, sizeof(req));

    if (digest == NULL)
	digest = hx509_signature_sha1();

    ctx.req = &req.tbsRequest;
    ctx.certs = pool;
    ctx.digest = digest;
    ctx.parent = NULL;

    ret = hx509_certs_iter(context, reqcerts, add_to_req, &ctx);
    hx509_cert_free(ctx.parent);
    if (ret) {
	free_OCSPRequest(&req);
	return ret;
    }
    
    if (nonce) {

	req.tbsRequest.requestExtensions = 
	    calloc(1, sizeof(*req.tbsRequest.requestExtensions));
	if (req.tbsRequest.requestExtensions == NULL) {
	    free_OCSPRequest(&req);
	    return ENOMEM;
	}

	es = req.tbsRequest.requestExtensions;
	
	es->len = 1;
	es->val = calloc(es->len, sizeof(es->val[0]));
	
	ret = copy_oid(oid_id_pkix_ocsp_nonce(), &es->val[0].extnID);
	if (ret)
	    abort();
	
	es->val[0].extnValue.data = malloc(10);
	if (es->val[0].extnValue.data == NULL) {
	    free_OCSPRequest(&req);
	    return ENOMEM;
	}
	es->val[0].extnValue.length = 10;
	
	ret = RAND_bytes(es->val[0].extnValue.data,
			 es->val[0].extnValue.length);
	if (ret != 1) {
	    free_OCSPRequest(&req);
	    return HX509_CRYPTO_INTERNAL_ERROR;
	}
    }

    ASN1_MALLOC_ENCODE(OCSPRequest, request->data, request->length,
		       &req, &size, ret);
    free_OCSPRequest(&req);
    if (ret)
	return ret;
    if (size != request->length)
	_hx509_abort("internal ASN.1 encoder error");


    return 0;
}
