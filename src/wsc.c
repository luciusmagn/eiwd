/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2015-2019  Intel Corporation. All rights reserved.
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

#include <errno.h>
#include <stdio.h>
#include <ell/ell.h>

#include "src/missing.h"
#include "src/module.h"
#include "src/netdev.h"
#include "src/wiphy.h"
#include "src/station.h"
#include "src/scan.h"
#include "src/ie.h"
#include "src/wscutil.h"
#include "src/util.h"
#include "src/handshake.h"
#include "src/eap-wsc.h"
#include "src/crypto.h"
#include "src/common.h"
#include "src/storage.h"
#include "src/iwd.h"
#include "src/network.h"

#define WALK_TIME 120

static uint32_t netdev_watch = 0;

struct wsc {
	struct netdev *netdev;
	struct station *station;
	struct l_dbus_message *pending;
	struct l_dbus_message *pending_cancel;
	uint8_t *wsc_ies;
	size_t wsc_ies_size;
	struct l_timeout *walk_timer;
	uint32_t scan_id;
	struct scan_bss *target;
	uint32_t station_state_watch;
	struct {
		char ssid[33];
		enum security security;
		union {
			uint8_t psk[32];
			char passphrase[64];
		};
		uint8_t addr[6];
		bool has_passphrase;
	} creds[3];
	uint32_t n_creds;
	struct l_settings *eap_settings;

	bool wsc_association : 1;
};

static void wsc_try_credentials(struct wsc *wsc)
{
	unsigned int i;
	struct network *network;
	struct scan_bss *bss;

	for (i = 0; i < wsc->n_creds; i++) {
		network = station_network_find(wsc->station,
						wsc->creds[i].ssid,
						wsc->creds[i].security);
		if (!network)
			continue;

		bss = network_bss_find_by_addr(network, wsc->creds[i].addr);

		if (!bss)
			bss = network_bss_select(network, true);

		if (!bss)
			continue;

		if (wsc->creds[i].security == SECURITY_PSK) {
			bool ret;

			/*
			 * Prefer setting passphrase, this will work for both
			 * WPA2 and WPA3 since the PSK can always be generated
			 * if needed
			 */
			if (wsc->creds[i].has_passphrase)
				ret = network_set_passphrase(network,
						wsc->creds[i].passphrase);
			else
				ret = network_set_psk(network,
						wsc->creds[i].psk);

			if (!ret)
				continue;
		}

		station_connect_network(wsc->station, network, bss);
		wsc->pending = NULL;

		goto done;
	}

	station_set_autoconnect(wsc->station, true);
done:
	memset(wsc->creds, 0, sizeof(wsc->creds));
	wsc->n_creds = 0;
}

static void wsc_store_credentials(struct wsc *wsc)
{
	unsigned int i;

	for (i = 0; i < wsc->n_creds; i++) {
		enum security security = wsc->creds[i].security;
		const char *ssid = wsc->creds[i].ssid;
		struct l_settings *settings = l_settings_new();

		l_debug("Storing credential for '%s(%s)'", ssid,
						security_to_str(security));

		if (security == SECURITY_PSK) {
			char *hex = l_util_hexstring(wsc->creds[i].psk,
						sizeof(wsc->creds[i].psk));

			l_settings_set_value(settings, "Security",
							"PreSharedKey", hex);
			explicit_bzero(hex, strlen(hex));
			l_free(hex);
		}

		storage_network_sync(security, ssid, settings);
		l_settings_free(settings);

		/*
		 * TODO: Mark this network as known.  We might be getting
		 * multiple credentials from WSC, so there is a possibility
		 * that the network is not known and / or not in scan results.
		 * In both cases, the network should be considered for
		 * auto-connect.  Note, since we sync the settings, the next
		 * reboot will put the network on the known list.
		 */
	}
}

static void wsc_connect_cb(struct netdev *netdev, enum netdev_result result,
					void *event_data, void *user_data)
{
	struct wsc *wsc = user_data;

	l_debug("%d, result: %d", netdev_get_ifindex(wsc->netdev), result);

	wsc->wsc_association = false;

	l_settings_free(wsc->eap_settings);
	wsc->eap_settings = NULL;

	if (result == NETDEV_RESULT_HANDSHAKE_FAILED && wsc->n_creds > 0) {
		wsc_store_credentials(wsc);
		wsc_try_credentials(wsc);
		return;
	}

	switch (result) {
	case NETDEV_RESULT_ABORTED:
		return;
	case NETDEV_RESULT_HANDSHAKE_FAILED:
		break;
	default:
		break;
	}

	station_set_autoconnect(wsc->station, true);
}

static void wsc_credential_obtained(struct wsc *wsc,
					const struct wsc_credential *cred)
{
	uint16_t auth_mask;
	unsigned int i;

	l_debug("Obtained credenials for SSID: %s, address: %s",
			util_ssid_to_utf8(cred->ssid_len, cred->ssid),
			util_address_to_string(cred->addr));

	l_debug("auth_type: %02x, encryption_type: %02x",
			cred->auth_type, cred->encryption_type);

	if (getenv("IWD_WSC_DEBUG_KEYS"))
		l_debug("Key (%u): %.*s", cred->network_key_len,
				cred->network_key_len, cred->network_key);

	if (wsc->n_creds == L_ARRAY_SIZE(wsc->creds)) {
		l_warn("Maximum number of credentials obtained, ignoring...");
		return;
	}

	if (!util_ssid_is_utf8(cred->ssid_len, cred->ssid)) {
		l_warn("Ignoring Credentials with non-UTF8 SSID");
		return;
	}

	memcpy(wsc->creds[wsc->n_creds].ssid, cred->ssid, cred->ssid_len);
	wsc->creds[wsc->n_creds].ssid[cred->ssid_len] = '\0';

	/* We only support open/personal wpa/personal wpa2 */
	auth_mask = WSC_AUTHENTICATION_TYPE_OPEN |
			WSC_AUTHENTICATION_TYPE_WPA_PERSONAL |
			WSC_AUTHENTICATION_TYPE_WPA2_PERSONAL;
	if ((cred->auth_type & auth_mask) == 0) {
		l_warn("Ignoring Credentials with unsupported auth_type");
		return;
	}

	if (cred->auth_type & WSC_AUTHENTICATION_TYPE_OPEN) {
		auth_mask &= ~WSC_AUTHENTICATION_TYPE_OPEN;

		if (cred->auth_type & auth_mask) {
			l_warn("Ignoring mixed open/wpa credentials");
			return;
		}

		wsc->creds[wsc->n_creds].security = SECURITY_NONE;
	} else
		wsc->creds[wsc->n_creds].security = SECURITY_PSK;

	switch (wsc->creds[wsc->n_creds].security) {
	case SECURITY_NONE:
		if (cred->network_key_len != 0) {
			l_warn("ignoring invalid open key length");
			return;
		}

		break;
	case SECURITY_PSK:
		if (cred->network_key_len == 64) {
			unsigned char *decoded;
			const char *hex = (const char *) cred->network_key;

			decoded = l_util_from_hexstring(hex, NULL);
			if (!decoded) {
				l_warn("Ignoring non-hex network_key");
				return;
			}

			memcpy(wsc->creds[wsc->n_creds].psk, decoded, 32);
			explicit_bzero(decoded, 32);
			l_free(decoded);
		} else {
			strncpy(wsc->creds[wsc->n_creds].passphrase,
					(const char *) cred->network_key,
					cred->network_key_len);
			wsc->creds[wsc->n_creds].has_passphrase = true;
		}

		break;
	default:
		return;
	}

	for (i = 0; i < wsc->n_creds; i++) {
		if (strcmp(wsc->creds[i].ssid, wsc->creds[wsc->n_creds].ssid))
			continue;

		l_warn("Found duplicate credentials for SSID: %s",
				wsc->creds[i].ssid);
		explicit_bzero(&wsc->creds[wsc->n_creds],
				sizeof(wsc->creds[wsc->n_creds]));
		return;
	}

	memcpy(wsc->creds[wsc->n_creds].addr, cred->addr, 6);
	wsc->n_creds += 1;
}

static inline enum wsc_rf_band freq_to_rf_band(uint32_t freq)
{
	enum scan_band band;

	scan_freq_to_channel(freq, &band);

	switch (band) {
	case SCAN_BAND_2_4_GHZ:
		return WSC_RF_BAND_2_4_GHZ;
	case SCAN_BAND_5_GHZ:
		return WSC_RF_BAND_5_0_GHZ;
	}

	return WSC_RF_BAND_2_4_GHZ;
}

static void wsc_add_interface(struct netdev *netdev)
{
	struct wsc *wsc;

	if (!wiphy_get_max_scan_ie_len(netdev_get_wiphy(netdev))) {
		l_debug("Simple Configuration isn't supported by ifindex %u",
						netdev_get_ifindex(netdev));

		return;
	}

	wsc = l_new(struct wsc, 1);
	wsc->netdev = netdev;
}

static void wsc_remove_interface(struct netdev *netdev)
{
}

static void wsc_netdev_watch(struct netdev *netdev,
				enum netdev_watch_event event, void *userdata)
{
	switch (event) {
	case NETDEV_WATCH_EVENT_UP:
	case NETDEV_WATCH_EVENT_NEW:
		if (netdev_get_iftype(netdev) == NETDEV_IFTYPE_STATION &&
				netdev_get_is_up(netdev))
			wsc_add_interface(netdev);
		break;
	case NETDEV_WATCH_EVENT_DOWN:
	case NETDEV_WATCH_EVENT_DEL:
		wsc_remove_interface(netdev);
		break;
	default:
		break;
	}
}

static int wsc_init(void)
{
	l_debug("");
	netdev_watch = netdev_watch_add(wsc_netdev_watch, NULL, NULL);
	return 0;
}

static void wsc_exit(void)
{
	l_debug("");
	netdev_watch_remove(netdev_watch);
}

IWD_MODULE(wsc, wsc_init, wsc_exit)
