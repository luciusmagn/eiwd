/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ell/ell.h>

#include "src/eapol.h"

struct eapol_key_data {
	const unsigned char *frame;
	size_t frame_len;
	enum eapol_protocol_version protocol_version;
	uint16_t packet_len;
	enum eapol_descriptor_type descriptor_type;
	enum eapol_key_descriptor_version key_descriptor_version;
	bool key_type:1;
	bool install:1;
	bool key_ack:1;
	bool key_mic:1;
	bool secure:1;
	bool error:1;
	bool request:1;
	bool encrypted_key_data:1;
	bool smk_message:1;
	uint16_t key_length;
	uint8_t key_replay_counter[8];
	uint8_t key_nonce[32];
	uint8_t eapol_key_iv[16];
	uint8_t key_rsc[8];
	uint8_t key_mic_data[16];
	uint16_t key_data_len;
};

static const unsigned char eapol_key_data_1[] = {
	0x01, 0x03, 0x00, 0x5f, 0xfe, 0x00, 0x89, 0x00, 0x20, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0xd5, 0xe2, 0x13, 0x9b, 0x1b, 0x1c, 0x1e,
	0xcb, 0xf4, 0xc7, 0x9d, 0xb3, 0x70, 0xcd, 0x1c, 0xea, 0x07, 0xf1, 0x61,
	0x76, 0xed, 0xa6, 0x78, 0x8a, 0xc6, 0x8c, 0x2c, 0xf4, 0xd7, 0x6f, 0x2b,
	0xf7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00,
};

static struct eapol_key_data eapol_key_test_1 = {
	.frame = eapol_key_data_1,
	.frame_len = sizeof(eapol_key_data_1),
	.protocol_version = EAPOL_PROTOCOL_VERSION_2001,
	.packet_len = 95,
	.descriptor_type = EAPOL_DESCRIPTOR_TYPE_WPA,
	.key_descriptor_version = EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4,
	.key_type = true,
	.install = false,
	.key_ack = true,
	.key_mic = false,
	.secure = false,
	.error = false,
	.request = false,
	.encrypted_key_data = false,
	.smk_message = false,
	.key_length = 32,
	.key_replay_counter =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
	.key_nonce = { 0xd5, 0xe2, 0x13, 0x9b, 0x1b, 0x1c, 0x1e, 0xcb, 0xf4,
			0xc7, 0x9d, 0xb3, 0x70, 0xcd, 0x1c, 0xea, 0x07, 0xf1,
			0x61, 0x76, 0xed, 0xa6, 0x78, 0x8a, 0xc6, 0x8c, 0x2c,
			0xf4, 0xd7, 0x6f, 0x2b, 0xf7 },
	.eapol_key_iv = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_rsc = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_mic_data = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_data_len = 0,
};

static const unsigned char eapol_key_data_2[] = {
	0x02, 0x03, 0x00, 0x75, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x6a, 0xce, 0x64, 0xc1, 0xa6, 0x44,
	0xd2, 0x7b, 0x84, 0xe0, 0x39, 0x26, 0x3b, 0x63, 0x3b, 0xc3, 0x74, 0xe3,
	0x29, 0x9d, 0x7d, 0x45, 0xe1, 0xc4, 0x25, 0x44, 0x05, 0x48, 0x05, 0xbf,
	0xe5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x16, 0xdd, 0x14, 0x00, 0x0f, 0xac, 0x04, 0x05, 0xb1, 0xb6,
	0x8b, 0x5a, 0x91, 0xfc, 0x04, 0x06, 0x83, 0x84, 0x06, 0xe8, 0xd1, 0x5f,
	0xdb,
};

static struct eapol_key_data eapol_key_test_2 = {
	.frame = eapol_key_data_2,
	.frame_len = sizeof(eapol_key_data_2),
	.protocol_version = EAPOL_PROTOCOL_VERSION_2004,
	.packet_len = 117,
	.descriptor_type = EAPOL_DESCRIPTOR_TYPE_80211,
	.key_descriptor_version = EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES,
	.key_type = true,
	.install = false,
	.key_ack = true,
	.key_mic = false,
	.secure = false,
	.error = false,
	.request = false,
	.encrypted_key_data = false,
	.smk_message = false,
	.key_length = 16,
	.key_replay_counter =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_nonce = { 0x12, 0x6a, 0xce, 0x64, 0xc1, 0xa6, 0x44, 0xd2, 0x7b,
			0x84, 0xe0, 0x39, 0x26, 0x3b, 0x63, 0x3b, 0xc3, 0x74,
			0xe3, 0x29, 0x9d, 0x7d, 0x45, 0xe1, 0xc4, 0x25, 0x44,
			0x05, 0x48, 0x05, 0xbf, 0xe5 },
	.eapol_key_iv = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_rsc = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_mic_data = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_data_len = 22,
};

static const unsigned char eapol_key_data_3[] = {
	0x02, 0x03, 0x00, 0x5f, 0x02, 0x00, 0x8a, 0x00, 0x10, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xc2, 0xbb, 0x57, 0xab, 0x58, 0x8f, 0x92,
	0xeb, 0xbd, 0x44, 0xe8, 0x11, 0x09, 0x4f, 0x60, 0x1c, 0x08, 0x79, 0x86,
	0x03, 0x0c, 0x3a, 0xc7, 0x49, 0xcc, 0x61, 0xd6, 0x3e, 0x33, 0x83, 0x2e,
	0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00
};

static struct eapol_key_data eapol_key_test_3 = {
	.frame = eapol_key_data_3,
	.frame_len = sizeof(eapol_key_data_3),
	.protocol_version = EAPOL_PROTOCOL_VERSION_2004,
	.packet_len = 95,
	.descriptor_type = EAPOL_DESCRIPTOR_TYPE_80211,
	.key_descriptor_version = EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES,
	.key_type = true,
	.install = false,
	.key_ack = true,
	.key_mic = false,
	.secure = false,
	.error = false,
	.request = false,
	.encrypted_key_data = false,
	.smk_message = false,
	.key_length = 16,
	.key_replay_counter =
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_nonce = { 0xc2, 0xbb, 0x57, 0xab, 0x58, 0x8f, 0x92, 0xeb, 0xbd,
			0x44, 0xe8, 0x11, 0x09, 0x4f, 0x60, 0x1c, 0x08, 0x79,
			0x86, 0x03, 0x0c, 0x3a, 0xc7, 0x49, 0xcc, 0x61, 0xd6,
			0x3e, 0x33, 0x83, 0x2e, 0x50, },
	.eapol_key_iv = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_rsc = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_mic_data = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	.key_data_len = 0,
};

static void eapol_key_test(const void *data)
{
	const struct eapol_key_data *test = data;
	struct eapol_key *packet;

	packet = (struct eapol_key *)test->frame;

	assert(packet->protocol_version == test->protocol_version);
	assert(packet->packet_type == 0x03);
	assert(L_BE16_TO_CPU(packet->packet_len) == test->packet_len);
	assert(packet->descriptor_type == test->descriptor_type);
	assert(packet->key_descriptor_version == test->key_descriptor_version);
	assert(packet->key_type == test->key_type);
	assert(packet->install == test->install);
	assert(packet->key_ack == test->key_ack);
	assert(packet->key_mic == test->key_mic);
	assert(packet->secure == test->secure);
	assert(packet->error == test->error);
	assert(packet->request == test->request);
	assert(packet->encrypted_key_data == test->encrypted_key_data);
	assert(packet->smk_message == test->smk_message);
	assert(L_BE16_TO_CPU(packet->key_length) == test->key_length);
	assert(!memcmp(packet->key_replay_counter, test->key_replay_counter,
			sizeof(packet->key_replay_counter)));
	assert(!memcmp(packet->key_nonce, test->key_nonce,
			sizeof(packet->key_nonce)));
	assert(!memcmp(packet->eapol_key_iv, test->eapol_key_iv,
			sizeof(packet->eapol_key_iv)));
	assert(!memcmp(packet->key_mic_data, test->key_mic_data,
			sizeof(packet->key_mic_data)));
	assert(!memcmp(packet->key_rsc, test->key_rsc,
			sizeof(packet->key_rsc)));
	assert(L_BE16_TO_CPU(packet->key_data_len) == test->key_data_len);
}

int main(int argc, char *argv[])
{
	l_test_init(&argc, &argv);

	l_test_add("/EAPoL Key/Key Frame 1",
			eapol_key_test, &eapol_key_test_1);
	l_test_add("/EAPoL Key/Key Frame 2",
			eapol_key_test, &eapol_key_test_2);
	l_test_add("/EAPoL Key/Key Frame 3",
			eapol_key_test, &eapol_key_test_3);

	return l_test_run();
}
