/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2018  Intel Corporation. All rights reserved.
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

#include <stdbool.h>

struct wiphy;
struct netdev;
enum security;
struct scan_bss;
struct network;

struct station {
	struct network *connected_network;
	struct l_queue *autoconnect_list;

	struct wiphy *wiphy;
	struct netdev *netdev;
};

void station_autoconnect_next(struct station *station);
void station_add_autoconnect_bss(struct station *station,
					struct network *network,
					struct scan_bss *bss);

struct handshake_state *station_handshake_setup(struct station *station,
						struct network *network,
						struct scan_bss *bss);

struct station *station_create(struct wiphy *wiphy, struct netdev *netdev);
void station_free(struct station *station);