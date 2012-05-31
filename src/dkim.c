/* Copyright (c) 2010-2011, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "main.h"
#include "message.h"
#include "dkim.h"
#include "dns.h"

/* Parser of dkim params */
typedef gboolean (*dkim_parse_param_f) (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);

static gboolean rspamd_dkim_parse_signature (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_signalg (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_domain (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_canonalg (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_ignore (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_selector (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_hdrlist (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_version (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_timestamp (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_expiration (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_bodyhash (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);
static gboolean rspamd_dkim_parse_bodylength (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err);


static const dkim_parse_param_f parser_funcs[] = {
	[DKIM_PARAM_SIGNATURE] = rspamd_dkim_parse_signature,
	[DKIM_PARAM_SIGNALG] = rspamd_dkim_parse_signalg,
	[DKIM_PARAM_DOMAIN] = rspamd_dkim_parse_domain,
	[DKIM_PARAM_CANONALG] = rspamd_dkim_parse_canonalg,
	[DKIM_PARAM_QUERYMETHOD] = rspamd_dkim_parse_ignore,
	[DKIM_PARAM_SELECTOR] = rspamd_dkim_parse_selector,
	[DKIM_PARAM_HDRLIST] = rspamd_dkim_parse_hdrlist,
	[DKIM_PARAM_VERSION] = rspamd_dkim_parse_version,
	[DKIM_PARAM_IDENTITY] = rspamd_dkim_parse_ignore,
	[DKIM_PARAM_TIMESTAMP] = rspamd_dkim_parse_timestamp,
	[DKIM_PARAM_EXPIRATION] = rspamd_dkim_parse_expiration,
	[DKIM_PARAM_COPIEDHDRS] = rspamd_dkim_parse_ignore,
	[DKIM_PARAM_BODYHASH] = rspamd_dkim_parse_bodyhash,
	[DKIM_PARAM_BODYLENGTH] = rspamd_dkim_parse_bodylength
};

#define DKIM_ERROR dkim_error_quark ()
GQuark
dkim_error_quark (void)
{
	return g_quark_from_static_string ("dkim-error-quark");
}

/* Parsers implementation */
static gboolean
rspamd_dkim_parse_signature (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	ctx->b = memory_pool_alloc (ctx->pool, len + 1);
	rspamd_strlcpy (ctx->b, param, len + 1);
	g_base64_decode_inplace (ctx->b, &len);
	ctx->blen = len;
	return TRUE;
}

static gboolean
rspamd_dkim_parse_signalg (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	if (len == 8) {
		if (memcmp (param, "rsa-sha1", len) == 0) {
			ctx->sig_alg = DKIM_SIGN_RSASHA1;
			return TRUE;
		}
	}
	else if (len == 10) {
		if (memcmp (param, "rsa-sha256", len) == 0) {
			ctx->sig_alg = DKIM_SIGN_RSASHA256;
			return TRUE;
		}
	}

	g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_INVALID_A, "invalid dkim sign algorithm");
	return FALSE;
}

static gboolean
rspamd_dkim_parse_domain (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	ctx->domain = memory_pool_alloc (ctx->pool, len + 1);
	rspamd_strlcpy (ctx->domain, param, len + 1);
	return TRUE;
}

static gboolean
rspamd_dkim_parse_canonalg (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	const gchar						*p, *slash = NULL, *end = param + len;
	gsize							 sl = 0;

	p = param;
	while (p != end) {
		if (*p == '/') {
			slash = p;
			break;
		}
		p ++;
		sl ++;
	}

	if (slash == NULL) {
		/* Only check header */
		if (len == 6 && memcmp (param, "simple", len) == 0) {
			ctx->header_canon_type = DKIM_CANON_SIMPLE;
			return TRUE;
		}
		else if (len == 7 && memcmp (param, "relaxed", len) == 0) {
			ctx->header_canon_type = DKIM_CANON_RELAXED;
			return TRUE;
		}
	}
	else {
		/* First check header */
		if (sl == 6 && memcmp (param, "simple", sl) == 0) {
			ctx->header_canon_type = DKIM_CANON_SIMPLE;
		}
		else if (sl == 7 && memcmp (param, "relaxed", sl) == 0) {
			ctx->header_canon_type = DKIM_CANON_RELAXED;
		}
		else {
			goto err;
		}
		/* Check body */
		len -= sl + 1;
		slash ++;
		if (len == 6 && memcmp (slash, "simple", len) == 0) {
			ctx->body_canon_type = DKIM_CANON_SIMPLE;
			return TRUE;
		}
		else if (len == 7 && memcmp (slash, "relaxed", len) == 0) {
			ctx->body_canon_type = DKIM_CANON_RELAXED;
			return TRUE;
		}
	}

err:
	g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_INVALID_A, "invalid dkim canonization algorithm");
	return FALSE;
}

static gboolean
rspamd_dkim_parse_ignore (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	/* Just ignore unused params */
	return TRUE;
}

static gboolean
rspamd_dkim_parse_selector (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	ctx->selector = memory_pool_alloc (ctx->pool, len + 1);
	rspamd_strlcpy (ctx->selector, param, len + 1);
	return TRUE;
}

static gboolean
rspamd_dkim_parse_hdrlist (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	const gchar						*c, *p, *end = param + len;
	gchar							*h;
	gboolean						 from_found = FALSE;

	c = param;
	p = param;
	while (p <= end) {
		if ((*p == ':' || p == end) && p - c > 0) {
			/* Insert new header to the list */
			h = memory_pool_alloc (ctx->pool, p - c + 1);
			rspamd_strlcpy (h, c, p - c + 1);
			g_strstrip (h);
			/* Check mandatory from */
			if (!from_found && g_ascii_strcasecmp (h, "from") == 0) {
				from_found = TRUE;
			}
			ctx->hlist = g_list_prepend (ctx->hlist, h);
			c = p + 1;
			p ++;
		}
		else {
			p ++;
		}
	}

	if (!ctx->hlist) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_INVALID_H, "invalid dkim header list");
		return FALSE;
	}
	else {
		if (!from_found) {
			g_list_free (ctx->hlist);
			g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_INVALID_H, "invalid dkim header list, from header is missing");
			return FALSE;
		}
		/* Reverse list */
		ctx->hlist = g_list_reverse (ctx->hlist);
		memory_pool_add_destructor (ctx->pool, (pool_destruct_func)g_list_free, ctx->hlist);
	}

	return TRUE;
}

static gboolean
rspamd_dkim_parse_version (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	if (len != 1 || *param != '1') {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_VERSION, "invalid dkim version");
		return FALSE;
	}

	ctx->ver = 1;
	return TRUE;
}

static gboolean
rspamd_dkim_parse_timestamp (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	gulong							 val;

	if (!rspamd_strtoul (param, len, &val)) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "invalid dkim timestamp");
		return FALSE;
	}
	ctx->timestamp = val;

	return TRUE;
}

static gboolean
rspamd_dkim_parse_expiration (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	gulong							 val;

	if (!rspamd_strtoul (param, len, &val)) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "invalid dkim expiration");
		return FALSE;
	}
	ctx->expiration = val;

	return TRUE;
}

static gboolean
rspamd_dkim_parse_bodyhash (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	ctx->bh = memory_pool_alloc (ctx->pool, len + 1);
	rspamd_strlcpy (ctx->bh, param, len + 1);
	g_base64_decode_inplace (ctx->bh, &len);
	ctx->bhlen = len;
	return TRUE;
}

static gboolean
rspamd_dkim_parse_bodylength (rspamd_dkim_context_t* ctx, const gchar *param, gsize len, GError **err)
{
	gulong							 val;

	if (!rspamd_strtoul (param, len, &val)) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_INVALID_L, "invalid dkim body length");
		return FALSE;
	}
	ctx->len = val;

	return TRUE;
}

/**
 * Create new dkim context from signature
 * @param sig message's signature
 * @param pool pool to allocate memory from
 * @param err pointer to error object
 * @return new context or NULL
 */
rspamd_dkim_context_t*
rspamd_create_dkim_context (const gchar *sig, memory_pool_t *pool, GError **err)
{
	const gchar						*p, *c, *tag, *end;
	gsize							 taglen;
	gint							 param = DKIM_PARAM_UNKNOWN;
	time_t							 now;
	rspamd_dkim_context_t			*new;
	enum {
		DKIM_STATE_TAG = 0,
		DKIM_STATE_AFTER_TAG,
		DKIM_STATE_VALUE,
		DKIM_STATE_SKIP_SPACES = 99,
		DKIM_STATE_ERROR = 100
	}								 state, next_state;


	new = memory_pool_alloc0 (pool, sizeof (rspamd_dkim_context_t));
	new->pool = pool;
	new->header_canon_type = DKIM_CANON_DEFAULT;
	new->body_canon_type = DKIM_CANON_DEFAULT;
	new->sig_alg = DKIM_SIGN_UNKNOWN;
	/* A simple state machine of parsing tags */
	state = DKIM_STATE_SKIP_SPACES;
	next_state = DKIM_STATE_TAG;
	taglen = 0;
	p = sig;
	c = sig;
	end = p + strlen (p);
	while (p <= end) {
		switch (state) {
		case DKIM_STATE_TAG:
			if (g_ascii_isspace (*p)) {
				taglen = p - c;
				while (*p && g_ascii_isspace (*p)) {
					/* Skip spaces before '=' sign */
					p ++;
				}
				if (*p != '=') {
					g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "invalid dkim param");
					state = DKIM_STATE_ERROR;
				}
				else {
					state = DKIM_STATE_SKIP_SPACES;
					next_state = DKIM_STATE_AFTER_TAG;
					param = DKIM_PARAM_UNKNOWN;
					p ++;
					tag = c;
				}
			}
			else if (*p == '=') {
				state = DKIM_STATE_SKIP_SPACES;
				next_state = DKIM_STATE_AFTER_TAG;
				param = DKIM_PARAM_UNKNOWN;
				p ++;
				tag = c;
			}
			else {
				taglen ++;
				p ++;
			}
			break;
		case DKIM_STATE_AFTER_TAG:
			/* We got tag at tag and len at taglen */
			switch (taglen) {
			case 0:
				g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "zero length dkim param");
				state = DKIM_STATE_ERROR;
				break;
			case 1:
				/* Simple tags */
				switch (*tag) {
				case 'v':
					param = DKIM_PARAM_VERSION;
					break;
				case 'a':
					param = DKIM_PARAM_SIGNALG;
					break;
				case 'b':
					param = DKIM_PARAM_SIGNATURE;
					break;
				case 'c':
					param = DKIM_PARAM_CANONALG;
					break;
				case 'd':
					param = DKIM_PARAM_DOMAIN;
					break;
				case 'h':
					param = DKIM_PARAM_HDRLIST;
					break;
				case 'i':
					param = DKIM_PARAM_IDENTITY;
					break;
				case 'l':
					param = DKIM_PARAM_BODYLENGTH;
					break;
				case 'q':
					param = DKIM_PARAM_QUERYMETHOD;
					break;
				case 's':
					param = DKIM_PARAM_SELECTOR;
					break;
				case 't':
					param = DKIM_PARAM_TIMESTAMP;
					break;
				case 'x':
					param = DKIM_PARAM_EXPIRATION;
					break;
				case 'z':
					param = DKIM_PARAM_COPIEDHDRS;
					break;
				default:
					g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "invalid dkim param: %c", *tag);
					state = DKIM_STATE_ERROR;
					break;
				}
				break;
			case 2:
				if (tag[0] == 'b' && tag[1] == 'h') {
					param = DKIM_PARAM_BODYHASH;
				}
				else {
					g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "invalid dkim param: %c%c", tag[0], tag[1]);
					state = DKIM_STATE_ERROR;
				}
				break;
			default:
				g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_UNKNOWN, "invalid dkim param lenght: %zd", taglen);
				state = DKIM_STATE_ERROR;
				break;
			}
			if (state != DKIM_STATE_ERROR) {
				/* Skip spaces */
				state = DKIM_STATE_SKIP_SPACES;
				next_state = DKIM_STATE_VALUE;
			}
			break;
		case DKIM_STATE_VALUE:
			if (*p == ';') {
				if (param == DKIM_PARAM_UNKNOWN || !parser_funcs[param](new, c, p - c, err)) {
					state = DKIM_STATE_ERROR;
				}
				else {
					state = DKIM_STATE_SKIP_SPACES;
					next_state = DKIM_STATE_TAG;
					p ++;
					taglen = 0;
				}
			}
			else if (p == end) {
				if (param == DKIM_PARAM_UNKNOWN || !parser_funcs[param](new, c, p - c + 1, err)) {
					state = DKIM_STATE_ERROR;
				}
				else {
					/* Finish processing */
					p ++;
				}
			}
			else {
				p ++;
			}
			break;
		case DKIM_STATE_SKIP_SPACES:
			if (g_ascii_isspace (*p)) {
				p ++;
			}
			else {
				c = p;
				state = next_state;
			}
			break;
		case DKIM_STATE_ERROR:
			if (err) {
				msg_info ("dkim parse failed: %s", (*err)->message);
				return NULL;
			}
			else {
				msg_info ("dkim parse failed: unknown error");
				return NULL;
			}
			break;
		}
	}

	/* Now check validity of signature */
	if (new->b == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_B, "b parameter missing");
		return NULL;
	}
	if (new->bh == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_BH, "bh parameter missing");
		return NULL;
	}
	if (new->domain == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_D, "domain parameter missing");
		return NULL;
	}
	if (new->selector == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_S, "selector parameter missing");
		return NULL;
	}
	if (new->ver == 0) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_V, "v parameter missing");
		return NULL;
	}
	if (new->hlist == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_H, "h parameter missing");
		return NULL;
	}
	if (new->sig_alg == DKIM_SIGN_UNKNOWN) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EMPTY_S, "s parameter missing");
		return NULL;
	}
	if (new->sig_alg == DKIM_SIGN_RSASHA1) {
		/* Check bh length */
		if (new->bhlen != g_checksum_type_get_length (G_CHECKSUM_SHA1)) {
			g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_BADSIG, "signature has incorrect length: %ud", new->bhlen);
			return NULL;
		}

	}
	else if (new->sig_alg == DKIM_SIGN_RSASHA256) {
		if (new->bhlen != g_checksum_type_get_length (G_CHECKSUM_SHA256)) {
			g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_BADSIG, "signature has incorrect length: %ud", new->bhlen);
			return NULL;
		}
	}
	/* Check expiration */
	now = time (NULL);
	if (new->timestamp && new->timestamp > now) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_FUTURE, "signature was made in future, ignoring");
		return NULL;
	}
	if (new->expiration && new->expiration < now) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_EXPIRED, "signature has expired");
		return NULL;
	}

	/* Now create dns key to request further */
	taglen = strlen (new->domain) + strlen (new->selector) + sizeof (DKIM_DNSKEYNAME) + 2;
	new->dns_key = memory_pool_alloc (new->pool, taglen);
	rspamd_snprintf (new->dns_key, taglen, "%s.%s.%s", new->selector, DKIM_DNSKEYNAME, new->domain);

	/* Create checksums for further operations */
	if (new->sig_alg == DKIM_SIGN_RSASHA1) {
		new->body_hash = g_checksum_new (G_CHECKSUM_SHA1);
		new->headers_hash = g_checksum_new (G_CHECKSUM_SHA1);
	}
	else if (new->sig_alg == DKIM_SIGN_RSASHA256) {
		new->body_hash = g_checksum_new (G_CHECKSUM_SHA256);
		new->headers_hash = g_checksum_new (G_CHECKSUM_SHA256);
	}
	else {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_BADSIG, "signature has unsupported signature algorithm");
		return NULL;
	}

	memory_pool_add_destructor (new->pool, (pool_destruct_func)g_checksum_free, new->body_hash);
	memory_pool_add_destructor (new->pool, (pool_destruct_func)g_checksum_free, new->headers_hash);

	return new;
}

struct rspamd_dkim_key_cbdata {
	rspamd_dkim_context_t *ctx;
	dkim_key_handler_f handler;
	gpointer ud;
};

static rspamd_dkim_key_t*
rspamd_dkim_make_key (const gchar *keydata, guint keylen, GError **err)
{
	rspamd_dkim_key_t							*key = NULL;

	key = g_slice_alloc0 (sizeof (rspamd_dkim_key_t));
	key->keydata = g_slice_alloc (keylen + 1);
	rspamd_strlcpy (key->keydata, keydata, keylen + 1);
	key->keylen = keylen + 1;
	key->decoded_len = keylen + 1;
	g_base64_decode_inplace (key->keydata, &key->decoded_len);
#ifdef HAVE_OPENSSL
	key->key_bio = BIO_new_mem_buf (key->keydata, key->decoded_len);
	if (key->key_bio == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_KEYFAIL, "cannot make ssl bio from key");
		rspamd_dkim_key_free (key);
		return NULL;
	}

	key->key_evp = d2i_PUBKEY_bio (key->key_bio, NULL);
	if (key->key_evp == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_KEYFAIL, "cannot extract pubkey from bio");
		rspamd_dkim_key_free (key);
		return NULL;
	}

	key->key_rsa = EVP_PKEY_get1_RSA (key->key_evp);
	if (key->key_rsa == NULL) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_KEYFAIL, "cannot extract rsa key from evp key");
		rspamd_dkim_key_free (key);
		return NULL;
	}

#endif

	return key;
}

/**
 * Free DKIM key
 * @param key
 */
void
rspamd_dkim_key_free (rspamd_dkim_key_t *key)
{
#ifdef HAVE_OPENSSL
	if (key->key_rsa) {
		RSA_free (key->key_rsa);
	}
	if (key->key_bio) {
		BIO_free (key->key_bio);
	}
#endif
	g_slice_free1 (key->keylen, key->keydata);
	g_slice_free1 (sizeof (rspamd_dkim_key_t), key);
}

static rspamd_dkim_key_t*
rspamd_dkim_parse_key (const gchar *txt, gsize *keylen, GError **err)
{
	const gchar									*c, *p, *end;
	gint										 state = 0;
	gsize										 len;

	c = txt;
	p = txt;
	end = txt + strlen (txt);

	while (p <= end) {
		switch (state) {
		case 0:
			if (p != end && p[0] == 'p' && p[1] == '=') {
				/* We got something like public key */
				c = p + 2;
				p = c;
				state = 1;
			}
			else {
				/* Ignore everything */
				p ++;
			}
			break;
		case 1:
			/* State when we got p= and looking for some public key */
			if ((*p == ';' || p == end) && p > c) {
				len = p - c;
				return rspamd_dkim_make_key (c, len, err);
			}
			else {
				p ++;
			}
			break;
		}
	}

	if (p - c == 0) {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_KEYREVOKED, "key was revoked");
	}
	else {
		g_set_error (err, DKIM_ERROR, DKIM_SIGERROR_KEYFAIL, "key was not found");
	}

	return NULL;
}

/* Get TXT request data and parse it */
static void
rspamd_dkim_dns_cb (struct rspamd_dns_reply *reply, gpointer arg)
{
	struct rspamd_dkim_key_cbdata				*cbdata = arg;
	rspamd_dkim_key_t							*key;
	GError										*err = NULL;
	GList										*cur;
	union rspamd_reply_element					*elt;
	gsize										 keylen = 0;

	if (reply->code != DNS_RC_NOERROR) {
		g_set_error (&err, DKIM_ERROR, DKIM_SIGERROR_NOKEY, "dns request to %s failed: %s", cbdata->ctx->dns_key,
				dns_strerror (reply->code));
		cbdata->handler (NULL, 0, cbdata->ctx, cbdata->ud, err);
	}
	else {
		cur = reply->elements;
		while (cur) {
			elt = cur->data;
			key = rspamd_dkim_parse_key (elt->txt.data, &keylen, &err);
			if (key) {
				break;
			}
			cur = g_list_next (cur);
		}
		if (key != NULL && err != NULL) {
			/* Free error as it is insignificant */
			g_error_free (err);
			err = NULL;
		}
		cbdata->handler (key, keylen, cbdata->ctx, cbdata->ud, err);
	}
}

/**
 * Make DNS request for specified context and obtain and parse key
 * @param ctx dkim context from signature
 * @param resolver dns resolver object
 * @param s async session to make request
 * @return
 */
gboolean
rspamd_get_dkim_key (rspamd_dkim_context_t *ctx, struct rspamd_dns_resolver *resolver,
		struct rspamd_async_session *s, dkim_key_handler_f handler, gpointer ud)
{
	struct rspamd_dkim_key_cbdata				*cbdata;

	g_return_val_if_fail (ctx != NULL, FALSE);
	g_return_val_if_fail (ctx->dns_key != NULL, FALSE);

	cbdata = memory_pool_alloc (ctx->pool, sizeof (struct rspamd_dkim_key_cbdata));
	cbdata->ctx = ctx;
	cbdata->handler = handler;
	cbdata->ud = ud;

	return make_dns_request (resolver, s, ctx->pool, rspamd_dkim_dns_cb, cbdata, DNS_REQUEST_TXT, ctx->dns_key);
}

static gboolean
rspamd_dkim_relaxed_body_step (GChecksum *ck, const gchar **start, guint remain)
{
	const gchar									*h;
	static gchar								 buf[BUFSIZ];
	gchar										*t;
	guint										 len, inlen;
	gboolean									 got_sp, finished = FALSE;

	if (remain > sizeof (buf)) {
		len = sizeof (buf);
	}
	else {
		len = remain;
		finished = TRUE;
	}
	inlen = len;
	h = *start;
	t = &buf[0];
	got_sp = FALSE;

	while (len && inlen) {
		if (*h == '\r' || *h == '\n') {
			/* Ignore spaces at the end of line */
			if (got_sp) {
				got_sp = FALSE;
				t --;
			}
		}
		else if (g_ascii_isspace (*h)) {
			if (got_sp) {
				/* Ignore multiply spaces */
				h ++;
				len --;
				continue;
			}
			else {
				*t++ = ' ';
				h ++;
				inlen --;
				len --;
				got_sp = TRUE;
				continue;
			}
		}
		else {
			got_sp = FALSE;
		}
		*t++ = *h++;
		inlen --;
		len --;
	}

	*start = h;

	/* Maybe extremely slow
	 * msg_debug ("update signature with buffer: %*s", t - buf, buf);
	 */
	g_checksum_update (ck, buf, t - buf);

	return !finished;
}

static gboolean
rspamd_dkim_canonize_body (rspamd_dkim_context_t *ctx, const gchar *start, const gchar *end)
{
	if (start == NULL) {
		/* Empty body */
		g_checksum_update (ctx->body_hash, CRLF, sizeof (CRLF) - 1);
	}
	else {
		end --;
		while (end > start + 2) {
			if (*end == '\n' && *(end - 1) == '\r' && *(end - 2) == '\n') {
				end -= 2;
			}
			else {
				break;
			}
		}
		if (end == start || end == start + 2) {
			/* Empty body */
			g_checksum_update (ctx->body_hash, CRLF, sizeof (CRLF) - 1);
		}
		else {
			if (ctx->body_canon_type == DKIM_CANON_SIMPLE) {
				/* Simple canonization */
				g_checksum_update (ctx->body_hash, start, end - start + 1);
			}
			else {
				while (rspamd_dkim_relaxed_body_step (ctx->body_hash, &start, end - start + 1));
			}
			if (*end != '\n' || *(end - 1) != '\r') {
				msg_debug ("append CRLF");
				g_checksum_update (ctx->body_hash, CRLF, sizeof (CRLF) - 1);
			}
		}
		return TRUE;
	}

	/* TODO: Implement relaxed algorithm */
	return FALSE;
}

/* Update hash by signature value (ignoring b= tag) */
static void
rspamd_dkim_signature_update (rspamd_dkim_context_t *ctx, const gchar *begin, guint len)
{
	const gchar									*p, *c, *end;
	gboolean									 tag, skip;

	end = begin + len;
	p = begin;
	c = begin;
	tag = TRUE;
	skip = FALSE;

	while (p < end) {
		if (tag && p[0] == 'b' && p[1] == '=') {
			/* Add to signature */
			msg_debug ("initial update hash with signature part: %*s", p - c + 2, c);
			g_checksum_update (ctx->headers_hash, c, p - c + 2);
			skip = TRUE;
		}
		else if (skip && (*p == ';' || p == end - 1)) {
			skip = FALSE;
			c = p;
		}
		else if (!tag && *p == ';') {
			tag = TRUE;
		}
		else if (tag && *p == '=') {
			tag = FALSE;
		}
		p ++;
	}

	p --;
	/* Skip \r\n at the end */
	while ((*p == '\r' || *p == '\n') && p >= c) {
		p --;
	}

	if (p - c + 1 > 0) {
		msg_debug ("final update hash with signature part: %*s", p - c + 1, c);
		g_checksum_update (ctx->headers_hash, c, p - c + 1);
	}
}

static gboolean
rspamd_dkim_canonize_header_relaxed (rspamd_dkim_context_t *ctx, const gchar *header, const gchar *header_name, gboolean is_sign)
{
	const gchar									*h;
	gchar										*t, *buf;
	guint										 inlen;
	gboolean									 got_sp, allocated = FALSE;

	inlen = strlen (header) + strlen (header_name) + sizeof (":" CRLF);
	if (inlen > BUFSIZ) {
		buf = g_malloc (inlen);
		allocated = TRUE;
	}
	else {
		/* Faster */
		buf = g_alloca (inlen);
	}

	/* Name part */
	t = buf;
	h = header_name;
	while (*h) {
		*t ++ = g_ascii_tolower (*h++);
	}
	*t++ = ':';

	/* Value part */
	h = header;
	got_sp = FALSE;

	while (*h) {
		if (g_ascii_isspace (*h)) {
			if (got_sp) {
				h ++;
				continue;
			}
			else {
				got_sp = TRUE;
				*t ++ = ' ';
				h ++;
				continue;
			}
		}
		else {
			got_sp = FALSE;
		}
		*t ++ = *h ++;
	}
	if (g_ascii_isspace (*(t - 1))) {
		t --;
	}
	*t++ = '\r';
	*t++ = '\n';
	*t = '\0';

	if (!is_sign) {
		msg_debug ("update signature with header: %s", buf);
		g_checksum_update (ctx->headers_hash, buf, t - buf);
	}
	else {
		rspamd_dkim_signature_update (ctx, buf, t - buf);
	}

	if (allocated) {
		g_free (buf);
	}

	return TRUE;
}

static gboolean
rspamd_dkim_canonize_header_simple (rspamd_dkim_context_t *ctx, const gchar *headers, const gchar *header_name, gboolean is_sign)
{
	const gchar									*p, *c;
	gint										 state = 0, hlen;
	gboolean									 found = FALSE;

	/* This process is very similar to raw headers processing */
	p = headers;
	c = p;
	hlen = strlen (header_name);

	while (*p) {
		switch (state) {
		case 0:
			/* Compare state */
			if (*p == ':') {
				/* Compare header's name with desired one */
				if (p - c == hlen) {
					if (g_ascii_strncasecmp (c, header_name, hlen) == 0) {
						/* Get value */
						state = 2;
					}
					else {
						/* Skip the whole header */
						state = 1;
					}
				}
				else {
					/* Skip the whole header */
					state = 1;
				}
			}
			p ++;
			break;
		case 1:
			/* Skip header state */
			if (*p == '\n' && !g_ascii_isspace (p[1])) {
				/* Header is skipped */
				state = 0;
				c = p + 1;
			}
			p ++;
			break;
		case 2:
			/* c contains the beginning of header */
			if (*p == '\n' && (!g_ascii_isspace (p[1]) || p[1] == '\0')) {
				if (!is_sign) {
					msg_debug ("update signature with header: %*s", p - c + 1, c);
					g_checksum_update (ctx->headers_hash, c, p - c + 1);
				}
				else {
					rspamd_dkim_signature_update (ctx, c, p - c + 1);
				}
				c = p + 1;
				state = 0;
				found = TRUE;
			}
			p ++;
			break;
		}
	}

	return found;
}

static gboolean
rspamd_dkim_canonize_header (rspamd_dkim_context_t *ctx, struct worker_task *task, const gchar *header_name, gboolean is_sig)
{
	struct raw_header 							*rh;

	if (ctx->header_canon_type == DKIM_CANON_SIMPLE) {
		return rspamd_dkim_canonize_header_simple (ctx, task->raw_headers_str, header_name, is_sig);
	}
	else {
		rh = g_hash_table_lookup (task->raw_headers, header_name);
		if (rh) {
			while (rh) {
				if (! rspamd_dkim_canonize_header_relaxed (ctx, rh->value, header_name, is_sig)) {
					return FALSE;
				}
				if (!is_sig) {
					rh = rh->next;
				}
				else {
					rh = NULL;
				}
			}
			return TRUE;
		}
	}

	/* TODO: Implement relaxed algorithm */
	return FALSE;
}

/**
 * Check task for dkim context using dkim key
 * @param ctx dkim verify context
 * @param key dkim key (from cache or from dns request)
 * @param task task to check
 * @return
 */
gint
rspamd_dkim_check (rspamd_dkim_context_t *ctx, rspamd_dkim_key_t *key, struct worker_task *task)
{
	const gchar									*p, *headers_end = NULL, *end, *body_end;
	gboolean									 got_cr = FALSE, got_crlf = FALSE, got_lf = FALSE;
	GList										*cur;
	gchar										*digest;
	gsize										 dlen;
	gint										 res = DKIM_CONTINUE;
#ifdef HAVE_OPENSSL
	gint										 nid;
#endif

	g_return_val_if_fail (ctx != NULL, DKIM_ERROR);
	g_return_val_if_fail (key != NULL, DKIM_ERROR);
	g_return_val_if_fail (task->msg != NULL, DKIM_ERROR);

	/* First of all find place of body */
	p = task->msg->begin;

	end = task->msg->begin + task->msg->len;

	while (p <= end) {
		/* Search for \r\n\r\n at the end of headers */
		if (*p == '\n') {
			if (got_cr && *(p - 1) == '\r') {
				if (got_crlf) {
					/* \r\n\r\n */
					headers_end = p + 1;
					break;
				}
				else {
					/* Set got crlf flag */
					got_crlf = TRUE;
					got_cr = FALSE;
				}
			}
			else if (got_cr && *(p - 1) != '\r') {
				/* We got CR somewhere but not right before */
				got_cr = FALSE;
				if (*(p - 1) == '\n') {
					/* \r\n\n case */
					headers_end = p + 1;
					break;
				}
				got_lf = TRUE;
			}
			else if (got_lf && *(p - 1) == '\n') {
				/* \n\n case */
				headers_end = p + 1;
				break;
			}
			else {
				got_lf = TRUE;
			}
		}
		else if (*p == '\r') {
			if (got_cr && *(p - 1) == '\r') {
				/* \r\r case */
				headers_end = p + 1;
				break;
			}
			else if (got_lf && *(p - 1) == '\n') {
				/* \n\r case */
				headers_end = p + 1;
				break;
			}
			else {
				got_cr = TRUE;
			}
		}
		else {
			got_cr = FALSE;
			got_crlf = FALSE;
		}
		p ++;
	}

	/* Start canonization of body part */
	if (headers_end) {
		if (ctx->len == 0 || (gint)ctx->len > end - headers_end) {
			body_end = end;
		}
		else {
			/* Strip message */
			body_end = headers_end + ctx->len;
		}
	}
	else {
		body_end = end;
	}
	if (!rspamd_dkim_canonize_body (ctx, headers_end, body_end)) {
		return DKIM_RECORD_ERROR;
	}
	/* Now canonize headers */
	cur = ctx->hlist;
	while (cur) {
		if (!rspamd_dkim_canonize_header (ctx, task, cur->data, FALSE)) {
			msg_debug ("cannot find header %s for canonization", cur->data);
			return DKIM_RECORD_ERROR;
		}
		cur = g_list_next (cur);
	}

	/* Canonize dkim signature */
	rspamd_dkim_canonize_header (ctx, task, DKIM_SIGNHEADER, TRUE);

	dlen = ctx->bhlen;
	digest = g_alloca (dlen);
	g_checksum_get_digest (ctx->body_hash, digest, &dlen);

	/* Check bh field */
	if (memcmp (ctx->bh, digest, dlen) != 0) {
		msg_debug ("bh value missmatch");
		return DKIM_REJECT;
	}

	g_checksum_get_digest (ctx->headers_hash, digest, &dlen);
#ifdef HAVE_OPENSSL
	/* Check headers signature */

	if (ctx->sig_alg == DKIM_SIGN_RSASHA1) {
		nid = NID_sha1;
	}
	else if (ctx->sig_alg == DKIM_SIGN_RSASHA256) {
		nid = NID_sha256;
	}
	else {
		/* Not reached */
		nid = NID_sha1;
	}

	if (RSA_verify (nid, digest, dlen, ctx->b, ctx->blen, key->key_rsa) != 1) {
		msg_debug ("rsa verify failed");
		res = DKIM_REJECT;
	}
#endif
	return res;
}
