/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file tls/cache.c
 * @brief Functions to support TLS session resumption
 *
 * @copyright 2015-2016 The FreeRADIUS server project
 */
RCSID("$Id$")
USES_APPLE_DEPRECATED_API	/* OpenSSL API has been deprecated by Apple */

#ifdef WITH_TLS
#define LOG_PREFIX "tls - "

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/process.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/rad_assert.h>

#define MAX_CACHE_ID_SIZE (256)

/** Add attributes identifying the TLS session to be acted upon, and the action to be performed
 *
 * Adds the following attributes to the request:
 *
 *	- &request:TLS-Session-Id
 *	- &control:TLS-Session-Cache-Action
 *
 * Session identity will contain the binary session key used to create, retrieve
 * and delete cache entries related to the SSL session.
 *
 * Session-Cache-Action will contain the action to be performed.  This is then
 * utilised by unlang policy (in a virtual server called with these attributes)
 * to perform different actions.
 *
 * @todo Add attribute representing session validity period.
 * @todo Move adding TLS-Session-Cache-Action to tls_cache_process and remove it again after calling
 *	the virtual server.
 *
 * @param[in] request The current request.
 * @param[in] key Identifier for the session.
 * @param[in] key_len Length of the key.
 * @param[in] action being performed (written to &control:TLS-Session-Cache-Action).
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int tls_cache_attrs(REQUEST *request,
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
			   uint8_t const *key,
#else
			   uint8_t *key,
#endif
			   size_t key_len, tls_cache_action_t action)
{
	VALUE_PAIR *vp;

	fr_pair_delete_by_num(&request->packet->vps, 0, PW_TLS_SESSION_ID, TAG_ANY);

	RDEBUG2("Setting TLS cache control attributes");
	vp = fr_pair_afrom_num(request->packet, 0, PW_TLS_SESSION_ID);
	if (!vp) return -1;

	fr_pair_value_memcpy(vp, key, key_len);
	fr_pair_add(&request->packet->vps, vp);
	RINDENT();
	rdebug_pair(L_DBG_LVL_2, request, vp, NULL);
	REXDENT();

	vp = fr_pair_afrom_num(request, 0, PW_TLS_CACHE_ACTION);
	if (!vp) return -1;

	vp->vp_integer = action;
	fr_pair_add(&request->control, vp);
	RINDENT();
	rdebug_pair(L_DBG_LVL_2, request, vp, "&control:");
	REXDENT();

	return 0;
}

/** Execute the virtual server configured to perform cache actions
 *
 * @param[in] request The current request.
 * @param[in] virtual_server Name of the virtual server to execute.
 * @param[in] autz_type The authorize sub-section to execute.
 * @return the rcode from the virtual server.
 */
int tls_cache_process(REQUEST *request, char const *virtual_server, int autz_type)
{
	rlm_rcode_t rcode;
	VALUE_PAIR *vp;

	/*
	 *	Save the current status of the request.
	 */
	char const *server = request->server;
	char const *module = request->module;
	char const *component = request->component;

	/*
	 *	Indicate what action we're performing
	 */
	vp = fr_pair_afrom_num(request, 0, PW_TLS_CACHE_ACTION);
	if (!vp) return -1;

	vp->vp_integer = autz_type;

	fr_pair_add(&request->control, vp);
	RINDENT();
	rdebug_pair(L_DBG_LVL_2, request, vp, "&control:");
	REXDENT();

	/*
	 *	Run it through the appropriate virtual server.
	 */
	request->server = virtual_server;
	request->module = NULL;

	rcode = process_authorize(autz_type + 1000, request);

	/*
	 *	Restore the original status of the request.
	 */
	request->server = server;
	request->module = module;
	request->component = component;

	fr_pair_delete_by_num(&request->control, 0, PW_TLS_CACHE_ACTION, TAG_ANY);

	return rcode;
}

/** Retrieve session ID (in binary form) from the session
 *
 * @param[out] out Where to write the session id.
 * @param[in] outlen length of output buffer.
 * @param[in] tls_session to get Id for.
 * @return
 *	- -1 on failure (buffer too small).
 *	- >0 on success, the number of bytes written.
 */
static ssize_t tls_cache_id(uint8_t *out, size_t outlen, SSL_SESSION *tls_session)
{
#if OPENSSL_VERSION_NUMBER < 0x10001000L
	size_t len;

	len = tls_session->session_id_length;
	if (len > outlen) return -1;
	memcpy(out, tls_session->session_id, len);

	return (ssize_t)len;
#else
	unsigned int len;
	uint8_t const *p;

	p = SSL_SESSION_get_id(tls_session, &len);
	if (len > outlen) return -1;
	memcpy(out, p, len);

	return (ssize_t)len;
#endif
}

/** Write a newly created session to the cache
 *
 * @param[in] ssl session state.
 * @param[in] sess to serialise and write to the cache.
 * @return 0.  What we return is not used by OpenSSL to indicate success or failure,
 *	but to indicate whether it should free its copy of the session data.
 */
static int tls_cache_write(SSL *ssl, SSL_SESSION *sess)
{
	fr_tls_conf_t	*conf;
	REQUEST			*request;
	size_t			len, rcode;
	ssize_t			slen;
	uint8_t			*p, *data = NULL;
	VALUE_PAIR		*vp;
	uint8_t			buffer[MAX_CACHE_ID_SIZE];

	request = SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_REQUEST);
	conf = SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_CONF);

	slen = tls_cache_id(buffer, sizeof(buffer), sess);
	if (slen < 0) {
		REDEBUG("Session ID buffer to small");
		return 0;
	}
	if (tls_cache_attrs(request, (uint8_t *) buffer, (size_t)slen, CACHE_ACTION_SESSION_WRITE) < 0) {
		RWDEBUG("Failed adding session key to the request");
		return 0;
	}

	/* find out what length data we need */
	len = i2d_SSL_SESSION(sess, NULL);
	if (len < 1) {
		/* something went wrong */
		RWDEBUG("Session serialisation failed, couldn't determine required buffer length");
		return 0;
	}

	/* alloc and convert to ASN.1 */
	data = talloc_array(NULL, uint8_t, len);
	if (!data) {
		RWDEBUG("Session serialisation failed, couldn't allocate buffer (%zd bytes)", len);
		return 0;
	}

	/* openssl mutates &p */
	p = data;
	rcode = i2d_SSL_SESSION(sess, &p);
	if (rcode != len) {
		RWDEBUG("Session serialisation failed");
		goto error;
	}

	/*
	 *	Put the SSL data into an attribute.
	 */
	vp = fr_pair_afrom_num(request->state_ctx, 0, PW_TLS_SESSION_DATA);
	if (!vp) goto error;

	fr_pair_value_memsteal(vp, data);
	RINDENT();
	rdebug_pair(L_DBG_LVL_2, request, vp, "&session-state:");
	REXDENT();
	fr_pair_add(&request->state, vp);
	data = NULL;

	/*
	 *	Call the virtual server to write the session
	 */
	switch (tls_cache_process(request, conf->session_cache_server, CACHE_ACTION_SESSION_WRITE)) {
	case RLM_MODULE_OK:
	case RLM_MODULE_UPDATED:
		break;

	default:
		RWDEBUG("Failed storing session data");
		break;
	}

	/*
	 *	Ensure that the session data can't be used by anyone else.
	 */
	fr_pair_delete_by_num(&request->state, 0, PW_TLS_SESSION_DATA, TAG_ANY);

error:
	if (data) talloc_free(data);

	return 0;
}

/** Read session data from the cache
 *
 * @param[in] ssl session state.
 * @param[in] key to retrieve session data for.
 * @param[in] key_len The length of the key.
 * @param[out] copy Indicates whether OpenSSL should increment the reference
 *	count on SSL_SESSION to prevent it being automatically freed.  We always
 *	set this to 0.
 * @return
 *	- Deserialised session data on success.
 *	- NULL on error.
 */
static SSL_SESSION *tls_cache_read(SSL *ssl,
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
				   unsigned char const *key,
#else
				   unsigned char *key,
#endif
				   int key_len, int *copy)
{
	fr_tls_conf_t		*conf;
	REQUEST			*request;
	unsigned char const	**p;
	uint8_t const		*q;
	VALUE_PAIR		*vp;
	SSL_SESSION		*sess;

	request = SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_REQUEST);
	conf = SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_CONF);

	if (tls_cache_attrs(request, key, key_len, CACHE_ACTION_SESSION_READ) < 0) {
		RWDEBUG("Failed adding session key to the request");
		return NULL;
	}

	*copy = 0;

	/*
	 *	Call the virtual server to read the session
	 */
	switch (tls_cache_process(request, conf->session_cache_server, CACHE_ACTION_SESSION_READ)) {
	case RLM_MODULE_OK:
	case RLM_MODULE_UPDATED:
		break;

	default:
		RWDEBUG("Failed acquiring session data");
		return NULL;
	}

	vp = fr_pair_find_by_num(request->state, 0, PW_TLS_SESSION_DATA, TAG_ANY);
	if (!vp) {
		RWDEBUG("No cached session found");
		return NULL;
	}

	q = vp->vp_octets;	/* openssl will mutate q, so we can't use vp_octets directly */
	p = (unsigned char const **)&q;

	sess = d2i_SSL_SESSION(NULL, p, vp->vp_length);
	if (!sess) {
		RWDEBUG("Failed loading persisted session: %s", ERR_error_string(ERR_get_error(), NULL));
		return NULL;
	}
	RDEBUG3("Read %zu bytes of session data.  Session deserialized successfully", vp->vp_length);

	/*
	 *	OpenSSL's API is very inconsistent.
	 *
	 *	We need to set external data here, so it can be
	 *	retrieved in tls_cache_delete.
	 *
	 *	ex_data is not serialised in i2d_SSL_SESSION
	 *	so we don't have to bother unsetting it.
	 */
	SSL_SESSION_set_ex_data(sess, FR_TLS_EX_INDEX_TLS_SESSION, SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_TLS_SESSION));

	/*
	 *	SSL_set_session increases the reference could
	 *	on the session, so when OpenSSL attempts to
	 *	free it, when setting our returned session
	 *	it becomes a noop.
	 *
	 *	Spent many hours trying to find a better place
	 *	to do validation than this, but it seems
	 *	like this is the only way.
	 */
	SSL_set_session(ssl, sess);
	if (tls_validate_client_cert_chain(ssl) != 1) {
		RWDEBUG("Validation failed, forcefully expiring resumed session");
		SSL_SESSION_set_timeout(sess, 0);
	}

	/*
	 *	Ensure that the session data can't be used by anyone else.
	 */
	fr_pair_delete_by_num(&request->state, 0, PW_TLS_SESSION_DATA, TAG_ANY);

	return sess;
}

/** Delete session data from the cache
 *
 * @param[in] ctx Current ssl context.
 * @param[in] sess to be deleted.
 */
static void tls_cache_delete(SSL_CTX *ctx, SSL_SESSION *sess)
{
	fr_tls_conf_t		*conf;
	tls_session_t		*tls_session;
	REQUEST			*request;
	uint8_t			buffer[MAX_CACHE_ID_SIZE];
	ssize_t			slen;

	conf = talloc_get_type_abort(SSL_CTX_get_app_data(ctx), fr_tls_conf_t);
	tls_session = talloc_get_type_abort(SSL_SESSION_get_ex_data(sess, FR_TLS_EX_INDEX_TLS_SESSION), tls_session_t);
	request = talloc_get_type_abort(SSL_get_ex_data(tls_session->ssl, FR_TLS_EX_INDEX_REQUEST), REQUEST);

	slen = tls_cache_id(buffer, sizeof(buffer), sess);
	if (slen < 0) {
		RWDEBUG("Session ID buffer too small");
	error:
		talloc_free(request);
		return;
	}

	if (tls_cache_attrs(request, buffer, (size_t)slen, CACHE_ACTION_SESSION_DELETE) < 0) {
		RWDEBUG("Failed adding session key to the request");
		goto error;
	}

	/*
	 *	Call the virtual server to delete the session
	 */
	switch (tls_cache_process(request, conf->session_cache_server, CACHE_ACTION_SESSION_DELETE)) {
	case RLM_MODULE_OK:
	case RLM_MODULE_UPDATED:
	case RLM_MODULE_NOTFOUND:
	case RLM_MODULE_NOOP:
		break;

	default:
		RWDEBUG("Failed deleting session data");
		goto error;
	}
}

/** Prevent a TLS session from being cached
 *
 * Usually called if the session has failed for some reason.
 *
 * @param session on which to prevent resumption.
 */
void tls_cache_deny(tls_session_t *session)
{
	/*
	 *	Even for 1.1.0 we don't know when this function
	 *	will be called, so better to remove the session
	 *	directly.
	 */
	SSL_CTX_remove_session(session->ctx, session->ssl_session);
}

/** Prevent a TLS session from being resumed in future
 *
 * @note In OpenSSL > 1.1.0 this should not be called directly, but passed as a callback to
 *	SSL_CTX_set_not_resumable_session_callback.
 *
 * @param ssl			The current OpenSSL session.
 * @param is_forward_secure	Whether the cipher is forward secure, pass -1 if unknown.
 * @return
 *	- 0 on success.
 *	- 1 if enabling session resumption was disabled for this session.
 */
int tls_cache_disable_cb(SSL *ssl,
#if OPENSSL_VERSION_NUMBER < 0x10100000L
			 UNUSED
#endif
			 int is_forward_secure)
{
	REQUEST			*request;

	tls_session_t		*session;
	VALUE_PAIR		*vp;

	session = talloc_get_type_abort(SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_TLS_SESSION), tls_session_t);
	request = talloc_get_type_abort(SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_REQUEST), REQUEST);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	{
		fr_tls_conf_t *conf;

		conf = talloc_get_type_abort(SSL_get_ex_data(ssl, FR_TLS_EX_INDEX_CONF), fr_tls_conf_t);
		if (conf->session_cache_require_extms && (SSL_get_extms_support(session->ssl) != 1)) {
			RDEBUG2("Client does not support the Extended Master Secret extension, "
				"disabling session resumption");
			goto disable;
		}

		if (conf->session_cache_require_pfs && !is_forward_secure) {
			RDEBUG2("Cipher suite is not forward secure, disabling session resumption");
			goto disable;
		}
	}
#endif

	/*
	 *	If there's no session resumption, delete the entry
	 *	from the cache.  This means either it's disabled
	 *	globally for this SSL context, OR we were told to
	 *	disable it for this user.
	 *
	 *	This also means you can't turn it on just for one
	 *	user.
	 */
	if (!session->allow_session_resumption) goto disable;

	vp = fr_pair_find_by_num(request->control, 0, PW_ALLOW_SESSION_RESUMPTION, TAG_ANY);
	if (vp && (vp->vp_integer == 0)) {
		RDEBUG2("&control:Allow-Session-Resumption == no, disabling session resumption");
	disable:
		SSL_CTX_remove_session(session->ctx, session->ssl_session);
		session->allow_session_resumption = false;
		return 1;
	}

	return 0;
}

/** Sets callbacks on a SSL_CTX to enable/disable session resumption
 *
 * @param ctx			to modify.
 * @param enabled		Whether session caching should be enabled.
 * @param session_context	under which cache entries should be stored.
 *				Prevents sessions being restored between
 *				different rlm_eap instances.
 * @param lifetime		The maximum period a cached session remains
 *				valid for.
 */
void tls_cache_init(SSL_CTX *ctx, bool enabled, char const *session_context, uint32_t lifetime)
{
	if (!enabled) {
		SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
		return;
	}

	rad_assert(session_context);

	SSL_CTX_sess_set_new_cb(ctx, tls_cache_write);
	SSL_CTX_sess_set_get_cb(ctx, tls_cache_read);
	SSL_CTX_sess_set_remove_cb(ctx, tls_cache_delete);
	SSL_CTX_set_quiet_shutdown(ctx, 1);

	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER | SSL_SESS_CACHE_NO_INTERNAL);
	SSL_CTX_set_timeout(ctx, lifetime);

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	SSL_CTX_set_not_resumable_session_callback(ctx, tls_cache_disable_cb);
#endif

	/*
	 *	This sets the context sessions can be resumed in.
	 *	This is to prevent sessions being created by one application
	 *	and used by another.  In our case it prevents sessions being
	 *	reused between modules, or TLS server components such as
	 *	RADSEC.
	 *
	 *	A context must always be set when doing session resumption
	 *	otherwise session resumption will fail.
	 */
	SSL_CTX_set_session_id_context(ctx,
				       (unsigned char const *) session_context,
				       (unsigned int) strlen(session_context));
}
#endif /* WITH_TLS */
