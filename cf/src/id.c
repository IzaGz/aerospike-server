/*
 * id.c
 *
 * Copyright (C) 2008-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "util.h" // we don't have our own header file

#include <errno.h>
#include <ifaddrs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <citrusleaf/cf_digest.h>
#include <citrusleaf/cf_types.h>
#include <citrusleaf/alloc.h>

#include "fault.h"


/*
** need a spot for this
*/

cf_digest cf_digest_zero = { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } };

/*
** nodeids are great things to use as keys in the hash table
*/


uint32_t
cf_nodeid_shash_fn(void *value)
{
	uint32_t *b = value;
	uint32_t acc = 0;
	for (int i=0;i<sizeof(cf_node);i++) {
		acc += *b;
	}
	return(acc);
}

uint32_t
cf_nodeid_rchash_fn(void *value, uint32_t value_len)
{
	uint32_t *b = value;
	uint32_t acc = 0;
	for (int i=0;i<sizeof(cf_node);i++) {
		acc += *b;
	}
	return(acc);
}

/*
 * Gets the ip address of an interface.
 */

int
cf_ipaddr_get(int socket, char *nic_id, char **node_ip )
{
	struct sockaddr_in sin;
	struct ifreq ifr;
	in_addr_t ip_addr;

	memset(&ip_addr, 0, sizeof(in_addr_t));
	memset(&sin, 0, sizeof(struct sockaddr));
	memset(&ifr, 0, sizeof(ifr));

	// copy the nic name (eth0, eth1, eth2, etc.) ifr variable structure
	strncpy(ifr.ifr_name, nic_id, IFNAMSIZ);

	// get the ifindex for the adapter...
	if (ioctl(socket, SIOCGIFINDEX, &ifr) < 0) {
		cf_debug(CF_MISC, "Can't get ifindex for adapter %s - %d %s\n", nic_id, errno, cf_strerror(errno));
		return(-1);
	}

	// get the IP address
	memset(&sin, 0, sizeof(struct sockaddr));
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, nic_id, IFNAMSIZ);
	ifr.ifr_addr.sa_family = AF_INET;
	if (ioctl(socket, SIOCGIFADDR, &ifr)< 0) {
		cf_debug(CF_MISC, "can't get IP address: %d %s", errno, cf_strerror(errno));
		return(-1);
	}
	memcpy(&sin, &ifr.ifr_addr, sizeof(struct sockaddr));
	ip_addr = sin.sin_addr.s_addr;
	char cpaddr[24];
	if (NULL == inet_ntop(AF_INET, &ip_addr, (char *)cpaddr, sizeof(cpaddr))) {
		cf_warning(CF_MISC, "received suspicious address %s : %s", cpaddr, cf_strerror(errno));
		return(-1);
	}
	cf_info (CF_MISC, "Node ip: %s", cpaddr);
	*node_ip = cf_strdup(cpaddr);

	return(0);
}

/*
 * Gets a unique id for this process instance
 * Uses the mac address right now
 * And combine with the unique port number, which is why it needs to be passed in
 * Needs to be a little more subtle:
 * Should stash the mac address or something, in case you have to replace a card.
 *
 * parameters- 
 * Input params:
 * port - used in setting Node ID
 * hb_mode - Controls whether hb_addrp is filled out with the IP address. 
 * config_interface_names - Pointer to an array of interface names if specified in the config file, 
 *			    NULL if absent. 
 * 
 * Output params:
 * id - Node ID (address and port)
 * node_ipp - Pointer wherein the IP adddress is stored 
 * hb_addrp - Pointer to a string wherein the heartbeat address is stored, as specified by hb_mode 
 */

// names to check, in order
//
char *default_interface_names[] = { "eth%d", "bond%d", "wlan%d", 0 };

int
cf_nodeid_get( unsigned short port, cf_node *id, char **node_ipp, hb_mode_enum hb_mode, char **hb_addrp, char **config_interface_names)
{
	int fdesc;
	struct ifreq req;

	// The default interface names can be overridden by the interface name passed into config
	char **interface_names = default_interface_names;
	bool default_config = true;
	int jlimit = 11;

	if (config_interface_names) {
		interface_names = config_interface_names;
		default_config = false;
		jlimit = 1;
	}

	if (0 >= (fdesc = socket(AF_INET, SOCK_STREAM, 0))) {
		cf_warning(CF_MISC, "can't open socket: %d %s", errno, cf_strerror(errno));
		return(-1);
	}
	int i = 0;
	bool done = false;

	while ((interface_names[i]) && (!done)) {

		int j=0;
		while ((!done) && (j < jlimit)) {

			if (default_config)
				sprintf(req.ifr_name, interface_names[i],j);
			else
				sprintf(req.ifr_name, interface_names[i]);

			if (0 == ioctl(fdesc, SIOCGIFHWADDR, &req)) {
				if (cf_ipaddr_get(fdesc, req.ifr_name, node_ipp) == 0) {
					done = true;
					break;
				}
			}

			cf_debug(CF_MISC, "can't get physical address of interface %s: %d %s", req.ifr_name, errno, cf_strerror(errno));

			j++;

		}
		i++;
	}

	if (!done) {
		if (default_config) {
			
			struct ifaddrs* interface_addrs = NULL;
			if (getifaddrs(&interface_addrs) == -1) {
				cf_warning(CF_MISC, "getifaddrs failed %d %s", errno, cf_strerror(errno));
				return -1;
			}
			if (!interface_addrs) {
				cf_warning(CF_MISC, "getifaddrs returned NULL");
				return -1;
			}

			struct ifaddrs *ifa;
			for (ifa = interface_addrs; ifa != NULL && (!done); ifa = ifa->ifa_next) {
				if (ifa->ifa_data != 0) {
					struct ifreq req;
					strcpy(req.ifr_name, ifa->ifa_name);
			
					/* Get MAC address */
					if (ioctl(fdesc, SIOCGIFHWADDR, &req) == 0) {

						uint8_t* mac = (uint8_t*)req.ifr_ifru.ifru_hwaddr.sa_data;
						/* MAC address sanity check */
						if ((mac[0] == 0 && mac[1] == 0 && mac[2] == 0 
						&& mac[3] == 0 && mac[4] == 0 && mac[5] == 0)
						|| (mac[0] == 0xff && mac[1] == 0xff && mac[2] == 0xff
						&& mac[3] == 0xff && mac[4] == 0xff && mac[5] == 0xff )) {
							
							continue;
						}
						/* Get IP address */
						if (cf_ipaddr_get(fdesc, req.ifr_name, node_ipp) == 0) {
							done = true;
							continue;
						}
					}
					else {
						/* Continue to the next interface if cannot get MAC address */
						continue;
					}
				}
			}
		}
		else {
			cf_warning(CF_MISC, "can't get physical address of interface name specfied in config file, tried %s. fatal: %d %s", interface_names[0], errno, cf_strerror(errno));
			close(fdesc);
			return(-1);
		}
	}
	if (!done) {
		cf_warning(CF_MISC, "Tried eth,bond,wlan and list of all available interfaces on device.Failed to retrieve physical address with errno %d %s\n", errno, cf_strerror(errno));
		close(fdesc);
		return(-1);
	}

	close(fdesc);
	/*
	 * Set the hb_addr to be the same as the ip address if the mode is mesh and the hb_addr parameter is empty
	 * Configuration file overrides the automatic node ip detection
	 *	- this gives us a work around in case the node ip is somehow detected wrong in production
	 */
	if (hb_mode == AS_HB_MODE_MESH)
	{
		if (*hb_addrp == NULL)
			*hb_addrp = cf_strdup(*node_ipp);

		cf_info(CF_MISC, "Heartbeat address for mesh: %s", *hb_addrp);
	}

	*id = 0;
	memcpy(id, req.ifr_hwaddr.sa_data, 6);

	memcpy(((byte *)id) + 6, &port, 2);

	cf_debug(CF_MISC, "port %d id %"PRIx64, port, *id);

	return(0);
}

unsigned short
cf_nodeid_get_port(cf_node id)
{
	byte *b = (byte *) &id;
	unsigned short port;
	memcpy(&port, &b[6], 2);
	return(port);
}
