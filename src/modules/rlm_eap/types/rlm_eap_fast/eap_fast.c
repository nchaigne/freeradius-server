/*
 * eap_fast.c  contains the interfaces that are called from the main handler
 *
 * Version:     $Id$
 *
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
 *
 *   Copyright 2016 Alan DeKok <aland@freeradius.org>
 *   Copyright 2016 The FreeRADIUS server project
 */

RCSID("$Id$")

#include "eap_fast.h"
#include "eap_fast_crypto.h"
#include <freeradius-devel/sha1.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>

#define RANDFILL(x) do { rad_assert(sizeof(x) % sizeof(uint32_t) == 0); for (size_t i = 0; i < sizeof(x); i += sizeof(uint32_t)) *((uint32_t *)&x[i]) = fr_rand(); } while(0)

/*
 * Copyright (c) 2002-2016, Jouni Malinen <j@w1.fi> and contributors
 * All Rights Reserved.
 *
 * These programs are licensed under the BSD license (the one with
 * advertisement clause removed).
 *
 * this function shamelessly stolen from from hostap:src/crypto/tls_openssl.c
 */
static int openssl_get_keyblock_size(REQUEST *request, SSL *ssl)
{
	const EVP_CIPHER *c;
	const EVP_MD *h;
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	int md_size;

	if (ssl->enc_read_ctx == NULL || ssl->enc_read_ctx->cipher == NULL ||
	    ssl->read_hash == NULL)
		return -1;

	c = ssl->enc_read_ctx->cipher;
	h = EVP_MD_CTX_md(ssl->read_hash);
	if (h)
		md_size = EVP_MD_size(h);
	else if (ssl->s3)
		md_size = ssl->s3->tmp.new_mac_secret_size;
	else
		return -1;

	RDEBUG2("OpenSSL: keyblock size: key_len=%d MD_size=%d "
		   "IV_len=%d", EVP_CIPHER_key_length(c), md_size,
		   EVP_CIPHER_iv_length(c));
	return 2 * (EVP_CIPHER_key_length(c) +
		    md_size +
		    EVP_CIPHER_iv_length(c));
#else
	const SSL_CIPHER *ssl_cipher;
	int cipher, digest;

	ssl_cipher = SSL_get_current_cipher(ssl);
	if (!ssl_cipher)
		return -1;
	cipher = SSL_CIPHER_get_cipher_nid(ssl_cipher);
	digest = SSL_CIPHER_get_digest_nid(ssl_cipher);
	RDEBUG2("OpenSSL: cipher nid %d digest nid %d", cipher, digest);
	if (cipher < 0 || digest < 0)
		return -1;
	c = EVP_get_cipherbynid(cipher);
	h = EVP_get_digestbynid(digest);
	if (!c || !h)
		return -1;

	RDEBUG2("OpenSSL: keyblock size: key_len=%d MD_size=%d IV_len=%d",
		   EVP_CIPHER_key_length(c), EVP_MD_size(h),
		   EVP_CIPHER_iv_length(c));
	return 2 * (EVP_CIPHER_key_length(c) + EVP_MD_size(h) +
		    EVP_CIPHER_iv_length(c));
#endif
}

/**
 * RFC 4851 section 5.1 - EAP-FAST Authentication Phase 1: Key Derivations
 */
static void eap_fast_init_keys(REQUEST *request, tls_session_t *tls_session)
{
	eap_fast_tunnel_t *t = tls_session->opaque;
	uint8_t *buf;
	uint8_t *scratch;
	size_t ksize;

	RDEBUG2("Deriving EAP-FAST keys");

	rad_assert(t->simck == NULL);

	ksize = openssl_get_keyblock_size(request, tls_session->ssl);
	rad_assert(ksize > 0);
	buf = talloc_size(request, ksize + sizeof(*t->keyblock));
	scratch = talloc_size(request, ksize + sizeof(*t->keyblock));

	t->keyblock = talloc(request, eap_fast_keyblock_t);

	eap_fast_tls_gen_challenge(tls_session->ssl, buf, scratch, ksize + sizeof(*t->keyblock), "key expansion");
	memcpy(t->keyblock, &buf[ksize], sizeof(*t->keyblock));
	memset(buf, 0, ksize + sizeof(*t->keyblock));

	t->simck = talloc_size(request, EAP_FAST_SIMCK_LEN);
	memcpy(t->simck, t->keyblock, EAP_FAST_SKS_LEN);	/* S-IMCK[0] = session_key_seed */

	RHEXDUMP(4, "S-IMCK[0]", t->simck, EAP_FAST_SIMCK_LEN);

	t->cmk = talloc_size(request, EAP_FAST_CMK_LEN);	/* note that CMK[0] is not defined */
	t->imckc = 0;

	talloc_free(buf);
	talloc_free(scratch);
}

/**
 * RFC 4851 section 5.2 - Intermediate Compound Key Derivations
 */
static void eap_fast_update_icmk(REQUEST *request, tls_session_t *tls_session, uint8_t *msk)
{
	eap_fast_tunnel_t *t = tls_session->opaque;
	uint8_t imck[EAP_FAST_SIMCK_LEN + EAP_FAST_CMK_LEN];

	RDEBUG2("Updating ICMK");

	T_PRF(t->simck, EAP_FAST_SIMCK_LEN, "Inner Methods Compound Keys", msk, 32, imck, sizeof(imck));

	memcpy(t->simck, imck, EAP_FAST_SIMCK_LEN);
	RHEXDUMP(4, "S-IMCK[j]", t->simck, EAP_FAST_SIMCK_LEN);

	memcpy(t->cmk, &imck[EAP_FAST_SIMCK_LEN], EAP_FAST_CMK_LEN);
	RHEXDUMP(4, "CMK[j]", t->cmk, EAP_FAST_CMK_LEN);

	t->imckc++;

	/*
         * Calculate MSK/EMSK at the same time as they are coupled to ICMK
         *
         * RFC 4851 section 5.4 - EAP Master Session Key Generation
         */
	t->msk = talloc_size(request, EAP_FAST_KEY_LEN);
	T_PRF(t->simck, EAP_FAST_SIMCK_LEN, "Session Key Generating Function", NULL, 0, t->msk, EAP_FAST_KEY_LEN);
	RHEXDUMP(4, "MSK", t->msk, EAP_FAST_KEY_LEN);

	t->emsk = talloc_size(request, EAP_EMSK_LEN);
	T_PRF(t->simck, EAP_FAST_SIMCK_LEN, "Extended Session Key Generating Function", NULL, 0, t->emsk, EAP_EMSK_LEN);
	RHEXDUMP(4, "EMSK", t->emsk, EAP_EMSK_LEN);
}

void eap_fast_tlv_append(tls_session_t *tls_session, int tlv, bool mandatory, int length, const void *data)
{
	uint16_t hdr[2];

	hdr[0] = (mandatory) ? htons(tlv | EAP_FAST_TLV_MANDATORY) : htons(tlv);
	hdr[1] = htons(length);

	tls_session->record_from_buff(&tls_session->clean_in, &hdr, 4);
	tls_session->record_from_buff(&tls_session->clean_in, data, length);
}

static void eap_fast_send_error(tls_session_t *tls_session, int error)
{
	uint32_t value;
	value = htonl(error);

	eap_fast_tlv_append(tls_session, EAP_FAST_TLV_ERROR, true, sizeof(value), &value);
}

static void eap_fast_append_result(tls_session_t *tls_session, PW_CODE code)
{
	eap_fast_tunnel_t *t = (eap_fast_tunnel_t *) tls_session->opaque;

	int type = (t->result_final)
			? EAP_FAST_TLV_RESULT
			: EAP_FAST_TLV_INTERMED_RESULT;

	uint16_t state = (code == PW_CODE_ACCESS_REJECT)
			? EAP_FAST_TLV_RESULT_FAILURE
			: EAP_FAST_TLV_RESULT_SUCCESS;
	state = htons(state);

	eap_fast_tlv_append(tls_session, type, true, sizeof(state), &state);
}

static void eap_fast_send_identity_request(REQUEST *request, tls_session_t *tls_session, eap_session_t *eap_session)
{
	eap_packet_raw_t eap_packet;

	RDEBUG("Sending EAP-Identity");

	eap_packet.code = PW_EAP_REQUEST;
	eap_packet.id = eap_session->this_round->response->id + 1;
	eap_packet.length[0] = 0;
	eap_packet.length[1] = EAP_HEADER_LEN + 1;
	eap_packet.data[0] = PW_EAP_IDENTITY;

	eap_fast_tlv_append(tls_session, EAP_FAST_TLV_EAP_PAYLOAD, true, sizeof(eap_packet), &eap_packet);
}

static void eap_fast_send_pac_tunnel(REQUEST *request, tls_session_t *tls_session)
{
	eap_fast_tunnel_t			*t = tls_session->opaque;
	eap_fast_pac_t				pac;
	eap_fast_attr_pac_opaque_plaintext_t	opaque_plaintext;
	int					alen, dlen;

	memset(&pac, 0, sizeof(pac));
	memset(&opaque_plaintext, 0, sizeof(opaque_plaintext));

	RDEBUG("Sending Tunnel PAC");

	pac.key.hdr.type = htons(EAP_FAST_TLV_MANDATORY | PAC_INFO_PAC_KEY);
	pac.key.hdr.length = htons(sizeof(pac.key.data));
	rad_assert(sizeof(pac.key.data) % sizeof(uint32_t) == 0);
	RANDFILL(pac.key.data);

	pac.info.lifetime.hdr.type = htons(PAC_INFO_PAC_LIFETIME);
	pac.info.lifetime.hdr.length = htons(sizeof(pac.info.lifetime.data));
	pac.info.lifetime.data = htonl(time(NULL) + t->pac_lifetime);

	pac.info.a_id.hdr.type = htons(EAP_FAST_TLV_MANDATORY | PAC_INFO_A_ID);
	pac.info.a_id.hdr.length = htons(sizeof(pac.info.a_id.data));
	memcpy(pac.info.a_id.data, t->a_id, sizeof(pac.info.a_id.data));

	pac.info.a_id_info.hdr.type = htons(PAC_INFO_A_ID_INFO);
	pac.info.a_id_info.hdr.length = htons(sizeof(pac.info.a_id_info.data));
	#define MIN(a,b) (((a)>(b)) ? (b) : (a))
	alen = MIN(talloc_array_length(t->authority_identity) - 1, sizeof(pac.info.a_id_info.data));
	memcpy(pac.info.a_id_info.data, t->authority_identity, alen);

	pac.info.type.hdr.type = htons(EAP_FAST_TLV_MANDATORY | PAC_INFO_PAC_TYPE);
	pac.info.type.hdr.length = htons(sizeof(pac.info.type.data));
	pac.info.type.data = htons(PAC_TYPE_TUNNEL);

	pac.info.hdr.type = htons(EAP_FAST_TLV_MANDATORY | PAC_INFO_PAC_INFO);
	pac.info.hdr.length = htons(sizeof(pac.info.lifetime)
				+ sizeof(pac.info.a_id)
				+ sizeof(pac.info.a_id_info)
				+ sizeof(pac.info.type));

	memcpy(&opaque_plaintext.type, &pac.info.type, sizeof(opaque_plaintext.type));
	memcpy(&opaque_plaintext.lifetime, &pac.info.lifetime, sizeof(opaque_plaintext.lifetime));
	memcpy(&opaque_plaintext.key, &pac.key, sizeof(opaque_plaintext.key));

	RHEXDUMP(4, "PAC-Opaque plaintext data section", (uint8_t const *)&opaque_plaintext, sizeof(opaque_plaintext));

	rad_assert(PAC_A_ID_LENGTH <= EVP_GCM_TLS_TAG_LEN);
	memcpy(pac.opaque.aad, t->a_id, PAC_A_ID_LENGTH);
	rad_assert(RAND_bytes(pac.opaque.iv, sizeof(pac.opaque.iv)) != 0);
	dlen = eap_fast_encrypt((unsigned const char *)&opaque_plaintext, sizeof(opaque_plaintext),
				    t->a_id, PAC_A_ID_LENGTH, t->pac_opaque_key, pac.opaque.iv,
				    pac.opaque.data, pac.opaque.tag);

	pac.opaque.hdr.type = htons(EAP_FAST_TLV_MANDATORY | PAC_INFO_PAC_OPAQUE);
	pac.opaque.hdr.length = htons(sizeof(pac.opaque) - sizeof(pac.opaque.hdr) - sizeof(pac.opaque.data) + dlen);
	RHEXDUMP(4, "PAC-Opaque", (uint8_t const *)&pac.opaque, sizeof(pac.opaque) - sizeof(pac.opaque.data) + dlen);

	eap_fast_tlv_append(tls_session, EAP_FAST_TLV_MANDATORY | EAP_FAST_TLV_PAC, true,
			    sizeof(pac) - sizeof(pac.opaque.data) + dlen, &pac);
}

static void eap_fast_append_crypto_binding(REQUEST *request, tls_session_t *tls_session)
{
	eap_fast_tunnel_t		*t = tls_session->opaque;
	eap_tlv_crypto_binding_tlv_t	binding = {0};
	const int			len = sizeof(binding) - (&binding.reserved - (uint8_t *)&binding);

	RDEBUG("Sending Cryptobinding");

	binding.tlv_type = htons(EAP_FAST_TLV_MANDATORY | EAP_FAST_TLV_CRYPTO_BINDING);
	binding.length = htons(len);
	binding.version = EAP_FAST_VERSION;
	binding.received_version = EAP_FAST_VERSION;	/* FIXME use the clients value */
	binding.subtype = EAP_FAST_TLV_CRYPTO_BINDING_SUBTYPE_REQUEST;

	rad_assert(sizeof(binding.nonce) % sizeof(uint32_t) == 0);
	RANDFILL(binding.nonce);
	binding.nonce[sizeof(binding.nonce) - 1] &= ~0x01; /* RFC 4851 section 4.2.8 */
	RHEXDUMP(4, "NONCE", binding.nonce, sizeof(binding.nonce));

	RHEXDUMP(4, "Crypto-Binding TLV for Compound MAC calculation", (uint8_t const *) &binding, sizeof(binding));

	fr_hmac_sha1(binding.compound_mac, (uint8_t *)&binding, sizeof(binding), t->cmk, EAP_FAST_CMK_LEN);
	RHEXDUMP(4, "Compound MAC", binding.compound_mac, sizeof(binding.compound_mac));

	eap_fast_tlv_append(tls_session, EAP_FAST_TLV_CRYPTO_BINDING, true, len, &binding.reserved);
}

static int eap_fast_verify(REQUEST *request, tls_session_t *tls_session, uint8_t const *data, unsigned int data_len)
{
	uint16_t attr;
	uint16_t length;
	unsigned int remaining = data_len;
	int	total = 0;
	int	num[EAP_FAST_TLV_MAX] = {0};
	eap_fast_tunnel_t *t = (eap_fast_tunnel_t *) tls_session->opaque;
	uint32_t present = 0;

	rad_assert(sizeof(present) * 8 > EAP_FAST_TLV_MAX);

	while (remaining > 0) {
		if (remaining < 4) {
			RDEBUG2("EAP-FAST TLV is too small (%u) to contain a EAP-FAST TLV header", remaining);
			return 0;
		}

		memcpy(&attr, data, sizeof(attr));
		attr = ntohs(attr) & EAP_FAST_TLV_TYPE;

		switch (attr) {
		case EAP_FAST_TLV_RESULT:
		case EAP_FAST_TLV_NAK:
		case EAP_FAST_TLV_ERROR:
		case EAP_FAST_TLV_VENDOR_SPECIFIC:
		case EAP_FAST_TLV_EAP_PAYLOAD:
		case EAP_FAST_TLV_INTERMED_RESULT:
		case EAP_FAST_TLV_PAC:
		case EAP_FAST_TLV_CRYPTO_BINDING:
			num[attr]++;
			present |= 1 << attr;

			if (num[EAP_FAST_TLV_EAP_PAYLOAD] > 1) {
				RDEBUG("Too many EAP-Payload TLVs");
unexpected:
				for (int i = 0; i < EAP_FAST_TLV_MAX; i++)
					if (present & (1 << i))
						RDEBUG(" - attribute %d is present", i);
				eap_fast_send_error(tls_session, EAP_FAST_ERR_UNEXPECTED_TLV);
				return 0;
			}

			if (num[EAP_FAST_TLV_INTERMED_RESULT] > 1) {
				RDEBUG("Too many Intermediate-Result TLVs");
				goto unexpected;
			}
			break;
		default:
			if ((data[0] & 0x80) != 0) {
				RDEBUG("Unknown mandatory TLV %02x", attr);
				goto unexpected;
			}

			num[0]++;
		}

		total++;

		memcpy(&length, data + 2, sizeof(length));
		length = ntohs(length);

		data += 4;
		remaining -= 4;

		if (length > remaining) {
			RDEBUG2("EAP-FAST TLV %u is longer than room remaining in the packet (%u > %u).", attr,
				length, remaining);
			return 0;
		}

		/*
		 * If the rest of the TLVs are larger than
		 * this attribute, continue.
		 *
		 * Otherwise, if the attribute over-flows the end
		 * of the TLCs, die.
		 */
		if (remaining < length) {
			RDEBUG2("EAP-FAST TLV overflows packet!");
			return 0;
		}

		/*
		 * If there's an error, we bail out of the
		 * authentication process before allocating
		 * memory.
		 */
		if ((attr == EAP_FAST_TLV_INTERMED_RESULT) || (attr == EAP_FAST_TLV_RESULT)) {
			uint16_t status;

			if (length < 2) {
				RDEBUG("EAP-FAST TLV %u is too short.  Expected 2, got %d.", attr, length);
				return 0;
			}

			memcpy(&status, data, 2);
			status = ntohs(status);

			if (status == EAP_FAST_TLV_RESULT_FAILURE) {
				RDEBUG("EAP-FAST TLV %u indicates failure.  Rejecting request.", attr);
				return 0;
			}

			if (status != EAP_FAST_TLV_RESULT_SUCCESS) {
				RDEBUG("EAP-FAST TLV %u contains unknown value.  Rejecting request.", attr);
				goto unexpected;
			}
		}

		/*
		 * remaining > length, continue.
		 */
		remaining -= length;
		data += length;
	}

	/*
	 * Check if the peer mixed & matched TLVs.
	 */
	if ((num[EAP_FAST_TLV_NAK] > 0) && (num[EAP_FAST_TLV_NAK] != total)) {
		RDEBUG("NAK TLV sent with non-NAK TLVs.  Rejecting request.");
		goto unexpected;
	}

	if (num[EAP_FAST_TLV_INTERMED_RESULT] > 0 && num[EAP_FAST_TLV_RESULT]) {
		RDEBUG("NAK TLV sent with non-NAK TLVs.  Rejecting request.");
		goto unexpected;
	}

	/*
	 * Check mandatory or not mandatory TLVs.
	 */
	switch (t->stage) {
	case TLS_SESSION_HANDSHAKE:
		if (present) {
			RDEBUG("Unexpected TLVs in TLS Session Handshake stage");
			goto unexpected;
		}
		break;
	case AUTHENTICATION:
		if (present != 1 << EAP_FAST_TLV_EAP_PAYLOAD) {
			RDEBUG("Unexpected TLVs in authentication stage");
			goto unexpected;
		}
		break;
	case CRYPTOBIND_CHECK:
	{
		uint32_t bits = (t->result_final)
				? 1 << EAP_FAST_TLV_RESULT
				: 1 << EAP_FAST_TLV_INTERMED_RESULT;
		if (present & ~(bits | (1 << EAP_FAST_TLV_CRYPTO_BINDING) | (1 << EAP_FAST_TLV_PAC))) {
			RDEBUG("Unexpected TLVs in cryptobind checking stage");
			goto unexpected;
		}
		break;
	}
	case PROVISIONING:
		if (present & ~((1 << EAP_FAST_TLV_PAC) | (1 << EAP_FAST_TLV_RESULT))) {
			RDEBUG("Unexpected TLVs in provisioning stage");
			goto unexpected;
		}
		break;
	case COMPLETE:
		if (present) {
			RDEBUG("Unexpected TLVs in complete stage");
			goto unexpected;
		}
		break;
	default:
		RDEBUG("Unexpected stage %d", t->stage);
		return 0;
	}

	/*
	 * We got this far.  It looks OK.
	 */
	return 1;
}


VALUE_PAIR *eap_fast_fast2vp(REQUEST *request, SSL *ssl, uint8_t const *data, size_t data_len,
                             fr_dict_attr_t const *fast_da, vp_cursor_t *out)
{
	uint16_t	attr;
	uint16_t	length;
	size_t		data_left = data_len;
	VALUE_PAIR	*first = NULL;
	fr_dict_attr_t const *da;

	if (!fast_da)
		fast_da = fr_dict_attr_by_num(NULL, 0, PW_EAP_FAST_TLV);
	rad_assert(fast_da != NULL);

	if (!out) {
		out = talloc(request, vp_cursor_t);
		rad_assert(out != NULL);
		fr_cursor_init(out, &first);
	}

	/*
	 * Decode the TLVs
	 */
	while (data_left > 0) {
		ssize_t decoded;

		/* FIXME do something with mandatory */

		memcpy(&attr, data, sizeof(attr));
		attr = ntohs(attr) & EAP_FAST_TLV_TYPE;

		memcpy(&length, data + 2, sizeof(length));
		length = ntohs(length);

		data += 4;
		data_left -= 4;

		/*
		 * Look up the TLV.
		 *
		 * For now, if it doesn't exist, ignore it.
		 */
		da = fr_dict_attr_child_by_num(fast_da, attr);
		if (!da) goto next_attr;

		if (da->type == PW_TYPE_TLV) {
			eap_fast_fast2vp(request, ssl, data, length, da, out);
			goto next_attr;
		}

		decoded = fr_radius_decode_pair_value(request, out, da, data, length, data_left, NULL);
		if (decoded < 0) {
			RERROR("Failed decoding %s: %s", da->name, fr_strerror());
			goto next_attr;
		}

	next_attr:
		while (fr_cursor_next(out)) {
			/* nothing */
		}

		data += length;
		data_left -= length;
	}

	/*
	 * We got this far.  It looks OK.
	 */
	return first;
}


static void eap_vp2fast(tls_session_t *tls_session, VALUE_PAIR *first)
{
	VALUE_PAIR	*vp;
	vp_cursor_t	cursor;

	for (vp = fr_cursor_init(&cursor, &first); vp; vp = fr_cursor_next(&cursor))
	{
		if (vp->da->vendor != 0 && vp->da->attr != PW_EAP_MESSAGE) continue;

		eap_fast_tlv_append(tls_session, EAP_FAST_TLV_EAP_PAYLOAD, true, vp->vp_length, vp->vp_octets);
	}
}


/*
 * Use a reply packet to determine what to do.
 */
static rlm_rcode_t CC_HINT(nonnull) process_reply(NDEBUG_UNUSED eap_session_t *eap_session,
						  tls_session_t *tls_session,
						  REQUEST *request, RADIUS_PACKET *reply)
{
	rlm_rcode_t			rcode = RLM_MODULE_REJECT;
	VALUE_PAIR			*vp, *tunnel_vps = NULL;
	vp_cursor_t			cursor;
	vp_cursor_t			to_tunnel;

	eap_fast_tunnel_t	*t = tls_session->opaque;

	rad_assert(eap_session->request == request);

	/*
	 * If the response packet was Access-Accept, then
	 * we're OK.  If not, die horribly.
	 *
	 * FIXME: Take MS-CHAP2-Success attribute, and
	 * tunnel it back to the client, to authenticate
	 * ourselves to the client.
	 *
	 * FIXME: If we have an Access-Challenge, then
	 * the Reply-Message is tunneled back to the client.
	 *
	 * FIXME: If we have an EAP-Message, then that message
	 * must be tunneled back to the client.
	 *
	 * FIXME: If we have an Access-Challenge with a State
	 * attribute, then do we tunnel that to the client, or
	 * keep track of it ourselves?
	 *
	 * FIXME: EAP-Messages can only start with 'identity',
	 * NOT 'eap start', so we should check for that....
	 */
	switch (reply->code) {
	case PW_CODE_ACCESS_ACCEPT:
		RDEBUG("Got tunneled Access-Accept");

		fr_cursor_init(&to_tunnel, &tunnel_vps);
		rcode = RLM_MODULE_OK;

		/*
		 * Copy what we need into the TTLS tunnel and leave
		 * the rest to be cleaned up.
		 */
		for (vp = fr_cursor_init(&cursor, &reply->vps); vp; vp = fr_cursor_next(&cursor)) {
			switch (vp->da->vendor) {
			case VENDORPEC_MICROSOFT:
				/* FIXME must be a better way to capture/re-derive this later for ISK */
				if (vp->da->attr == PW_MSCHAP_MPPE_SEND_KEY)
					memcpy(t->isk.mppe_send, vp->vp_octets, CHAP_VALUE_LENGTH);
				if (vp->da->attr == PW_MSCHAP_MPPE_RECV_KEY)
					memcpy(t->isk.mppe_recv, vp->vp_octets, CHAP_VALUE_LENGTH);

				if (vp->da->attr == PW_MSCHAP2_SUCCESS) {
					RDEBUG("Got %s, tunneling it to the client in a challenge", vp->da->name);
					rcode = RLM_MODULE_HANDLED;
					t->authenticated = true;
					fr_cursor_prepend(&to_tunnel, fr_pair_copy(tls_session, vp));
				}
				break;

			default:
				break;
			}
		}
		RHEXDUMP(4, "ISK[j]", (uint8_t *)&t->isk, 2 * CHAP_VALUE_LENGTH); /* FIXME (part of above) */
		break;

	case PW_CODE_ACCESS_REJECT:
		RDEBUG("Got tunneled Access-Reject");
		rcode = RLM_MODULE_REJECT;
		break;

	/*
	 * Handle Access-Challenge, but only if we
	 * send tunneled reply data.  This is because
	 * an Access-Challenge means that we MUST tunnel
	 * a Reply-Message to the client.
	 */
	case PW_CODE_ACCESS_CHALLENGE:
		RDEBUG("Got tunneled Access-Challenge");

		fr_cursor_init(&to_tunnel, &tunnel_vps);

		/*
		 * Copy what we need into the TTLS tunnel and leave
		 * the rest to be cleaned up.
		 */
		for (vp = fr_cursor_init(&cursor, &reply->vps);
		     vp;
		     vp = fr_cursor_next(&cursor)) {
			switch (vp->da->vendor) {
			case 0:
				switch (vp->da->attr) {
				case PW_EAP_MESSAGE:
				case PW_REPLY_MESSAGE:
					fr_cursor_prepend(&to_tunnel, fr_pair_copy(tls_session, vp));
					break;

				default:
					break;

				}

			default:
				continue;
			}
		}
		rcode = RLM_MODULE_HANDLED;
		break;

	default:
		RDEBUG("Unknown RADIUS packet type %d: rejecting tunneled user", reply->code);
		rcode = RLM_MODULE_INVALID;
		break;
	}


	/*
	 * Pack any tunnelled VPs and send them back
	 * to the supplicant.
	 */
	if (tunnel_vps) {
		RDEBUG("Sending tunneled reply attributes");
		rdebug_pair_list(L_DBG_LVL_2, request, tunnel_vps, NULL);

		eap_vp2fast(tls_session, tunnel_vps);
		fr_pair_list_free(&tunnel_vps);
	}

	return rcode;
}

static PW_CODE eap_fast_eap_payload(REQUEST *request, eap_session_t *eap_session,
				    tls_session_t *tls_session, VALUE_PAIR *tlv_eap_payload)
{
	PW_CODE			code = PW_CODE_ACCESS_REJECT;
	rlm_rcode_t		rcode;
	VALUE_PAIR		*vp;
	eap_fast_tunnel_t	*t;
	REQUEST			*fake;

	RDEBUG("Processing received EAP Payload");

	/*
	 * Allocate a fake REQUEST structure.
	 */
	fake = request_alloc_fake(request);
	rad_assert(!fake->packet->vps);

	t = (eap_fast_tunnel_t *) tls_session->opaque;

	/*
	 * Add the tunneled attributes to the fake request.
	 */

	fake->packet->vps = fr_pair_afrom_num(fake->packet, 0, PW_EAP_MESSAGE);
	fr_pair_value_memcpy(fake->packet->vps, tlv_eap_payload->vp_octets, tlv_eap_payload->vp_length);

	RDEBUG("Got tunneled request");
	rdebug_pair_list(L_DBG_LVL_1, request, fake->packet->vps, NULL);

	/*
	 * Tell the request that it's a fake one.
	 */
	fr_pair_make(fake->packet, &fake->packet->vps, "Freeradius-Proxied-To", "127.0.0.1", T_OP_EQ);

	/*
	 * Update other items in the REQUEST data structure.
	 */
	fake->username = fr_pair_find_by_num(fake->packet->vps, 0, PW_USER_NAME, TAG_ANY);
	fake->password = fr_pair_find_by_num(fake->packet->vps, 0, PW_USER_PASSWORD, TAG_ANY);

	/*
	 * No User-Name, try to create one from stored data.
	 */
	if (!fake->username) {
		/*
		 * No User-Name in the stored data, look for
		 * an EAP-Identity, and pull it out of there.
		 */
		if (!t->username) {
			vp = fr_pair_find_by_num(fake->packet->vps, 0, PW_EAP_MESSAGE, TAG_ANY);
			if (vp &&
			    (vp->vp_length >= EAP_HEADER_LEN + 2) &&
			    (vp->vp_strvalue[0] == PW_EAP_RESPONSE) &&
			    (vp->vp_strvalue[EAP_HEADER_LEN] == PW_EAP_IDENTITY) &&
			    (vp->vp_strvalue[EAP_HEADER_LEN + 1] != 0)) {
				/*
				 * Create & remember a User-Name
				 */
				t->username = fr_pair_make(t, NULL, "User-Name", NULL, T_OP_EQ);
				rad_assert(t->username != NULL);

				fr_pair_value_bstrncpy(t->username, vp->vp_octets + 5, vp->vp_length - 5);

				RDEBUG("Got tunneled identity of %s", t->username->vp_strvalue);
			} else {
				/*
				 * Don't reject the request outright,
				 * as it's permitted to do EAP without
				 * user-name.
				 */
				RWDEBUG2("No EAP-Identity found to start EAP conversation");
			}
		} /* else there WAS a t->username */

		if (t->username) {
			vp = fr_pair_list_copy(fake->packet, t->username);
			fr_pair_add(&fake->packet->vps, vp);
			fake->username = fr_pair_find_by_num(fake->packet->vps, 0, PW_USER_NAME, TAG_ANY);
		}
	} /* else the request ALREADY had a User-Name */

	if (t->stage == AUTHENTICATION) {	/* FIXME do this only for MSCHAPv2 */
		VALUE_PAIR *tvp;

		tvp = fr_pair_afrom_num(fake->packet, 0, PW_EAP_TYPE);
		tvp->vp_integer = t->default_provisioning_method;
		fr_pair_add(&fake->control, tvp);

		/*
		 * RFC 5422 section 3.2.3 - Authenticating Using EAP-FAST-MSCHAPv2
		 */
		if (t->mode == EAP_FAST_PROVISIONING_ANON) {
			tvp = fr_pair_afrom_num(fake->packet, VENDORPEC_MICROSOFT, PW_MSCHAP_CHALLENGE);
			fr_pair_value_memcpy(tvp, t->keyblock->server_challenge, CHAP_VALUE_LENGTH);
			fr_pair_add(&fake->control, tvp);
			RHEXDUMP(4, "MSCHAPv2 auth_challenge", t->keyblock->server_challenge, CHAP_VALUE_LENGTH);

			tvp = fr_pair_afrom_num(fake->packet, 0, PW_MS_CHAP_PEER_CHALLENGE);
			fr_pair_value_memcpy(tvp, t->keyblock->client_challenge, CHAP_VALUE_LENGTH);
			fr_pair_add(&fake->control, tvp);
			RHEXDUMP(4, "MSCHAPv2 peer_challenge", t->keyblock->client_challenge, CHAP_VALUE_LENGTH);
		}
	}

	/*
	 * Call authentication recursively, which will
	 * do PAP, CHAP, MS-CHAP, etc.
	 */
	eap_virtual_server(request, fake, eap_session, t->virtual_server);

	/*
	 * Decide what to do with the reply.
	 */
	switch (fake->reply->code) {
	case 0:			/* No reply code, must be proxied... */
#ifdef WITH_PROXY
		vp = fr_pair_find_by_num(fake->control, 0, PW_PROXY_TO_REALM, TAG_ANY);
		if (vp) {
			int			ret;
			eap_tunnel_data_t	*tunnel;

			RDEBUG("Tunneled authentication will be proxied to %s", vp->vp_strvalue);

			/*
			 * Tell the original request that it's going
			 * to be proxied.
			 */
			fr_pair_list_mcopy_by_num(request, &request->control, &fake->control, 0,
						  PW_PROXY_TO_REALM, TAG_ANY);

			/*
			 * Seed the proxy packet with the
			 * tunneled request.
			 */
			rad_assert(!request->proxy);

			request->proxy = request_alloc_proxy(request);

			request->proxy->packet = talloc_steal(request->proxy, fake->packet);
			memset(&request->proxy->packet->src_ipaddr, 0,
			       sizeof(request->proxy->packet->src_ipaddr));
			memset(&request->proxy->packet->src_ipaddr, 0,
			       sizeof(request->proxy->packet->src_ipaddr));
			request->proxy->packet->src_port = 0;
			request->proxy->packet->dst_port = 0;
			fake->packet = NULL;
			fr_radius_free(&fake->reply);
			fake->reply = NULL;

			/*
			 * Set up the callbacks for the tunnel
			 */
			tunnel = talloc_zero(request, eap_tunnel_data_t);
			tunnel->tls_session = tls_session;

			/*
			 * Associate the callback with the request.
			 */
			ret = request_data_add(request, request->proxy, REQUEST_DATA_EAP_TUNNEL_CALLBACK,
					       tunnel, false, false, false);
			rad_cond_assert(ret == 0);

			/*
			 * rlm_eap.c has taken care of associating
			 * the eap_session with the fake request.
			 *
			 * So we associate the fake request with
			 * this request.
			 */
			ret = request_data_add(request, request->proxy, REQUEST_DATA_EAP_MSCHAP_TUNNEL_CALLBACK,
					       fake, true, false, false);
			rad_cond_assert(ret == 0);

			fake = NULL;

			/*
			 * Didn't authenticate the packet, but
			 * we're proxying it.
			 */
			code = PW_CODE_STATUS_CLIENT;

		} else
#endif	/* WITH_PROXY */
		  {
			  RDEBUG("No tunneled reply was found, and the request was not proxied: rejecting the user.");
			  code = PW_CODE_ACCESS_REJECT;
		  }
		break;

	default:
		/*
		 * Returns RLM_MODULE_FOO, and we want to return PW_FOO
		 */
		rcode = process_reply(eap_session, tls_session, request, fake->reply);
		switch (rcode) {
		case RLM_MODULE_REJECT:
			code = PW_CODE_ACCESS_REJECT;
			break;

		case RLM_MODULE_HANDLED:
			code = PW_CODE_ACCESS_CHALLENGE;
			break;

		case RLM_MODULE_OK:
			code = PW_CODE_ACCESS_ACCEPT;
			break;

		default:
			code = PW_CODE_ACCESS_REJECT;
			break;
		}
		break;
	}

	talloc_free(fake);

	return code;
}

static PW_CODE eap_fast_crypto_binding(REQUEST *request, UNUSED eap_session_t *eap_session,
				       tls_session_t *tls_session, eap_tlv_crypto_binding_tlv_t *binding)
{
	uint8_t			cmac[sizeof(binding->compound_mac)];
	eap_fast_tunnel_t	*t = tls_session->opaque;

	memcpy(cmac, binding->compound_mac, sizeof(cmac));
	memset(binding->compound_mac, 0, sizeof(binding->compound_mac));

	RHEXDUMP(4, "Crypto-Binding TLV for Compound MAC calculation", (uint8_t const *) binding, sizeof(*binding));
	RHEXDUMP(4, "Received Compound MAC", cmac, sizeof(cmac));

	fr_hmac_sha1(binding->compound_mac, (uint8_t *)binding, sizeof(*binding), t->cmk, EAP_FAST_CMK_LEN);
	if (memcmp(binding->compound_mac, cmac, sizeof(cmac))) {
		RDEBUG2("Crypto-Binding TLV mis-match");
		RHEXDUMP(4, "Calculated Compound MAC",
			 (uint8_t const *) binding->compound_mac, sizeof(binding->compound_mac));
		return PW_CODE_ACCESS_REJECT;
	}

	return PW_CODE_ACCESS_ACCEPT;
}

static PW_CODE eap_fast_process_tlvs(REQUEST *request, eap_session_t *eap_session,
				     tls_session_t *tls_session, VALUE_PAIR *fast_vps)
{
	eap_fast_tunnel_t		*t = (eap_fast_tunnel_t *) tls_session->opaque;
	VALUE_PAIR			*vp;
	vp_cursor_t			cursor;
	eap_tlv_crypto_binding_tlv_t	*binding = NULL;

	for (vp = fr_cursor_init(&cursor, &fast_vps); vp; vp = fr_cursor_next(&cursor)) {
		PW_CODE code = PW_CODE_ACCESS_REJECT;
		char *value;

		switch (vp->da->parent->attr) {
		case PW_EAP_FAST_TLV:
			switch (vp->da->attr) {
			case EAP_FAST_TLV_EAP_PAYLOAD:
				code = eap_fast_eap_payload(request, eap_session, tls_session, vp);
				if (code == PW_CODE_ACCESS_ACCEPT)
					t->stage = CRYPTOBIND_CHECK;
				break;
			case EAP_FAST_TLV_RESULT:
			case EAP_FAST_TLV_INTERMED_RESULT:
				code = PW_CODE_ACCESS_ACCEPT;
				t->stage = PROVISIONING;
				break;
			default:
				value = fr_pair_asprint(request->packet, vp, '"');
				RDEBUG2("ignoring unknown %s", value);
				talloc_free(value);
				continue;
			}
			break;
		case EAP_FAST_TLV_CRYPTO_BINDING:
			if (!binding) {
				binding = talloc_zero(request->packet, eap_tlv_crypto_binding_tlv_t);
				binding->tlv_type = htons(EAP_FAST_TLV_MANDATORY | EAP_FAST_TLV_CRYPTO_BINDING);
				binding->length = htons(sizeof(*binding) - 2 * sizeof(uint16_t));
			}
			/*
			 * fr_radius_encode_pair() does not work for structures
			 */
			switch (vp->da->attr) {
			case 1:	/* PW_EAP_FAST_CRYPTO_BINDING_RESERVED */
				binding->reserved = vp->vp_integer;
				break;
			case 2:	/* PW_EAP_FAST_CRYPTO_BINDING_VERSION */
				binding->version = vp->vp_integer;
				break;
			case 3:	/* PW_EAP_FAST_CRYPTO_BINDING_RECV_VERSION */
				binding->received_version = vp->vp_integer;
				break;
			case 4:	/* PW_EAP_FAST_CRYPTO_BINDING_SUB_TYPE */
				binding->subtype = vp->vp_integer;
				break;
			case 5:	/* PW_EAP_FAST_CRYPTO_BINDING_NONCE */
				memcpy(binding->nonce, vp->vp_octets, vp->vp_length);
				break;
			case 6:	/* PW_EAP_FAST_CRYPTO_BINDING_COMPOUND_MAC */
				memcpy(binding->compound_mac, vp->vp_octets, vp->vp_length);
				break;
			}
			continue;
		case EAP_FAST_TLV_PAC:
			switch (vp->da->attr) {
			case PAC_INFO_PAC_ACK:
				if (vp->vp_integer == EAP_FAST_TLV_RESULT_SUCCESS) {
					code = PW_CODE_ACCESS_ACCEPT;
					t->pac.expires = UINT32_MAX;
					t->pac.expired = false;
					t->stage = COMPLETE;
				}
				break;
			case PAC_INFO_PAC_TYPE:
				if (vp->vp_integer != PAC_TYPE_TUNNEL) {
					RDEBUG("only able to serve Tunnel PAC's, ignoring request");
					continue;
				}
				t->pac.send = true;
				continue;
			default:
				value = fr_pair_asprint(request->packet, vp, '"');
				RDEBUG2("ignoring unknown EAP-FAST-PAC-TLV %s", value);
				talloc_free(value);
				continue;
			}
			break;
		default:
			value = fr_pair_asprint(request->packet, vp, '"');
			RDEBUG2("ignoring non-EAP-FAST TLV %s", value);
			talloc_free(value);
			continue;
		}

		if (code == PW_CODE_ACCESS_REJECT)
			return PW_CODE_ACCESS_REJECT;
	}

	if (binding) {
		PW_CODE code = eap_fast_crypto_binding(request, eap_session, tls_session, binding);
		if (code == PW_CODE_ACCESS_ACCEPT)
			t->stage = PROVISIONING;
	}

	return PW_CODE_ACCESS_ACCEPT;
}


/*
 * Process the inner tunnel data
 */
PW_CODE eap_fast_process(eap_session_t *eap_session, tls_session_t *tls_session)
{
	PW_CODE			code;
	VALUE_PAIR		*fast_vps;
	uint8_t			const *data;
	size_t			data_len;
	eap_fast_tunnel_t		*t;
	REQUEST			*request = eap_session->request;

	/*
	 * Just look at the buffer directly, without doing
	 * record_to_buff.
	 */
	data_len = tls_session->clean_out.used;
	tls_session->clean_out.used = 0;
	data = tls_session->clean_out.data;

	t = (eap_fast_tunnel_t *) tls_session->opaque;

	/*
	 * See if the tunneled data is well formed.
	 */
	if (!eap_fast_verify(request, tls_session, data, data_len)) return PW_CODE_ACCESS_REJECT;

	if (t->stage == TLS_SESSION_HANDSHAKE) {
		rad_assert(t->mode == EAP_FAST_UNKNOWN);

		char buf[256];
		if (strstr(SSL_CIPHER_description(SSL_get_current_cipher(tls_session->ssl),
						  buf, sizeof(buf)), "Au=None")) {
			/* FIXME enforce MSCHAPv2 - RFC 5422 section 3.2.2 */
			RDEBUG2("Using anonymous provisioning");
			t->mode = EAP_FAST_PROVISIONING_ANON;
			t->pac.send = true;
		} else {
			if (SSL_session_reused(tls_session->ssl)) {
				RDEBUG("Session Resumed from PAC");
				t->mode = EAP_FAST_NORMAL_AUTH;
			} else {
				RDEBUG2("Using authenticated provisioning");
				t->mode = EAP_FAST_PROVISIONING_AUTH;
			}

			if (!t->pac.expires || t->pac.expired || t->pac.expires - time(NULL) < t->pac_lifetime * 0.6)
				t->pac.send = true;
		}

		eap_fast_init_keys(request, tls_session);

		eap_fast_send_identity_request(request, tls_session, eap_session);

		t->stage = AUTHENTICATION;
		return PW_CODE_ACCESS_CHALLENGE;
	}

	fast_vps = eap_fast_fast2vp(request, tls_session->ssl, data, data_len, NULL, NULL);

	RDEBUG("Got Tunneled FAST TLVs");
	rdebug_pair_list(L_DBG_LVL_1, request, fast_vps, NULL);

	code = eap_fast_process_tlvs(request, eap_session, tls_session, fast_vps);

	fr_pair_list_free(&fast_vps);

	if (code == PW_CODE_ACCESS_REJECT) return PW_CODE_ACCESS_REJECT;

	switch (t->stage) {
	case AUTHENTICATION:
		code = PW_CODE_ACCESS_CHALLENGE;
		break;
	case CRYPTOBIND_CHECK:
	{
		if (t->mode != EAP_FAST_PROVISIONING_ANON && !t->pac.send)
			t->result_final = true;

		eap_fast_append_result(tls_session, code);

		eap_fast_update_icmk(request, tls_session, (uint8_t *)&t->isk);
		eap_fast_append_crypto_binding(request, tls_session);

		code = PW_CODE_ACCESS_CHALLENGE;
		break;
	}
	case PROVISIONING:
		t->result_final = true;

		eap_fast_append_result(tls_session, code);

		if (code == PW_CODE_ACCESS_REJECT)
			break;

		if (t->pac.send) {
			RDEBUG("Peer requires new PAC");
			eap_fast_send_pac_tunnel(request, tls_session);
			code = PW_CODE_ACCESS_CHALLENGE;
			break;
		}

		t->stage = COMPLETE;
		/* fallthrough */
	case COMPLETE:
		/*
		 * RFC 5422 section 3.5 - Network Access after EAP-FAST Provisioning
		 */
		if ((t->pac.type && t->pac.expired) || t->mode == EAP_FAST_PROVISIONING_ANON) {
			RDEBUG("Rejecting expired PAC or unauthenticated provisioning");
			code = PW_CODE_ACCESS_REJECT;
			break;
		}

		/*
		 * eap_tls_gen_mppe_keys() is unsuitable for EAP-FAST as Cisco decided
		 * it would be a great idea to flip the recv/send keys around
		 */
		#define EAPTLS_MPPE_KEY_LEN 32
		eap_add_reply(request, "MS-MPPE-Recv-Key", t->msk, EAPTLS_MPPE_KEY_LEN);
		eap_add_reply(request, "MS-MPPE-Send-Key", &t->msk[EAPTLS_MPPE_KEY_LEN], EAPTLS_MPPE_KEY_LEN);
		eap_add_reply(request, "EAP-MSK", t->msk, EAP_FAST_KEY_LEN);
		eap_add_reply(request, "EAP-EMSK", t->emsk, EAP_EMSK_LEN);

		break;
	default:
		RERROR("no idea! %d", t->stage);
		code = PW_CODE_ACCESS_REJECT;
	}

	return code;
}
