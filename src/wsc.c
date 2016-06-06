/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2015  Intel Corporation. All rights reserved.
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
#include <ell/ell.h>

#include "src/dbus.h"
#include "src/netdev.h"
#include "src/device.h"
#include "src/wiphy.h"
#include "src/scan.h"
#include "src/mpdu.h"
#include "src/ie.h"
#include "src/wscutil.h"
#include "src/util.h"
#include "src/wsc.h"

#define WALK_TIME 120

static struct l_genl_family *nl80211 = NULL;
static uint32_t device_watch = 0;

struct wsc_sm {
	uint32_t ifindex;
	uint8_t *wsc_ies;
	size_t wsc_ies_size;
	struct l_timeout *walk_timer;
	uint32_t scan_id;
};

struct wsc {
	struct device *device;
	struct l_dbus_message *pending;
	struct wsc_sm *sm;
};

static bool scan_results(uint32_t wiphy_id, uint32_t ifindex,
				struct l_queue *bss_list, void *userdata)
{
	struct wsc_sm *sm = userdata;
	struct scan_bss *bss_2g;
	struct scan_bss *bss_5g;
	struct scan_bss *target;
	uint8_t uuid_2g[16];
	uint8_t uuid_5g[16];
	const struct l_queue_entry *bss_entry;
	struct wsc_probe_response probe_response;

	bss_2g = NULL;
	bss_5g = NULL;

	sm->scan_id = 0;

	for (bss_entry = l_queue_get_entries(bss_list); bss_entry;
				bss_entry = bss_entry->next) {
		struct scan_bss *bss = bss_entry->data;
		enum scan_band band;
		int err;

		l_debug("bss '%s' with SSID: %s, freq: %u",
			util_address_to_string(bss->addr),
			util_ssid_to_utf8(bss->ssid_len, bss->ssid),
			bss->frequency);

		l_debug("bss->wsc: %p, %zu", bss->wsc, bss->wsc_size);

		if (!bss->wsc)
			continue;

		err = wsc_parse_probe_response(bss->wsc, bss->wsc_size,
						&probe_response);
		if (err < 0) {
			l_debug("ProbeResponse parse failed: %s",
							strerror(-err));
			continue;
		}

		l_debug("SelectedRegistar: %s",
			probe_response.selected_registrar ? "true" : "false");

		if (!probe_response.selected_registrar)
			continue;

		if (probe_response.device_password_id !=
				WSC_DEVICE_PASSWORD_ID_PUSH_BUTTON)
			continue;

		scan_freq_to_channel(bss->frequency, &band);

		switch (band) {
		case SCAN_BAND_2_4_GHZ:
			if (bss_2g) {
				l_debug("2G Session overlap error");
				return false;
			}

			bss_2g = bss;
			memcpy(uuid_2g, probe_response.uuid_e, 16);
			break;

		case SCAN_BAND_5_GHZ:
			if (bss_5g) {
				l_debug("5G Session overlap error");
				return false;
			}

			bss_5g = bss;
			memcpy(uuid_5g, probe_response.uuid_e, 16);
			break;

		default:
			return false;
		}
	}

	if (bss_2g && bss_5g && memcmp(uuid_2g, uuid_5g, 16)) {
		l_debug("Found two PBC APs on different bands");
		return false;
	}

	if (bss_5g)
		target = bss_5g;
	else if (bss_2g)
		target = bss_2g;
	else {
		l_debug("No PBC APs found, running the scan again");
		sm->scan_id = scan_active(sm->ifindex,
						sm->wsc_ies, sm->wsc_ies_size,
						NULL, scan_results, sm, NULL);
		return false;
	}

	l_debug("Found AP to connect to: %s",
			util_address_to_string(target->addr));

	return false;
}

struct wsc_sm *wsc_sm_new_pushbutton(uint32_t ifindex, const uint8_t *addr,
					uint32_t bands)
{
	static const uint8_t wfa_oui[] = { 0x00, 0x50, 0xF2 };
	struct wsc_sm *sm;
	struct wsc_probe_request req;
	uint8_t *wsc_data;
	size_t wsc_data_size;

	memset(&req, 0, sizeof(req));

	req.version2 = true;
	req.request_type = WSC_REQUEST_TYPE_ENROLLEE_INFO;

	/* TODO: Grab from configuration file ? */
	req.config_methods = WSC_CONFIGURATION_METHOD_VIRTUAL_PUSH_BUTTON |
				WSC_CONFIGURATION_METHOD_KEYPAD;

	if (!wsc_uuid_from_addr(addr, req.uuid_e))
		return NULL;

	/* TODO: Grab from configuration file ? */
	req.primary_device_type.category = 255;
	memcpy(req.primary_device_type.oui, wfa_oui, 3);
	req.primary_device_type.oui_type = 0x04;
	req.primary_device_type.subcategory = 0;

	if (bands & SCAN_BAND_2_4_GHZ)
		req.rf_bands |= WSC_RF_BAND_2_4_GHZ;
	if (bands & SCAN_BAND_5_GHZ)
		req.rf_bands |= WSC_RF_BAND_5_0_GHZ;

	req.association_state = WSC_ASSOCIATION_STATE_NOT_ASSOCIATED,
	req.configuration_error = WSC_CONFIGURATION_ERROR_NO_ERROR,
	req.device_password_id = WSC_DEVICE_PASSWORD_ID_PUSH_BUTTON,
	req.request_to_enroll = true,

	wsc_data = wsc_build_probe_request(&req, &wsc_data_size);
	if (!wsc_data)
		return NULL;

	sm = l_new(struct wsc_sm, 1);
	sm->wsc_ies = ie_tlv_encapsulate_wsc_payload(wsc_data, wsc_data_size,
							&sm->wsc_ies_size);
	l_free(wsc_data);

	if (!sm->wsc_ies) {
		l_free(sm);
		return NULL;
	}

	sm->ifindex = ifindex;
	sm->scan_id = scan_active(ifindex, sm->wsc_ies, sm->wsc_ies_size,
					NULL, scan_results, sm, NULL);

	return sm;
}

void wsc_sm_free(struct wsc_sm *sm)
{
	l_free(sm->wsc_ies);

	if (sm->scan_id > 0) {
		scan_cancel(sm->ifindex, sm->scan_id);
		sm->scan_id = 0;
	}

	l_free(sm);
}

static struct l_dbus_message *wsc_push_button(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct wsc *wsc = user_data;

	l_debug("");

	if (wsc->pending)
		return dbus_error_busy(message);

	/* TODO: Parse wiphy bands to set the RF Bands properly below */
	wsc->sm = wsc_sm_new_pushbutton(device_get_ifindex(wsc->device),
				device_get_address(wsc->device),
				SCAN_BAND_2_4_GHZ | SCAN_BAND_5_GHZ);

	wsc->pending = l_dbus_message_ref(message);
	return NULL;
}

static struct l_dbus_message *wsc_cancel(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct wsc *wsc = user_data;
	struct l_dbus_message *reply;

	l_debug("");

	if (!wsc->pending)
		return dbus_error_not_available(message);

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	dbus_pending_reply(&wsc->pending, dbus_error_aborted(wsc->pending));

	wsc_sm_free(wsc->sm);
	wsc->sm = NULL;

	return reply;
}

static void setup_wsc_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "PushButton", 0,
				wsc_push_button, "", "");
	l_dbus_interface_method(interface, "Cancel", 0,
				wsc_cancel, "", "");
}

static void wsc_free(void *userdata)
{
	struct wsc *wsc = userdata;

	if (wsc->pending) {
		dbus_pending_reply(&wsc->pending,
					dbus_error_not_available(wsc->pending));

		wsc_sm_free(wsc->sm);
		wsc->sm = NULL;
	}

	l_free(wsc);
}

static void device_appeared(struct device *device, void *userdata)
{
	struct l_dbus *dbus = dbus_get_bus();
	struct wsc *wsc;

	wsc = l_new(struct wsc, 1);
	wsc->device = device;

	if (!l_dbus_object_add_interface(dbus, device_get_path(device),
						IWD_WSC_INTERFACE,
						wsc)) {
		wsc_free(wsc);
		l_info("Unable to register %s interface", IWD_WSC_INTERFACE);
	}
}

static void device_disappeared(struct device *device, void *userdata)
{
}

bool wsc_init(struct l_genl_family *in)
{
	if (!l_dbus_register_interface(dbus_get_bus(), IWD_WSC_INTERFACE,
					setup_wsc_interface,
					wsc_free, false))
		return false;

	device_watch = device_watch_add(device_appeared, device_disappeared,
						NULL, NULL);
	if (!device_watch)
		return false;

	nl80211 = in;
	return true;
}

bool wsc_exit()
{
	l_debug("");

	if (!nl80211)
		return false;

	l_dbus_unregister_interface(dbus_get_bus(), IWD_WSC_INTERFACE);

	device_watch_remove(device_watch);
	nl80211 = 0;

	return true;
}
