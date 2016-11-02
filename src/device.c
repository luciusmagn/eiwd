/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2013-2016  Intel Corporation. All rights reserved.
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
#include <errno.h>
#include <time.h>

#include <ell/ell.h>

#include "src/iwd.h"
#include "src/common.h"
#include "src/util.h"
#include "src/ie.h"
#include "src/eapol.h"
#include "src/wiphy.h"
#include "src/scan.h"
#include "src/netdev.h"
#include "src/dbus.h"
#include "src/network.h"
#include "src/device.h"
#include "src/watchlist.h"

struct device_watchlist_item {
	uint32_t id;
	device_watch_func_t added;
	device_watch_func_t removed;
	void *userdata;
	device_destroy_func_t destroy;
};

struct autoconnect_entry {
	uint16_t rank;
	struct network *network;
	struct scan_bss *bss;
};

struct device {
	uint32_t index;
	enum device_state state;
	struct l_queue *bss_list;
	struct l_queue *old_bss_list;
	struct l_dbus_message *scan_pending;
	struct l_hashmap *networks;
	struct l_queue *networks_sorted;
	struct scan_bss *connected_bss;
	struct network *connected_network;
	struct l_queue *autoconnect_list;
	struct l_dbus_message *connect_pending;
	struct l_dbus_message *disconnect_pending;
	uint32_t netdev_watch_id;
	struct watchlist state_watches;

	struct wiphy *wiphy;
	struct netdev *netdev;

	bool scanning : 1;
	bool autoconnect : 1;
};

static struct watchlist device_watches;
static struct l_queue *device_list;

uint32_t device_watch_add(device_watch_func_t func,
				void *userdata, device_destroy_func_t destroy)
{
	return watchlist_add(&device_watches, func, userdata, destroy);
}

bool device_watch_remove(uint32_t id)
{
	return watchlist_remove(&device_watches, id);
}

void __iwd_device_foreach(iwd_device_foreach_func func, void *user_data)
{
	const struct l_queue_entry *device_entry;

	for (device_entry = l_queue_get_entries(device_list); device_entry;
					device_entry = device_entry->next) {
		struct device *device = device_entry->data;

		func(device, user_data);
	}
}

static const char *iwd_network_get_path(struct device *device,
					const char *ssid,
					enum security security)
{
	static char path[256];
	unsigned int pos, i;

	pos = snprintf(path, sizeof(path), "%s/", device_get_path(device));

	for (i = 0; ssid[i] && pos < sizeof(path); i++)
		pos += snprintf(path + pos, sizeof(path) - pos, "%02x",
								ssid[i]);

	snprintf(path + pos, sizeof(path) - pos, "_%s",
				security_to_str(security));

	return path;
}

static const char *device_state_to_string(enum device_state state)
{
	switch (state) {
	case DEVICE_STATE_OFF:
		return "off";
	case DEVICE_STATE_DISCONNECTED:
		return "disconnected";
	case DEVICE_STATE_AUTOCONNECT:
		return "autoconnect";
	case DEVICE_STATE_CONNECTING:
		return "connecting";
	case DEVICE_STATE_CONNECTED:
		return "connected";
	case DEVICE_STATE_DISCONNECTING:
		return "disconnecting";
	}

	return "invalid";
}

static void device_autoconnect_next(struct device *device)
{
	struct autoconnect_entry *entry;
	int r;

	while ((entry = l_queue_pop_head(device->autoconnect_list))) {
		l_debug("Considering autoconnecting to BSS '%s' with SSID: %s,"
			" freq: %u, rank: %u, strength: %i",
			util_address_to_string(entry->bss->addr),
			network_get_ssid(entry->network),
			entry->bss->frequency, entry->rank,
			entry->bss->signal_strength);

		/* TODO: Blacklist the network from auto-connect */
		r = network_autoconnect(entry->network, entry->bss);
		l_free(entry);

		if (!r)
			return;
	}
}

static void bss_free(void *data)
{
	struct scan_bss *bss = data;
	const char *addr;

	addr = util_address_to_string(bss->addr);
	l_debug("Freeing BSS %s", addr);

	scan_bss_free(bss);
}

static void network_free(void *data)
{
	struct network *network = data;

	network_remove(network, -ESHUTDOWN);
}

static int autoconnect_rank_compare(const void *a, const void *b, void *user)
{
	const struct autoconnect_entry *new_ae = a;
	const struct autoconnect_entry *ae = b;

	return ae->rank - new_ae->rank;
}

static bool process_network(const void *key, void *data, void *user_data)
{
	struct network *network = data;
	struct device *device = user_data;

	if (!network_bss_list_isempty(network)) {
		/* Build the network list ordered by rank */
		network_rank_update(network);

		l_queue_insert(device->networks_sorted, network,
				network_rank_compare, NULL);

		return false;
	}

	/* Drop networks that have no more BSSs in range */
	l_debug("No remaining BSSs for SSID: %s -- Removing network",
			network_get_ssid(network));
	network_remove(network, -ERANGE);

	return true;
}

static void process_bss(struct device *device, struct scan_bss *bss,
			struct timespec *timestamp)
{
	struct network *network;
	struct ie_rsn_info info;
	int r;
	enum security security;
	const char *path;
	double rankmod;
	struct autoconnect_entry *entry;
	char ssid[33];

	l_debug("Found BSS '%s' with SSID: %s, freq: %u, rank: %u, "
			"strength: %i",
			util_address_to_string(bss->addr),
			util_ssid_to_utf8(bss->ssid_len, bss->ssid),
			bss->frequency, bss->rank, bss->signal_strength);

	if (!util_ssid_is_utf8(bss->ssid_len, bss->ssid)) {
		l_warn("Ignoring BSS with non-UTF8 SSID");
		return;
	}

	memcpy(ssid, bss->ssid, bss->ssid_len);
	ssid[bss->ssid_len] = '\0';

	memset(&info, 0, sizeof(info));
	r = scan_bss_get_rsn_info(bss, &info);
	if (r < 0) {
		if (r != -ENOENT)
			return;

		security = scan_get_security(bss->capability, NULL);
	} else
		security = scan_get_security(bss->capability, &info);

	if (security == SECURITY_PSK)
		bss->sha256 = info.akm_suites & IE_RSN_AKM_SUITE_PSK_SHA256;
	else if (security == SECURITY_8021X)
		bss->sha256 = info.akm_suites & IE_RSN_AKM_SUITE_8021X_SHA256;

	path = iwd_network_get_path(device, ssid, security);

	network = l_hashmap_lookup(device->networks, path);
	if (!network) {
		network = network_create(device, ssid, security);

		if (!network_register(network, path)) {
			network_remove(network, -EINVAL);
			return;
		}

		l_hashmap_insert(device->networks,
					network_get_path(network), network);
		l_debug("Added new Network \"%s\" security %s",
			network_get_ssid(network), security_to_str(security));
	}

	if (network_bss_list_isempty(network))
		network_seen(network, timestamp);

	network_bss_add(network, bss);

	if (device->state != DEVICE_STATE_AUTOCONNECT)
		return;

	/* See if network is autoconnectable (is a known network) */
	if (!network_rankmod(network, &rankmod))
		return;

	entry = l_new(struct autoconnect_entry, 1);
	entry->network = network;
	entry->bss = bss;
	entry->rank = bss->rank * rankmod;
	l_queue_insert(device->autoconnect_list, entry,
				autoconnect_rank_compare, NULL);
}

static bool bss_match(const void *a, const void *b)
{
	const struct scan_bss *bss_a = a;
	const struct scan_bss *bss_b = b;

	return !memcmp(bss_a->addr, bss_b->addr, sizeof(bss_a->addr));
}

/*
 * Used when scan results were obtained; either from passive scan running
 * inside device.c or active scans running in other state machines, e.g. wsc.c
 */
void device_set_scan_results(struct device *device, struct l_queue *bss_list)
{
	struct network *network;
	const struct l_queue_entry *bss_entry;
	struct timespec now;
	struct l_dbus *dbus = dbus_get_bus();

	clock_gettime(CLOCK_REALTIME, &now);

	device->scanning = false;
	l_dbus_property_changed(dbus, device_get_path(device),
				IWD_DEVICE_INTERFACE, "Scanning");

	device->old_bss_list = device->bss_list;
	device->bss_list = bss_list;

	while ((network = l_queue_pop_head(device->networks_sorted)))
		network_bss_list_clear(network);

	l_queue_destroy(device->autoconnect_list, l_free);
	device->autoconnect_list = l_queue_new();

	for (bss_entry = l_queue_get_entries(bss_list); bss_entry;
				bss_entry = bss_entry->next) {
		struct scan_bss *bss = bss_entry->data;

		process_bss(device, bss, &now);
	}

	if (device->connected_bss) {
		struct scan_bss *bss;

		bss = l_queue_find(device->bss_list, bss_match,
						device->connected_bss);

		if (!bss) {
			l_warn("Connected BSS not in scan results!");
			l_queue_push_tail(device->bss_list,
						device->connected_bss);
			network_bss_add(device->connected_network,
						device->connected_bss);
			l_queue_remove(device->old_bss_list,
						device->connected_bss);
		} else
			device->connected_bss = bss;
	}

	l_hashmap_foreach_remove(device->networks, process_network, device);

	l_queue_destroy(device->old_bss_list, bss_free);
	device->old_bss_list = NULL;

	if (device->state == DEVICE_STATE_AUTOCONNECT)
		device_autoconnect_next(device);
}

static bool new_scan_results(uint32_t wiphy_id, uint32_t ifindex,
				struct l_queue *bss_list, void *userdata)
{
	struct device *device = userdata;

	device_set_scan_results(device, bss_list);
	return true;
}

struct network *device_get_connected_network(struct device *device)
{
	return device->connected_network;
}

const char *device_get_path(struct device *device)
{
	static char path[26];

	snprintf(path, sizeof(path), "%s/%u", wiphy_get_path(device->wiphy),
			device->index);
	return path;
}

bool device_is_busy(struct device *device)
{
	if (device->state != DEVICE_STATE_DISCONNECTED &&
			device->state != DEVICE_STATE_AUTOCONNECT &&
			device->state != DEVICE_STATE_OFF)
		return true;

	return false;
}

struct wiphy *device_get_wiphy(struct device *device)
{
	return device->wiphy;
}

struct netdev *device_get_netdev(struct device *device)
{
	return device->netdev;
}

uint32_t device_get_ifindex(struct device *device)
{
	return device->index;
}

const uint8_t *device_get_address(struct device *device)
{
	return netdev_get_address(device->netdev);
}

enum device_state device_get_state(struct device *device)
{
	return device->state;
}

static void periodic_scan_trigger(int err, void *user_data)
{
	struct device *device = user_data;
	struct l_dbus *dbus = dbus_get_bus();

	device->scanning = true;
	l_dbus_property_changed(dbus, device_get_path(device),
				IWD_DEVICE_INTERFACE, "Scanning");
}

uint32_t device_add_state_watch(struct device *device,
					device_state_watch_func_t func,
					void *user_data,
					device_destroy_func_t destroy)
{
	return watchlist_add(&device->state_watches, func, user_data, destroy);
}

bool device_remove_state_watch(struct device *device, uint32_t id)
{
	return watchlist_remove(&device->state_watches, id);
}

struct network *device_network_find(struct device *device, const char *ssid,
					enum security security)
{
	const char *path = iwd_network_get_path(device, ssid, security);

	return l_hashmap_lookup(device->networks, path);
}

static void device_enter_state(struct device *device, enum device_state state)
{
	struct l_dbus *dbus = dbus_get_bus();
	bool disconnected;

	l_debug("Old State: %s, new state: %s",
			device_state_to_string(device->state),
			device_state_to_string(state));

	switch (state) {
	case DEVICE_STATE_OFF:
		scan_periodic_stop(device->index);
		break;
	case DEVICE_STATE_AUTOCONNECT:
		scan_periodic_start(device->index, periodic_scan_trigger,
					new_scan_results, device);
		break;
	case DEVICE_STATE_DISCONNECTED:
		scan_periodic_stop(device->index);
		break;
	case DEVICE_STATE_CONNECTED:
		scan_periodic_stop(device->index);
		break;
	case DEVICE_STATE_CONNECTING:
		break;
	case DEVICE_STATE_DISCONNECTING:
		break;
	}

	disconnected = device->state <= DEVICE_STATE_AUTOCONNECT;

	if ((disconnected && state > DEVICE_STATE_AUTOCONNECT) ||
			(!disconnected && state != device->state))
		l_dbus_property_changed(dbus, device_get_path(device),
					IWD_DEVICE_INTERFACE, "State");

	device->state = state;

	WATCHLIST_NOTIFY(&device->state_watches,
					device_state_watch_func_t, state);
}

static void device_disassociated(struct device *device)
{
	struct network *network = device->connected_network;
	struct l_dbus *dbus = dbus_get_bus();

	l_debug("%d", device->index);

	if (network) {
		if (device->state == DEVICE_STATE_CONNECTED)
			network_disconnected(network);

		device->connected_bss = NULL;
		device->connected_network = NULL;

		l_dbus_property_changed(dbus, device_get_path(device),
					IWD_DEVICE_INTERFACE,
					"ConnectedNetwork");
		l_dbus_property_changed(dbus, network_get_path(network),
					IWD_NETWORK_INTERFACE, "Connected");
	}

	device_enter_state(device, DEVICE_STATE_DISCONNECTED);

	if (device->autoconnect)
		device_enter_state(device, DEVICE_STATE_AUTOCONNECT);
}

static void device_lost_beacon(struct device *device)
{
	l_debug("%d", device->index);

	if (device->connect_pending)
		dbus_pending_reply(&device->connect_pending,
				dbus_error_failed(device->connect_pending));

	device_disassociated(device);
}

static void device_disconnect_by_ap(struct device *device)
{
	l_debug("%d", device->index);

	if (device->connect_pending) {
		struct network *network = device->connected_network;

		dbus_pending_reply(&device->connect_pending,
				dbus_error_failed(device->connect_pending));

		network_connect_failed(network);
	}

	device_disassociated(device);
}

static void device_connect_cb(struct netdev *netdev, enum netdev_result result,
					void *user_data)
{
	struct device *device = user_data;

	l_debug("%d, result: %d", device->index, result);

	if (device->connect_pending) {
		struct l_dbus_message *reply;

		switch (result) {
		case NETDEV_RESULT_ABORTED:
			reply = dbus_error_aborted(device->connect_pending);
			break;
		case NETDEV_RESULT_OK:
			reply = l_dbus_message_new_method_return(
						device->connect_pending);
			l_dbus_message_set_arguments(reply, "");
			break;
		default:
			reply = dbus_error_failed(device->connect_pending);
			break;
		}

		dbus_pending_reply(&device->connect_pending, reply);
	}

	if (result != NETDEV_RESULT_OK) {
		if (result != NETDEV_RESULT_ABORTED) {
			network_connect_failed(device->connected_network);
			device_disassociated(device);
		}

		return;
	}

	network_connected(device->connected_network);
	device_enter_state(device, DEVICE_STATE_CONNECTED);
	device->autoconnect = true;
}

static void device_netdev_event(struct netdev *netdev, enum netdev_event event,
					void *user_data)
{
	struct device *device = user_data;
	struct network *network = device->connected_network;

	switch (event) {
	case NETDEV_EVENT_AUTHENTICATING:
		l_debug("Authenticating");
		break;
	case NETDEV_EVENT_ASSOCIATING:
		l_debug("Associating");
		break;
	case NETDEV_EVENT_4WAY_HANDSHAKE:
		l_debug("Handshaking");
		break;
	case NETDEV_EVENT_SETTING_KEYS:
		l_debug("Setting keys");

		/* If we got here, then our PSK works.  Save if required */
		network_sync_psk(network);

		break;
	case NETDEV_EVENT_LOST_BEACON:
		device_lost_beacon(device);
		break;
	case NETDEV_EVENT_DISCONNECT_BY_AP:
		device_disconnect_by_ap(device);
	};
}

bool device_set_autoconnect(struct device *device, bool autoconnect)
{
	if (device->autoconnect == autoconnect)
		return true;

	device->autoconnect = autoconnect;

	if (device->state == DEVICE_STATE_DISCONNECTED && autoconnect)
		device_enter_state(device, DEVICE_STATE_AUTOCONNECT);

	if (device->state == DEVICE_STATE_AUTOCONNECT && !autoconnect)
		device_enter_state(device, DEVICE_STATE_DISCONNECTED);

	return true;
}

void device_connect_network(struct device *device, struct network *network,
				struct scan_bss *bss,
				struct l_dbus_message *message)
{
	enum security security = network_get_security(network);
	struct wiphy *wiphy = device->wiphy;
	struct l_dbus *dbus = dbus_get_bus();
	struct eapol_sm *sm = NULL;

	if (security == SECURITY_PSK || security == SECURITY_8021X) {
		struct ie_rsn_info bss_info;
		uint8_t rsne_buf[256];
		struct ie_rsn_info info;

		sm = eapol_sm_new();

		eapol_sm_set_authenticator_address(sm, bss->addr);
		eapol_sm_set_supplicant_address(sm,
				netdev_get_address(device->netdev));

		memset(&info, 0, sizeof(info));

		if (security == SECURITY_PSK)
			info.akm_suites =
				bss->sha256 ? IE_RSN_AKM_SUITE_PSK_SHA256 :
						IE_RSN_AKM_SUITE_PSK;
		else
			info.akm_suites =
				bss->sha256 ? IE_RSN_AKM_SUITE_8021X_SHA256 :
						IE_RSN_AKM_SUITE_8021X;

		memset(&bss_info, 0, sizeof(bss_info));
		scan_bss_get_rsn_info(bss, &bss_info);

		info.pairwise_ciphers = wiphy_select_cipher(wiphy,
						bss_info.pairwise_ciphers);
		info.group_cipher = wiphy_select_cipher(wiphy,
						bss_info.group_cipher);

		/* RSN takes priority */
		if (bss->rsne) {
			ie_build_rsne(&info, rsne_buf);
			eapol_sm_set_ap_rsn(sm, bss->rsne);
			eapol_sm_set_own_rsn(sm, rsne_buf);
		} else {
			ie_build_wpa(&info, rsne_buf);
			eapol_sm_set_ap_wpa(sm, bss->wpa);
			eapol_sm_set_own_wpa(sm, rsne_buf);
		}

		if (security == SECURITY_PSK)
			eapol_sm_set_pmk(sm, network_get_psk(network));
		else
			eapol_sm_set_8021x_config(sm,
					network_get_settings(network));
	}

	device->connect_pending = l_dbus_message_ref(message);

	if (netdev_connect(device->netdev, bss, sm,
					device_netdev_event,
					device_connect_cb, device) < 0) {
		if (sm)
			eapol_sm_free(sm);

		dbus_pending_reply(&device->connect_pending,
				dbus_error_failed(device->connect_pending));
		return;
	}

	device->connected_bss = bss;
	device->connected_network = network;

	device_enter_state(device, DEVICE_STATE_CONNECTING);

	l_dbus_property_changed(dbus, device_get_path(device),
				IWD_DEVICE_INTERFACE, "ConnectedNetwork");
	l_dbus_property_changed(dbus, network_get_path(network),
				IWD_NETWORK_INTERFACE, "Connected");
}

static void device_scan_triggered(int err, void *user_data)
{
	struct device *device = user_data;
	struct l_dbus_message *reply;
	struct l_dbus *dbus = dbus_get_bus();

	l_debug("device_scan_triggered: %i", err);

	if (err < 0) {
		dbus_pending_reply(&device->scan_pending,
				dbus_error_failed(device->scan_pending));
		return;
	}

	l_debug("Scan triggered for %s", netdev_get_name(device->netdev));

	device->scanning = true;
	l_dbus_property_changed(dbus, device_get_path(device),
				IWD_DEVICE_INTERFACE, "Scanning");

	reply = l_dbus_message_new_method_return(device->scan_pending);
	l_dbus_message_set_arguments(reply, "");
	dbus_pending_reply(&device->scan_pending, reply);
}

static struct l_dbus_message *device_scan(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct device *device = user_data;

	l_debug("Scan called from DBus");

	if (device->scan_pending)
		return dbus_error_busy(message);

	device->scan_pending = l_dbus_message_ref(message);

	if (!scan_passive(device->index, device_scan_triggered,
				new_scan_results, device, NULL))
		return dbus_error_failed(message);

	return NULL;
}

static void device_disconnect_cb(struct netdev *netdev, bool success,
					void *user_data)
{
	struct device *device = user_data;

	l_debug("%d, success: %d", device->index, success);

	if (device->disconnect_pending) {
		struct l_dbus_message *reply;

		if (success) {
			reply = l_dbus_message_new_method_return(
						device->disconnect_pending);
			l_dbus_message_set_arguments(reply, "");
		} else
			reply = dbus_error_failed(device->disconnect_pending);


		dbus_pending_reply(&device->disconnect_pending, reply);

	}

	device_enter_state(device, DEVICE_STATE_DISCONNECTED);

	if (device->autoconnect)
		device_enter_state(device, DEVICE_STATE_AUTOCONNECT);
}

int device_disconnect(struct device *device)
{
	struct network *network = device->connected_network;
	struct l_dbus *dbus = dbus_get_bus();

	if (device->state == DEVICE_STATE_DISCONNECTING)
		return -EBUSY;

	if (!device->connected_bss)
		return -ENOTCONN;

	if (netdev_disconnect(device->netdev, device_disconnect_cb, device) < 0)
		return -EIO;

	/*
	 * If the disconnect somehow fails we won't know if we're still
	 * connected so we may as well indicate now that we're no longer
	 * connected.
	 */
	if (device->state == DEVICE_STATE_CONNECTED)
		network_disconnected(network);

	device->connected_bss = NULL;
	device->connected_network = NULL;

	l_dbus_property_changed(dbus, device_get_path(device),
				IWD_DEVICE_INTERFACE, "ConnectedNetwork");
	l_dbus_property_changed(dbus, network_get_path(network),
				IWD_NETWORK_INTERFACE, "Connected");

	device_enter_state(device, DEVICE_STATE_DISCONNECTING);

	return 0;
}

static struct l_dbus_message *device_dbus_disconnect(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct device *device = user_data;
	int result;

	l_debug("");

	/*
	 * Disconnect was triggered by the user, don't autoconnect. Wait for
	 * the user's explicit instructions to scan and connect to the network
	 */
	device_set_autoconnect(device, false);

	result = device_disconnect(device);
	if (result == -EBUSY)
		return dbus_error_busy(message);

	if (result == -ENOTCONN)
		return dbus_error_not_connected(message);

	if (result < 0)
		return dbus_error_failed(message);

	device->disconnect_pending = l_dbus_message_ref(message);

	return NULL;
}

static struct l_dbus_message *device_get_networks(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct device *device = user_data;
	struct l_dbus_message *reply;
	struct l_dbus_message_builder *builder;
	const struct l_queue_entry *entry;

	reply = l_dbus_message_new_method_return(message);
	builder = l_dbus_message_builder_new(reply);

	l_dbus_message_builder_enter_array(builder, "(osns)");

	for (entry = l_queue_get_entries(device->networks_sorted); entry;
				entry = entry->next) {
		const struct network *network = entry->data;
		enum security security = network_get_security(network);
		int32_t signal_strength = network_get_signal_strength(network);

		l_dbus_message_builder_enter_struct(builder, "osns");
		l_dbus_message_builder_append_basic(builder, 'o',
						network_get_path(network));
		l_dbus_message_builder_append_basic(builder, 's',
						network_get_ssid(network));
		l_dbus_message_builder_append_basic(builder, 'n',
							&signal_strength);
		l_dbus_message_builder_append_basic(builder, 's',
						security_to_str(security));
		l_dbus_message_builder_leave_struct(builder);
	}

	l_dbus_message_builder_leave_array(builder);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	return reply;
}

static bool device_property_get_name(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;

	l_dbus_message_builder_append_basic(builder, 's',
					netdev_get_name(device->netdev));
	return true;
}

static bool device_property_get_address(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;
	const char *str;

	str = util_address_to_string(netdev_get_address(device->netdev));
	l_dbus_message_builder_append_basic(builder, 's', str);

	return true;
}

static bool device_property_get_connected_network(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;
	if (!device->connected_network)
		return false;

	l_dbus_message_builder_append_basic(builder, 'o',
				network_get_path(device->connected_network));

	return true;
}

static bool device_property_get_powered(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;
	bool powered = device->state != DEVICE_STATE_OFF;

	l_dbus_message_builder_append_basic(builder, 'b', &powered);

	return true;
}

struct set_powered_cb_data {
	struct device *device;
	struct l_dbus *dbus;
	struct l_dbus_message *message;
	l_dbus_property_complete_cb_t complete;
};

static void set_powered_cb(struct netdev *netdev, int result, void *user_data)
{
	struct set_powered_cb_data *cb_data = user_data;
	struct l_dbus_message *reply = NULL;

	if (result < 0)
		reply = dbus_error_failed(cb_data->message);

	cb_data->complete(cb_data->dbus, cb_data->message, reply);
}

static struct l_dbus_message *device_property_set_powered(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_iter *new_value,
					l_dbus_property_complete_cb_t complete,
					void *user_data)
{
	struct device *device = user_data;
	bool powered;
	struct set_powered_cb_data *cb_data;

	if (!l_dbus_message_iter_get_variant(new_value, "b", &powered))
		return dbus_error_invalid_args(message);

	if (powered == (device->state != DEVICE_STATE_OFF)) {
		complete(dbus, message, NULL);

		return NULL;
	}

	cb_data = l_new(struct set_powered_cb_data, 1);
	cb_data->device = device;
	cb_data->dbus = dbus;
	cb_data->message = message;
	cb_data->complete = complete;

	netdev_set_powered(device->netdev, powered, set_powered_cb, cb_data,
				l_free);

	return NULL;
}

static bool device_property_get_scanning(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;
	bool scanning = device->scanning;

	l_dbus_message_builder_append_basic(builder, 'b', &scanning);

	return true;
}

static bool device_property_get_state(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;
	const char *statestr = "unknown";

	switch (device->state) {
	case DEVICE_STATE_CONNECTED:
		statestr = "connected";
		break;
	case DEVICE_STATE_CONNECTING:
		statestr = "connecting";
		break;
	case DEVICE_STATE_DISCONNECTING:
		statestr = "disconnecting";
		break;
	case DEVICE_STATE_OFF:
	case DEVICE_STATE_DISCONNECTED:
	case DEVICE_STATE_AUTOCONNECT:
		statestr = "disconnected";
		break;
	}

	l_dbus_message_builder_append_basic(builder, 's', statestr);

	return true;
}

static bool device_property_get_adapter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct device *device = user_data;

	l_dbus_message_builder_append_basic(builder, 'o',
					wiphy_get_path(device->wiphy));

	return true;
}

static void setup_device_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "Scan", 0,
				device_scan, "", "");
	l_dbus_interface_method(interface, "Disconnect", 0,
				device_dbus_disconnect, "", "");
	l_dbus_interface_method(interface, "GetOrderedNetworks", 0,
				device_get_networks, "a(osns)", "",
				"networks");

	l_dbus_interface_property(interface, "Name", 0, "s",
					device_property_get_name, NULL);
	l_dbus_interface_property(interface, "Address", 0, "s",
					device_property_get_address, NULL);
	l_dbus_interface_property(interface, "ConnectedNetwork", 0, "o",
					device_property_get_connected_network,
					NULL);
	l_dbus_interface_property(interface, "Powered", 0, "b",
					device_property_get_powered,
					device_property_set_powered);
	l_dbus_interface_property(interface, "Scanning", 0, "b",
					device_property_get_scanning, NULL);
	l_dbus_interface_property(interface, "State", 0, "s",
					device_property_get_state, NULL);
	l_dbus_interface_property(interface, "Adapter", 0, "o",
					device_property_get_adapter, NULL);
}

static bool device_remove_network(const void *key, void *data, void *user_data)
{
	struct network *network = data;

	network_remove(network, -ESHUTDOWN);

	return true;
}

static void device_netdev_notify(struct netdev *netdev,
					enum netdev_watch_event event,
					void *user_data)
{
	struct device *device = user_data;
	struct l_dbus *dbus = dbus_get_bus();

	switch (event) {
	case NETDEV_WATCH_EVENT_UP:
		device_enter_state(device, DEVICE_STATE_AUTOCONNECT);

		WATCHLIST_NOTIFY(&device_watches, device_watch_func_t,
						device, DEVICE_EVENT_INSERTED);

		l_dbus_property_changed(dbus, device_get_path(device),
					IWD_DEVICE_INTERFACE, "Powered");
		break;
	case NETDEV_WATCH_EVENT_DOWN:
		device_enter_state(device, DEVICE_STATE_OFF);

		if (device->scan_pending)
			dbus_pending_reply(&device->scan_pending,
				dbus_error_aborted(device->scan_pending));

		if (device->connect_pending)
			dbus_pending_reply(&device->connect_pending,
				dbus_error_aborted(device->connect_pending));

		if (device->connected_network) {
			struct network *network = device->connected_network;

			device->connected_bss = NULL;
			device->connected_network = NULL;

			l_dbus_property_changed(dbus, device_get_path(device),
						IWD_DEVICE_INTERFACE,
						"ConnectedNetwork");
			l_dbus_property_changed(dbus, network_get_path(network),
						IWD_NETWORK_INTERFACE,
						"Connected");
		}

		l_hashmap_foreach_remove(device->networks,
						device_remove_network, device);

		l_queue_destroy(device->autoconnect_list, l_free);
		device->autoconnect_list = l_queue_new();

		l_queue_destroy(device->bss_list, bss_free);
		device->bss_list = l_queue_new();

		l_queue_destroy(device->networks_sorted, NULL);
		device->networks_sorted = l_queue_new();

		WATCHLIST_NOTIFY(&device_watches, device_watch_func_t,
						device, DEVICE_EVENT_REMOVED);

		l_dbus_property_changed(dbus, device_get_path(device),
					IWD_DEVICE_INTERFACE, "Powered");
		break;
	case NETDEV_WATCH_EVENT_NAME_CHANGE:
		l_dbus_property_changed(dbus, device_get_path(device),
					IWD_DEVICE_INTERFACE, "Name");
		break;
	}
}

struct device *device_create(struct wiphy *wiphy, struct netdev *netdev)
{
	struct device *device;
	struct l_dbus *dbus = dbus_get_bus();
	uint32_t ifindex = netdev_get_ifindex(netdev);

	device = l_new(struct device, 1);
	device->bss_list = l_queue_new();
	device->networks = l_hashmap_new();
	watchlist_init(&device->state_watches);
	l_hashmap_set_hash_function(device->networks, l_str_hash);
	l_hashmap_set_compare_function(device->networks,
				(l_hashmap_compare_func_t) strcmp);
	device->networks_sorted = l_queue_new();
	device->index = ifindex;
	device->wiphy = wiphy;
	device->netdev = netdev;
	device->autoconnect = true;

	l_queue_push_head(device_list, device);

	if (!l_dbus_object_add_interface(dbus, device_get_path(device),
					IWD_DEVICE_INTERFACE, device))
		l_info("Unable to register %s interface", IWD_DEVICE_INTERFACE);

	if (!l_dbus_object_add_interface(dbus, device_get_path(device),
					L_DBUS_INTERFACE_PROPERTIES, device))
		l_info("Unable to register %s interface",
				L_DBUS_INTERFACE_PROPERTIES);

	scan_ifindex_add(device->index);

	device_netdev_notify(netdev, netdev_get_is_up(netdev) ?
						NETDEV_WATCH_EVENT_UP :
						NETDEV_WATCH_EVENT_DOWN,
						device);
	device->netdev_watch_id =
		netdev_watch_add(netdev, device_netdev_notify, device);

	return device;
}

static void device_free(void *user)
{
	struct device *device = user;
	struct l_dbus *dbus;

	l_debug("");

	if (device->scan_pending)
		dbus_pending_reply(&device->scan_pending,
				dbus_error_aborted(device->scan_pending));

	if (device->connect_pending)
		dbus_pending_reply(&device->connect_pending,
				dbus_error_aborted(device->connect_pending));

	if (device->state != DEVICE_STATE_OFF)
		WATCHLIST_NOTIFY(&device_watches, device_watch_func_t,
						device, DEVICE_EVENT_REMOVED);

	watchlist_destroy(&device->state_watches);

	dbus = dbus_get_bus();
	l_dbus_unregister_object(dbus, device_get_path(device));

	l_queue_destroy(device->networks_sorted, NULL);
	l_hashmap_destroy(device->networks, network_free);

	l_queue_destroy(device->bss_list, bss_free);
	l_queue_destroy(device->old_bss_list, bss_free);
	l_queue_destroy(device->autoconnect_list, l_free);

	netdev_watch_remove(device->netdev, device->netdev_watch_id);

	scan_ifindex_remove(device->index);
	l_free(device);
}

void device_remove(struct device *device)
{
	if (!l_queue_remove(device_list, device))
		return;

	device_free(device);
}

bool device_init(void)
{
	if (!l_dbus_register_interface(dbus_get_bus(),
					IWD_DEVICE_INTERFACE,
					setup_device_interface,
					NULL, false))
		return false;

	watchlist_init(&device_watches);
	device_list = l_queue_new();

	return true;
}

bool device_exit(void)
{
	if (!l_queue_isempty(device_list))
		l_warn("device_list isn't empty!");

	l_queue_destroy(device_list, device_free);
	device_list = NULL;

	watchlist_destroy(&device_watches);

	l_dbus_unregister_interface(dbus_get_bus(), IWD_DEVICE_INTERFACE);

	return true;
}
