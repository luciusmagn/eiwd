/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <ell/ell.h>

#include "crypto.h"
#include "simutil.h"

/*
 * RFC 3174 functions
 */
/*
 * Section 3a - Circular left shift function S
 */
#define S(n, x) (((x) << (n)) | ((x) >> (32 - (n))))

/*
 * Section 5 - Functions and Constants Used
 *
 * K(t) - sequence of constant words K(0) - K(79)
 * (represented as a function, index t is constant for every 20 indexes)
 */
static uint32_t K(int t)
{
	if (t >= 0 && t <= 19)
		return 0x5a827999;
	else if (t >= 20 && t <= 39)
		return 0x6ed9eba1;
	else if (t >= 40 && t <= 59)
		return 0x8f1bbcdc;
	else if (t >= 60 && t <= 79)
		return 0xca62c1d6;

	return 0;
}

/*
 * f(t, B, C, D) - sequence of logical functions f(0) - f(79)
 * Every 20 indexes the value of t computes a different bit manipulation of
 * B, C and D
 */
static uint32_t f(int t, uint32_t B, uint32_t C, uint32_t D)
{
	if (t >= 0 && t <= 19)
		return (B & C) | ((~B) & D);
	else if (t >= 20 && t <= 39)
		return B ^ C ^ D;
	else if (t >= 40 && t <= 59)
		return (B & C) | (B & D) | (C & D);
	else if (t >= 60 && t <= 79)
		return B ^ C ^ D;

	return 0;
}

/*
 * RFC 3174 Section 6.1 Method 1
 *
 * Core SHA1 block digest function. Computes the SHA1 digest of a single block.
 * Named G as it appears in FIPS 182 PRNG.
 *
 * The Linux kernel does not expose this specific block digest function to the
 * user. The SHA1 function exposed in the kernel automatically does the length
 * encoded padding to the block which is different than what EAP-SIM requires.
 * EAP-SIM requires and extra bits in the block to be zero. This function was
 * implemented for this reason.
 */
static void G(uint32_t *out, uint8_t *block)
{
	int t;
	uint32_t H[5];
	uint32_t W[80];
	uint32_t A, B, C, D, E;
	uint32_t TEMP;

	H[0] = out[0];
	H[1] = out[1];
	H[2] = out[2];
	H[3] = out[3];
	H[4] = out[4];

	/*
	 * a. Divide M (block) into 16 words, W(0) ... W(15) where W(0) is the
	 * left-most word
	 */
	for (t = 0; t < 16; t++) {
		/* copy each word */
		W[t] = L_BE32_TO_CPU(((uint32_t *)block)[t]);
	}
	/*
	 * b. for t = 16 to 79 do
	 */
	for (t = 16; t <= 79; t++) {
		/* W(t) = S^1(W(t-3) XOR W(t-8) XOR W(t-14) XOR W(t-16)) */
		W[t] = S(1, (W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16]));
	}
	/* c. Let A = H0, B = H1, C = H2, D = H3, E = H4 */
	A = H[0];
	B = H[1];
	C = H[2];
	D = H[3];
	E = H[4];

	/* d. For t = 0 to 79 do */
	for (t = 0; t <= 79; t++) {
		/* TEMP = S^5(A) + f(t;B,C,D) + E + W(t) + K(t); */
		TEMP = (S(5, A)) + (f(t, B, C, D) + E + W[t] + K(t));
		/* E = D;  D = C;  C = S^30(B);  B = A; A = TEMP; */
		E = D; D = C; C = S(30, B); B = A; A = TEMP;
	}

	/*
	 * e. Let H[0-4] == A, B, C, D, E
	 */
	H[0] += A;
	H[1] += B;
	H[2] += C;
	H[3] += D;
	H[4] += E;

	memcpy(out, H, sizeof(H));
}

/*
 * Helper to XOR an array
 * to - result of XOR array
 * a - array 1
 * b - array 2
 * len - size of aray
 */
#define XOR(to, a, b, len) \
	for (i = 0; i < len; i++) { \
		to[i] = a[i] ^ b[i]; \
	}

bool eap_aka_get_milenage(const uint8_t *opc, const uint8_t *k,
		const uint8_t *rand, const uint8_t *sqn, const uint8_t *amf,
		uint8_t *autn, uint8_t *ck, uint8_t *ik, uint8_t *res)
{
	/* algorithm variables: TEMP, IN1, OUT1, OUT2, OUT5 (OUT3/4 == IK/CK) */
	uint8_t temp[16];
	uint8_t in1[16];
	uint8_t out1[16], out2[16], out5[16];
	/* other variables */
	struct l_cipher *aes;
	int i;
	uint8_t tmp1[16];
	uint8_t tmp2[16];

	aes = l_cipher_new(L_CIPHER_AES, k, 16);

	/* temp = TEMP = E[RAND ^ OPc]k */
	XOR(tmp1, rand, opc, 16);
	l_cipher_encrypt(aes, tmp1, temp, 16);

	/* IN1[0-47] = SQN[0-47] */
	memcpy(in1, sqn, 6);
	/* IN1[48-63] = AMF[0-15] */
	memcpy(in1 + 6, amf, 2);
	/* IN1[64-111] = SQN[0-47] */
	memcpy(in1 + 8, sqn, 6);
	/* IN1[112-127] = AMF[0-15] */
	memcpy(in1 + 14, amf, 2);

	/*
	 * f1 and f1* output OUT1
	 */
	/*
	 * tmp1 = rot(IN1 ^ OPc)r1
	 * r1 = 64 bits = 8 bytes
	 */
	for (i = 0; i < 16; i++)
		tmp1[(i + 8) % 16] = in1[i] ^ opc[i];

	/* tmp2 = TEMP ^ tmp1 */
	XOR(tmp2, temp, tmp1, 16);
	/* tmp2 = E[tmp2]k */
	l_cipher_encrypt(aes, tmp2, tmp1, 16);
	/* out1 = OUT1 = tmp1 ^ opc */
	XOR(out1, tmp1, opc, 16);

	/*
	 * f2 outputs OUT2 (RES | AK)
	 *
	 * r2 = 0 == no rotation
	 */
	/* tmp1 = rot(TEMP ^ OPc)r2 */
	XOR(tmp1, temp, opc, 16);
	/* tmp1 ^ c2. c2 at bit 127 == 1 */
	tmp1[15] ^= 1;
	l_cipher_encrypt(aes, tmp1, out2, 16);

	/* get RES from OUT2 */
	XOR(out2, out2, opc, 16);
	memcpy(res, out2 + 8, 8);

	/* AUTN = (SQN ^ AK) | AMF | MAC_A */
	XOR(autn, sqn, out2, 6);
	memcpy(autn + 6, amf, 2);
	memcpy(autn + 8, out1, 8);

	/*
	 * f3 outputs CK (OUT3)
	 *
	 * tmp1 = rot(TEMP ^ OPc)r3
	 *
	 * r3 = 32 bits = 4 bytes
	 */
	for (i = 0; i < 16; i++)
		tmp1[(i + 12) % 16] = temp[i] ^ opc[i];

	/* tmp1 ^ c3. c3 at bit 126 == 1 */
	tmp1[15] ^= 1 << 1;
	l_cipher_encrypt(aes, tmp1, ck, 16);
	/* ck ^ opc */
	XOR(ck, ck, opc, 16);

	/*
	 * f4 outputs IK (OUT4)
	 *
	 * tmp1 = rot(TEMP ^ OPc)r4
	 *
	 * r4 = 64 bits = 8 bytes
	 */
	for (i = 0; i < 16; i++)
		tmp1[(i + 8) % 16] = temp[i] ^ opc[i];

	/* tmp1 ^ c4. c4 at bit 125 == 1 */
	tmp1[15] ^= 1 << 2;
	l_cipher_encrypt(aes, tmp1, ik, 16);
	/* ik ^ opc */
	XOR(ik, ik, opc, 16);

	/*
	 * f5* outputs AK' (OUT5)
	 */
	for (i = 0; i < 16; i++)
		tmp1[(i + 4) % 16] = temp[i] ^ opc[i];

	/* tmp1 ^ c5. c5 at bit 124 == 1 */
	tmp1[15] ^= 1 << 3;
	l_cipher_encrypt(aes, tmp1, out5, 16);
	/* out5 ^ opc */
	XOR(out5, out5, opc, 16);

	l_cipher_free(aes);

	return true;
}

void eap_sim_fips_prf(const void *seed, size_t slen, uint8_t *out, size_t olen)
{
	uint8_t xkey[64];
	uint32_t w_i[5];
	uint32_t t[] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
			0xC3D2E1F0 };
	uint8_t *pos = out;
	uint32_t c;
	int j, i;

	/* Copy seed and zero pad remainder */
	memcpy(xkey, seed, slen);
	memset(xkey + slen, 0, sizeof(xkey) - slen);

	for (j = 0; j < (int)olen / 40; j++) {
		for (i = 0; i < 2; i++) {
			int k;

			memcpy(w_i, t, sizeof(t));
			/* w_i = G(t, XVAL) */
			G(w_i, xkey);
			for (k = 0; k < 5; k++)
				w_i[k] = L_CPU_TO_BE32(w_i[k]);

			memcpy(pos, w_i, 20);
			/* XKEY = (1 + XKEY + w_i) mod 2^b*/
			c = 1;
			for (k = 19; k >= 0; k--) {
				uint32_t sum = xkey[k] + pos[k] + c;

				xkey[k] = sum & 0xff;
				c = sum >> 8;
			}
			pos += 20;
		}
	}
}

bool eap_sim_get_encryption_keys(const uint8_t *buf, uint8_t *k_encr,
		uint8_t *k_aut, uint8_t *msk, uint8_t *emsk)
{
	const uint8_t *pos = buf;

	if (!buf || !msk || !emsk) {
		l_error("key pointers are invalid");
		return false;
	}

	if (k_encr)
		memcpy(k_encr, pos, EAP_SIM_K_ENCR_LEN);

	pos += EAP_SIM_K_ENCR_LEN;
	if (k_aut)
		memcpy(k_aut, pos, EAP_SIM_K_AUT_LEN);

	pos += EAP_SIM_K_AUT_LEN;
	memcpy(msk, pos, EAP_SIM_MSK_LEN);
	pos += EAP_SIM_MSK_LEN;
	memcpy(emsk, pos, EAP_SIM_EMSK_LEN);

	return true;
}

bool eap_sim_derive_mac(const uint8_t *buf, size_t len, const uint8_t *key,
		uint8_t *mac)
{
	return hmac_sha1(key, EAP_SIM_K_AUT_LEN, buf, len, mac,
			EAP_SIM_MAC_LEN);
}

size_t eap_sim_build_header(struct eap_state *eap, enum eap_type method,
		uint8_t type, uint8_t *buf, uint16_t len)
{
	buf[0] = 0x02;
	eap_save_last_id(eap, &buf[1]);
	l_put_be16(len, buf + 2);
	buf[4] = method;
	buf[5] = type;
	buf[6] = 0x00;
	buf[7] = 0x00;
	return 8;
}

void eap_sim_client_error(struct eap_state *eap, enum eap_type type,
		uint16_t code)
{
	uint8_t buf[12];

	eap_sim_build_header(eap, type, 0x0e, buf, 12);
	buf[8] = EAP_SIM_AT_CLIENT_ERROR_CODE;
	buf[9] = 1;
	l_put_be16(code, buf + 10);

	eap_send_response(eap, type, buf, 12);
}

size_t eap_sim_add_attribute(uint8_t *buf, enum eap_sim_at attr,
		uint8_t ptype, uint8_t *data, uint16_t dlen)
{
	int i;
	uint8_t pos = 0;
	uint8_t pad = 0;

	buf[pos++] = attr;

	if (ptype == EAP_SIM_PAD_NONE)
		/* no padding indicates data directly follows ID/size */
		buf[pos++] = EAP_SIM_ROUND(dlen + 2) / 4;
	else
		/* any padding indicates 2 extra bytes before data */
		buf[pos++] = EAP_SIM_ROUND(dlen + 4) / 4;

	if (ptype == EAP_SIM_PAD_LENGTH) {
		/* Encode length in next two bytes */
		l_put_be16(dlen, buf + pos);
		pos += 2;
	} else if (ptype == EAP_SIM_PAD_ZERO) {
		buf[pos++] = 0x00;
		buf[pos++] = 0x00;
	} else if (ptype == EAP_SIM_PAD_LENGTH_BITS) {
		l_put_be16(dlen * 8, buf + pos);
		pos += 2;
	} /* else no padding */

	if (data)
		memcpy(buf + pos, data, dlen);
	else
		memset(buf + pos, 0, dlen);

	pad = (buf[1] * 4) - (dlen + pos);
	pos += dlen;
	/* If header + data is not in multiple of 4 bytes then pad */
	for (i = 0; i < pad; i++)
		buf[pos + i] = 0x00;

	pos += pad;
	return pos;
}

bool eap_sim_verify_mac(struct eap_state *eap, enum eap_type type,
		const uint8_t *buf, uint16_t len, uint8_t *k_aut,
		uint8_t *extra, size_t elen)
{
	struct l_checksum *hmac;
	struct eap_sim_tlv_iter iter;
	const uint8_t *mac_p = NULL;
	uint8_t zero_mac[EAP_SIM_MAC_LEN] = { 0 };
	uint8_t hdr[5];
	struct iovec iov[4];

	eap_sim_tlv_iter_init(&iter, buf + 3, len - 3);

	while (eap_sim_tlv_iter_next(&iter)) {
		if (eap_sim_tlv_iter_get_type(&iter) == EAP_SIM_AT_MAC) {
			mac_p = eap_sim_tlv_iter_get_data(&iter) + 2;
			break;
		}
	}

	if (!mac_p) {
		l_error("packet did not contain AT_MAC attribute");
		return false;
	}

	/* re-build EAP packet header */
	hdr[0] = 0x01;
	eap_save_last_id(eap, &hdr[1]);
	l_put_be16(len + 5, hdr + 2);
	hdr[4] = type;

	iov[0].iov_base = (void *)hdr;
	iov[0].iov_len = 5;
	iov[1].iov_base = (void *)buf;
	iov[1].iov_len = len - EAP_SIM_MAC_LEN;
	iov[2].iov_base = zero_mac;
	iov[2].iov_len = EAP_SIM_MAC_LEN;
	iov[3].iov_base = extra;
	iov[3].iov_len = elen;

	hmac = l_checksum_new_hmac(L_CHECKSUM_SHA1, k_aut, EAP_SIM_K_AUT_LEN);
	l_checksum_updatev(hmac, iov, 4);
	/* reuse zero mac array for new mac */
	l_checksum_get_digest(hmac, zero_mac, EAP_SIM_MAC_LEN);
	l_checksum_free(hmac);

	if (memcmp(zero_mac, mac_p, EAP_SIM_MAC_LEN)) {
		l_error("MAC does not match");
		return false;
	}

	return true;
}

bool eap_sim_tlv_iter_init(struct eap_sim_tlv_iter *iter, const uint8_t *data,
		uint32_t len)
{
	iter->data = NULL;
	iter->pos = data;
	iter->len = 0;
	iter->end = data + len;
	return true;
}

bool eap_sim_tlv_iter_next(struct eap_sim_tlv_iter *iter)
{
	/* check room for tag/len */
	if (iter->end - iter->pos < 2)
		return false;

	iter->tag = iter->pos[0];
	iter->len = (iter->pos[1] * 4) - 2;
	iter->pos += 2;

	/* check room for value */
	if (iter->end - iter->pos < iter->len)
		return false;

	iter->data = iter->pos;
	iter->pos += iter->len;

	return true;
}

uint8_t eap_sim_tlv_iter_get_type(struct eap_sim_tlv_iter *iter)
{
	return iter->tag;
}

uint16_t eap_sim_tlv_iter_get_length(struct eap_sim_tlv_iter *iter)
{
	return iter->len;
}

const void *eap_sim_tlv_iter_get_data(struct eap_sim_tlv_iter *iter)
{
	return iter->data;
}