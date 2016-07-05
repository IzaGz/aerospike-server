/*
 * socket.h
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

#pragma once

#if defined USE_IPV6
#include <socket_ee.h>
#else
#include <socket_ce.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int32_t cf_ip_addr_from_string(const char *string, cf_ip_addr *addr);
int32_t cf_ip_addr_to_string(const cf_ip_addr *addr, char *string, size_t size);
int32_t cf_ip_addr_from_binary(const uint8_t *binary, cf_ip_addr *addr, size_t size);
int32_t cf_ip_addr_to_binary(const cf_ip_addr *addr, uint8_t *binary, size_t size);

int32_t cf_ip_port_from_string(const char *string, in_port_t *port);
int32_t cf_ip_port_to_string(in_port_t port, char *string, size_t size);
int32_t cf_ip_port_from_binary(const uint8_t *binary, in_port_t *port, size_t size);
int32_t cf_ip_port_to_binary(in_port_t port, uint8_t *binary, size_t size);

int32_t cf_sock_addr_from_string(const char *string, cf_sock_addr *addr);
int32_t cf_sock_addr_to_string(const cf_sock_addr *addr, char *string, size_t size);
int32_t cf_sock_addr_from_binary(const uint8_t *binary, cf_sock_addr *addr, size_t size);
int32_t cf_sock_addr_to_binary(const cf_sock_addr *addr, uint8_t *binary, size_t size);

void cf_sock_addr_from_binary_legacy(const uint64_t *binary, cf_sock_addr *addr);
void cf_sock_addr_to_binary_legacy(const cf_sock_addr *addr, uint64_t *binary);

// -------------------- OLD CODE --------------------

#include <netinet/in.h>
#include <sys/socket.h>

// TODO - as_ .c files depend on this:
#include <arpa/inet.h>


/* SYNOPSIS
 * */

// the reality is all addresses are IPv4, even with the coming ipv6, an address can
// fit easily in a uint64_t. Create a type and some utility routines so you can just
// traffic in an address type.

typedef uint64_t cf_sockaddr;

/* cf_socket_cfg
 * A socket, which can be used for either inbound or outbound connections */
typedef struct cf_socket_cfg_t {
	char *addr;
	int port;
	bool reuse_addr; // set if you want 'reuseaddr' for server socket setup
					 // not recommended for production use, rather nice for debugging
	int proto;
	int sock;
	struct sockaddr_in saddr;
} cf_socket_cfg;

/* cf_mcastsocket_cfg
 * A multicast socket */
typedef struct cf_mcastsocket_cfg_t {
	cf_socket_cfg s;
	struct ip_mreq ireq;
    char *tx_addr;    // if there is a specific ip address that should be used to send the mcast message
    unsigned char mcast_ttl;
} cf_mcastsocket_cfg;

/* Function declarations */
extern int cf_socket_set_nonblocking(int s);
extern void cf_socket_set_nodelay(int s);
extern int cf_socket_recv(int sock, void *buf, size_t buflen, int flags);
extern int cf_socket_send(int sock, void *buf, size_t buflen, int flags);
extern int cf_socket_init_svc(cf_socket_cfg *s);
extern int cf_socket_init_client(cf_socket_cfg *s, int timeout);
extern void cf_socket_close(cf_socket_cfg *s);
extern int cf_mcastsocket_init(cf_mcastsocket_cfg *ms);
extern void cf_mcastsocket_close(cf_mcastsocket_cfg *ms);
extern int cf_socket_recvfrom(int sock, void *buf, size_t buflen, int flags, cf_sockaddr *from);
extern int cf_socket_sendto(int sock, void *buf, size_t buflen, int flags, cf_sockaddr to);

extern int cf_socket_connect_nb(cf_sockaddr so, int *fd);
extern void cf_sockaddr_convertto(const struct sockaddr_in *src, cf_sockaddr *dst);
extern void cf_sockaddr_convertfrom(const cf_sockaddr src, struct sockaddr_in *dst);
extern void cf_sockaddr_setport(cf_sockaddr *so, unsigned short port);

/*
** get information about all interfaces
** currently returns ipv4 only - but does return loopback
**
** example:
**
** uint8_t buf[512];
** cf_ifaddr *ifaddr;
** int        ifaddr_sz;
** cf_ifaddr_get(&ifaddr, &ifaddr_sz, buf, sizeof(buf));
**
*/

typedef struct cf_ifaddr_s {
	uint32_t		flags;
	unsigned short	family;
	struct sockaddr sa;
} cf_ifaddr;

extern int cf_ifaddr_get(cf_ifaddr **ifaddr, int *ifaddr_sz, uint8_t *buf, size_t buf_sz);
