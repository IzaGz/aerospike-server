/*
 * socket.c
 *
 * Copyright (C) 2008-2016 Aerospike, Inc.
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

#define CF_SOCKET_PRIVATE
#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "fault.h"

#include "citrusleaf/alloc.h"

static char *
safe_strdup(const char *string)
{
	char *res = cf_strdup(string);

	if (res == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
	}

	return res;
}

void
cf_ip_addr_to_string_safe(const cf_ip_addr *addr, char *string, size_t size)
{
	if (cf_ip_addr_to_string(addr, string, size) < 0) {
		cf_crash(CF_SOCKET, "String buffer overflow");
	}
}

void
cf_ip_addr_sort(cf_ip_addr *addrs, uint32_t n_addrs)
{
	uint32_t n = n_addrs;

	if (n == 0) {
		return;
	}

	bool swapped;

	do {
		swapped = false;

		for (uint32_t i = 0; i < n - 1; ++i) {
			if (cf_ip_addr_compare(&addrs[i], &addrs[i + 1]) < 0) {
				cf_ip_addr tmp;
				cf_ip_addr_copy(&addrs[i], &tmp);
				cf_ip_addr_copy(&addrs[i + 1], &addrs[i]);
				cf_ip_addr_copy(&tmp, &addrs[i + 1]);
				swapped = true;
			}
		}

		--n;
	}
	while (swapped);
}

int32_t
cf_ip_port_from_string(const char *string, cf_ip_port *port)
{
	char *end;
	uint64_t tmp = strtoul(string, &end, 10);

	if (*end != 0 || tmp > 65535) {
		cf_warning(CF_SOCKET, "Invalid port '%s'", string);
		return -1;
	}

	*port = (cf_ip_port)tmp;
	return 0;
}

int32_t
cf_ip_port_to_string(cf_ip_port port, char *string, size_t size)
{
	int32_t count = snprintf(string, size, "%hu", port);

	if ((size_t)count >= size) {
		cf_warning(CF_SOCKET, "Output buffer overflow");
		return -1;
	}

	return count;
}

void
cf_ip_port_to_string_safe(cf_ip_port port, char *string, size_t size)
{
	if (cf_ip_port_to_string(port, string, size) < 0) {
		cf_crash(CF_SOCKET, "String buffer overflow");
	}
}

int32_t
cf_ip_port_from_binary(const uint8_t *binary, size_t size, cf_ip_port *port)
{
	if (size < 2) {
		cf_warning(CF_SOCKET, "Input buffer underflow");
		return -1;
	}

	*port = (binary[0] << 8) | binary[1];
	return 2;
}

int32_t
cf_ip_port_to_binary(cf_ip_port port, uint8_t *binary, size_t size)
{
	if (size < 2) {
		cf_warning(CF_SOCKET, "Output buffer overflow");
		return -1;
	}

	binary[0] = port >> 8;
	binary[1] = port & 255;
	return 2;
}

void
cf_ip_port_from_node_id(cf_node id, cf_ip_port *port)
{
	uint8_t *buff = (uint8_t *)&id;
	memcpy(port, buff + 6, 2);
}

void
cf_sock_addr_to_string_safe(const cf_sock_addr *addr, char *string, size_t size)
{
	if (cf_sock_addr_to_string(addr, string, size) < 0) {
		cf_crash(CF_SOCKET, "String buffer overflow");
	}
}

int32_t
cf_sock_addr_from_binary(const uint8_t *binary, size_t size, cf_sock_addr *addr)
{
	int32_t total = 0;
	int32_t count = cf_ip_addr_from_binary(binary, size, &addr->addr);

	if (count < 0) {
		return -1;
	}

	total += count;
	count = cf_ip_port_from_binary(binary + total, size - total, &addr->port);

	if (count < 0) {
		return -1;
	}

	total += count;
	return total;
}

int32_t
cf_sock_addr_to_binary(const cf_sock_addr *addr, uint8_t *binary, size_t size)
{
	int32_t total = 0;
	int32_t count = cf_ip_addr_to_binary(&addr->addr, binary, size);

	if (count < 0) {
		return -1;
	}

	total += count;
	count = cf_ip_port_to_binary(addr->port, binary + total, size - total);

	if (count < 0) {
		return -1;
	}

	total += count;
	return total;
}

int32_t
cf_sock_addr_from_host_port(const char *host, cf_ip_port port, cf_sock_addr *addr)
{
	if (cf_ip_addr_from_string(host, &addr->addr) < 0) {
		cf_warning(CF_SOCKET, "Invalid host address '%s'", host);
		return -1;
	}

	addr->port = port;
	return 0;
}

void
cf_sock_addr_from_addr_port(const cf_ip_addr *ip_addr, cf_ip_port port, cf_sock_addr *addr)
{
	addr->addr = *ip_addr;
	addr->port = port;
}

int32_t
cf_sock_addr_compare(const cf_sock_addr *lhs, const cf_sock_addr *rhs)
{
	int32_t res = cf_ip_addr_compare(&lhs->addr, &rhs->addr);

	if (res != 0) {
		return res;
	}

	if (lhs->port == rhs->port) {
		return 0;
	}

	return (int32_t)lhs->port - (int32_t)rhs->port;
}

void
cf_sock_addr_copy(const cf_sock_addr *from, cf_sock_addr *to)
{
	cf_ip_addr_copy(&from->addr, &to->addr);
	to->port = from->port;
}

void
cf_sock_addr_set_zero(cf_sock_addr *addr)
{
	cf_ip_addr_set_zero(&addr->addr);
	addr->port = 0;
}

bool
cf_sock_addr_is_zero(const cf_sock_addr *addr)
{
	return cf_ip_addr_is_zero(&addr->addr) && addr->port == 0;
}

static int32_t
safe_fcntl(int32_t fd, int32_t cmd, int32_t arg)
{
	int32_t res = fcntl(fd, cmd, arg);

	if (res < 0) {
		cf_crash(CF_SOCKET, "fcntl(%d) failed on FD %d: %d (%s)",
				cmd, fd, errno, cf_strerror(errno));
	}

	return res;
}

static int32_t
safe_ioctl(int32_t fd, int32_t req, int32_t *arg)
{
	int32_t res = ioctl(fd, req, arg);

	if (res < 0) {
		cf_crash(CF_SOCKET, "ioctl(%d) failed on FD %d: %d (%s)",
				req, fd, errno, cf_strerror(errno));
	}

	return res;
}

static void
safe_setsockopt(int32_t fd, int32_t level, int32_t name, const void *val, socklen_t len)
{
	if (setsockopt(fd, level, name, val, len) < 0) {
		cf_crash(CF_SOCKET, "setsockopt(%d¸ %d) failed on FD %d: %d (%s)",
				level, name, fd, errno, cf_strerror(errno));
	}
}

static void
safe_getsockopt(int32_t fd, int32_t level, int32_t name, void *val, socklen_t *len)
{
	if (getsockopt(fd, level, name, val, len) < 0) {
		cf_crash(CF_SOCKET, "getsockopt(%d, %d) failed on FD %d: %d (%s)",
				level, name, fd, errno, cf_strerror(errno));
	}
}

static int32_t
safe_wait(int32_t efd, struct epoll_event *events, int32_t max, int32_t timeout)
{
	while (true) {
		cf_debug(CF_SOCKET, "Waiting on epoll FD %d", efd);
		int32_t count = epoll_wait(efd, events, max, timeout);

		if (count < 0) {
			if (errno == EINTR) {
				cf_debug(CF_SOCKET, "Interrupted");
				continue;
			}

			cf_crash(CF_SOCKET, "epoll_wait() failed on epoll FD %d: %d (%s)",
					efd, errno, cf_strerror(errno));
		}

		return count;
	}
}

static void
safe_close(int32_t fd)
{
	if (close(fd) < 0) {
		cf_crash(CF_SOCKET, "Error while closing FD %d: %d (%s)",
				fd, errno, cf_strerror(errno));
	}
}

void
cf_socket_disable_blocking(cf_socket *sock)
{
	int32_t flags = safe_fcntl(sock->fd, F_GETFL, 0);
	safe_fcntl(sock->fd, F_SETFL, flags | O_NONBLOCK);
}

void
cf_socket_enable_blocking(cf_socket *sock)
{
	int32_t flags = safe_fcntl(sock->fd, F_GETFL, 0);
	safe_fcntl(sock->fd, F_SETFL, flags & ~O_NONBLOCK);
}

void
cf_socket_disable_nagle(cf_socket *sock)
{
	static const int32_t flag = 1;
	safe_setsockopt(sock->fd, SOL_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

void
cf_socket_enable_nagle(cf_socket *sock)
{
	static const int32_t flag = 0;
	safe_setsockopt(sock->fd, SOL_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

void
cf_socket_keep_alive(cf_socket *sock, int32_t idle, int32_t interval, int32_t count)
{
	static const int32_t flag = 1;
	safe_setsockopt(sock->fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

	if (idle > 0) {
		safe_setsockopt(sock->fd, SOL_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	}

	if (interval > 0) {
		safe_setsockopt(sock->fd, SOL_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	}

	if (count > 0) {
		safe_setsockopt(sock->fd, SOL_TCP, TCP_KEEPCNT, &count, sizeof(count));
	}
}

void
cf_socket_set_send_buffer(cf_socket *sock, int32_t size)
{
	safe_setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

void
cf_socket_set_receive_buffer(cf_socket *sock, int32_t size)
{
	safe_setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

void
cf_socket_set_window(cf_socket *sock, int32_t size)
{
	safe_setsockopt(sock->fd, SOL_TCP, TCP_WINDOW_CLAMP, &size, sizeof(size));
}

static int32_t
config_address(const cf_socket_cfg *conf, struct sockaddr *sa, cf_sock_addr *addr)
{
	cf_sock_addr _addr;

	if (addr == NULL) {
		addr = &_addr;
	}

	if (conf->addr == NULL) {
		cf_warning(CF_SOCKET, "Missing service address");
		return -1;
	}

	if (cf_ip_addr_from_string(conf->addr, &addr->addr) < 0) {
		cf_warning(CF_SOCKET, "Invalid service address: %s", conf->addr);
		return -1;
	}

	if (conf->port == 0) {
		cf_warning(CF_SOCKET, "Missing service port");
		return -1;
	}

	addr->port = conf->port;
	cf_sock_addr_to_native(addr, sa);
	return 0;
}

int32_t
cf_socket_init_server(cf_socket_cfg *conf)
{
	int32_t res = -1;
	struct sockaddr_storage sas;

	if (config_address(conf, (struct sockaddr *)&sas, NULL) < 0) {
		goto cleanup0;
	}

	int32_t fd = socket(sas.ss_family, conf->type, 0);

	if (fd < 0) {
		cf_warning(CF_SOCKET, "Error while creating socket for %s:%d: %d (%s)",
				conf->addr, conf->port, errno, cf_strerror(errno));
		goto cleanup0;
	}

	cf_socket *sock = cf_malloc(sizeof(cf_socket));

	if (sock == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
	}

	sock->fd = fd;
	fd = -1;

	if (conf->reuse_addr) {
		static const int32_t flag = 1;
		safe_setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	}

	while (bind(sock->fd, (struct sockaddr *)&sas,
			cf_socket_addr_len((struct sockaddr *)&sas)) < 0) {
		if (errno != EADDRINUSE) {
			cf_warning(CF_SOCKET, "Error while binding to %s:%d: %d (%s)",
					conf->addr, conf->port, errno, cf_strerror(errno));
			goto cleanup1;
		}

		cf_warning(CF_SOCKET, "Socket %s:%d in use, waiting", conf->addr, conf->port);
		usleep(5 * 1000 * 1000);
	}

	if (conf->type == SOCK_STREAM && listen(sock->fd, 512) < 0) {
		cf_warning(CF_SOCKET, "Error while listening on %s:%d: %d (%s)",
				conf->addr, conf->port, errno, cf_strerror(errno));
		goto cleanup1;
	}

	// No Nagle here. It will be disabled for the accepted connections.

	conf->sock = sock;
	res = 0;
	goto cleanup0;

cleanup1:
	safe_close(sock->fd);
	cf_free(sock);

cleanup0:
	return res;
}

static int32_t
connect_socket(cf_socket *sock, struct sockaddr *sa, int32_t timeout)
{
	cf_debug(CF_SOCKET, "Connecting FD %d", sock->fd);
	int32_t res = -1;

	cf_socket_disable_blocking(sock);
	int32_t rv = connect(sock->fd, sa, cf_socket_addr_len(sa));

	if (rv == 0) {
		cf_debug(CF_SOCKET, "FD %d connected [1]", sock->fd);
		res = 0;
		goto cleanup1;
	}

	if (errno != EINPROGRESS) {
		cf_warning(CF_SOCKET, "Error while connecting FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
		goto cleanup1;
	}

	int32_t efd = epoll_create(1);

	if (efd < 0) {
		cf_crash(CF_SOCKET, "epoll_create() failed: %d (%s)", errno, cf_strerror(errno));
	}

	struct epoll_event event = { .data.fd = sock->fd, .events = EPOLLOUT };

	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock->fd, &event) < 0) {
		cf_crash(CF_SOCKET, "epoll_ctl() failed for FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
	}

	int32_t count = safe_wait(efd, &event, 1, timeout);

	if (count == 0) {
		cf_warning(CF_SOCKET, "Timeout while connecting FD %d", sock->fd);
		goto cleanup2;
	}

	int32_t err;
	socklen_t err_len = sizeof(err);
	safe_getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &err, &err_len);

	if (err != 0) {
		cf_warning(CF_SOCKET, "Error while connecting FD %d: %d (%s)",
				sock->fd, err, cf_strerror(err));
		goto cleanup2;
	}

	cf_debug(CF_SOCKET, "FD %d connected [2]", sock->fd);
	res = 0;

cleanup2:
	if (epoll_ctl(efd, EPOLL_CTL_DEL, sock->fd, NULL) < 0) {
		cf_crash(CF_SOCKET, "epoll_ctl() failed for FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
	}

	safe_close(efd);

cleanup1:
	cf_socket_enable_blocking(sock);
	return res;
}

int32_t
cf_socket_init_client(cf_socket_cfg *conf, int32_t timeout)
{
	int32_t res = -1;
	struct sockaddr_storage sas;
	cf_sock_addr addr;

	if (config_address(conf, (struct sockaddr *)&sas, &addr) < 0) {
		goto cleanup0;
	}

	cf_debug(CF_SOCKET, "Initializing client for %s", cf_sock_addr_print(&addr));
	int32_t fd = socket(sas.ss_family, conf->type, 0);

	if (fd < 0) {
		cf_warning(CF_SOCKET, "Error while creating socket for %s:%d: %d (%s)",
				conf->addr, conf->port, errno, cf_strerror(errno));
		goto cleanup0;
	}

	cf_socket *sock = cf_malloc(sizeof(cf_socket));

	if (sock == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
	}

	sock->fd = fd;
	fd = -1;

	cf_socket_fix_client(sock);

	if (connect_socket(sock, (struct sockaddr *)&sas, timeout) < 0) {
		cf_warning(CF_SOCKET, "Error while connecting socket to %s:%d",
				conf->addr, conf->port);
		goto cleanup1;
	}

	cf_socket_disable_nagle(sock);

	conf->sock = sock;
	res = 0;
	goto cleanup0;

cleanup1:
	safe_close(sock->fd);
	cf_free(sock);

cleanup0:
	return res;
}

int32_t
cf_socket_init_client_nb(cf_sock_addr *addr, cf_socket **sock)
{
	int32_t res = -1;

	struct sockaddr_storage sas;
	cf_sock_addr_to_native(addr, (struct sockaddr *)&sas);

	cf_debug(CF_SOCKET, "Initializing non-blocking client for %s", cf_sock_addr_print(addr));

	int32_t fd = socket(sas.ss_family, SOCK_STREAM, 0);

	if (fd < 0) {
		cf_warning(CF_SOCKET, "Error while creating socket for %s: %d (%s)",
				cf_sock_addr_print(addr), errno, cf_strerror(errno));
		goto cleanup0;
	}

	cf_socket *_sock = cf_malloc(sizeof(cf_socket));

	if (_sock == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
	}

	_sock->fd = fd;
	fd = -1;

	cf_socket_fix_client(_sock);
	cf_socket_disable_blocking(_sock);

	if (connect(_sock->fd, (struct sockaddr *)&sas,
			cf_socket_addr_len((struct sockaddr *)&sas)) < 0) {
		if (errno != EINPROGRESS) {
			cf_warning(CF_SOCKET, "Error while connecting socket to %s: %d (%s)",
					cf_sock_addr_print(addr), errno, cf_strerror(errno));
			goto cleanup1;
		}
	}

	*sock = _sock;
	res = 0;
	goto cleanup0;

cleanup1:
	safe_close(_sock->fd);
	cf_free(_sock);

cleanup0:
	return res;
}

int32_t
cf_socket_accept(cf_socket *lsock, cf_socket **sock, cf_sock_addr *addr)
{
	int32_t res = -1;

	struct sockaddr_storage sas;
	struct sockaddr *sa = NULL;
	socklen_t sa_len = 0;

	if (addr != NULL) {
		sa = (struct sockaddr *)&sas;
		sa_len = sizeof(sas);
	}

	int32_t fd = accept(lsock->fd, sa, &sa_len);

	if (fd < 0) {
		cf_debug(CF_SOCKET, "Error while accepting from FD %d: %d (%s)",
				lsock->fd, errno, cf_strerror(errno));
		goto cleanup0;
	}

	if (addr != NULL) {
		cf_sock_addr_from_native(sa, addr);
	}

	cf_socket *_sock = cf_malloc(sizeof(cf_socket));

	if (_sock == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
	}

	_sock->fd = fd;
	fd = -1;

	*sock = _sock;
	res = 0;

cleanup0:
	return res;
}

typedef int32_t (*name_func)(int32_t fd, struct sockaddr *sa, socklen_t *sa_len);

static int32_t
x_name(name_func func, const char *which, int32_t fd, cf_sock_addr *addr)
{
	struct sockaddr_storage sas;
	socklen_t sas_len = sizeof(sas);

	if (func(fd, (struct sockaddr *)&sas, &sas_len) < 0) {
		cf_warning(CF_SOCKET, "Error while getting %s name: %d (%s)",
				which, errno, cf_strerror(errno));
		return -1;
	}

	cf_sock_addr_from_native((struct sockaddr *)&sas, addr);
	return 0;
}

int32_t
cf_socket_remote_name(cf_socket *sock, cf_sock_addr *addr)
{
	return x_name(getpeername, "remote", sock->fd, addr);
}

int32_t
cf_socket_local_name(cf_socket *sock, cf_sock_addr *addr)
{
	return x_name(getsockname, "local", sock->fd, addr);
}

int32_t
cf_socket_available(cf_socket *sock)
{
	int32_t size;
	safe_ioctl(sock->fd, FIONREAD, &size);
	return size;
}

int32_t
cf_socket_send_to(cf_socket *sock, void *buff, size_t size, int32_t flags, cf_sock_addr *addr)
{
	struct sockaddr_storage sas;
	struct sockaddr *sa = NULL;
	socklen_t sa_len = 0;

	if (addr != NULL) {
		cf_sock_addr_to_native(addr, (struct sockaddr *)&sas);
		sa = (struct sockaddr *)&sas;
		sa_len = cf_socket_addr_len((struct sockaddr *)&sas);
	}

	int32_t res = sendto(sock->fd, buff, size, flags | MSG_NOSIGNAL, sa, sa_len);

	if (res < 0) {
		cf_debug(CF_SOCKET, "Error while sending on FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
	}

	return res;
}

int32_t
cf_socket_send(cf_socket *sock, void *buff, size_t size, int32_t flags)
{
	return cf_socket_send_to(sock, buff, size, flags, NULL);
}

int32_t
cf_socket_recv_from(cf_socket *sock, void *buff, size_t size, int32_t flags, cf_sock_addr *addr)
{
	struct sockaddr_storage sas;
	struct sockaddr *sa = NULL;
	socklen_t sa_len = 0;

	if (addr != NULL) {
		sa = (struct sockaddr *)&sas;
		sa_len = sizeof(sas);
	}

	int32_t res = recvfrom(sock->fd, buff, size, flags | MSG_NOSIGNAL, sa, &sa_len);

	if (res < 0) {
		cf_debug(CF_SOCKET, "Error while receiving on FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
	}
	else if (addr != NULL) {
		cf_sock_addr_from_native(sa, addr);
	}

	return res;
}

int32_t
cf_socket_recv(cf_socket *sock, void *buff, size_t size, int32_t flags)
{
	return cf_socket_recv_from(sock, buff, size, flags, NULL);
}

static void
x_shutdown(cf_socket *sock, int32_t how)
{
	if (shutdown(sock->fd, how) < 0) {
		if (errno != ENOTCONN) {
			cf_crash(CF_SOCKET, "shutdown() failed on FD %d: %d (%s)",
					sock->fd, errno, cf_strerror(errno));
		}
		else {
			cf_debug(CF_SOCKET, "shutdown() on disconnected FD %d: %d (%s)",
					sock->fd, errno, cf_strerror(errno));
		}
	}
}

void
cf_socket_write_shutdown(cf_socket *sock)
{
	cf_debug(CF_SOCKET, "Shutting down write channel of FD %d", sock->fd);
	x_shutdown(sock, SHUT_WR);
}

void
cf_socket_shutdown(cf_socket *sock)
{
	cf_debug(CF_SOCKET, "Shutting down FD %d", sock->fd);
	x_shutdown(sock, SHUT_RDWR);
}

void
cf_socket_close(cf_socket *sock)
{
	cf_debug(CF_SOCKET, "Closing FD %d", sock->fd);
	safe_close(sock->fd);
	sock->fd = -1;
	cf_free(sock);
}

void
cf_socket_drain_close(cf_socket *sock)
{
	cf_debug(CF_SOCKET, "Draining and closing FD %d", sock->fd);
	int32_t efd = epoll_create(1);

	if (efd < 0) {
		cf_crash(CF_SOCKET, "epoll_create() failed: %d (%s)", errno, cf_strerror(errno));
	}

	struct epoll_event event = { .data.fd = sock->fd, .events = EPOLLRDHUP };

	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock->fd, &event) < 0) {
		cf_crash(CF_SOCKET, "epoll_ctl() failed for FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
	}

	cf_socket_shutdown(sock);
	int32_t count = safe_wait(efd, &event, 1, 5000);

	if (count == 0) {
		cf_warning(CF_SOCKET, "Timeout while waiting for FD %d to drain", sock->fd);
		goto cleanup1;
	}

	cf_debug(CF_SOCKET, "FD %d drained", sock->fd);

cleanup1:
	if (epoll_ctl(efd, EPOLL_CTL_DEL, sock->fd, NULL) < 0) {
		cf_crash(CF_SOCKET, "epoll_ctl() failed for FD %d: %d (%s)",
				sock->fd, errno, cf_strerror(errno));
	}

	safe_close(efd);
	cf_socket_close(sock);
}

int32_t
cf_socket_mcast_init(cf_socket_mcast_cfg *mconf)
{
	static const int32_t yes = 1;

	int32_t res = -1;

	cf_socket_cfg *conf = &mconf->conf;
	struct sockaddr_storage sas;
	cf_sock_addr addr;

	if (config_address(conf, (struct sockaddr *)&sas, &addr) < 0) {
		goto cleanup0;
	}

	cf_ip_addr _iaddr;
	cf_ip_addr *iaddr = NULL;

	if (mconf->if_addr != NULL) {
		if (cf_ip_addr_from_string(mconf->if_addr, &_iaddr) < 0) {
			cf_warning(CF_SOCKET, "Invalid multicast interface address: %s", mconf->if_addr);
			goto cleanup0;
		}

		iaddr = &_iaddr;
	}

	int32_t fd = socket(sas.ss_family, SOCK_DGRAM, 0);

	if (fd < 0) {
		cf_warning(CF_SOCKET, "Error while creating socket for %s:%d: %d (%s)",
				conf->addr, conf->port, errno, cf_strerror(errno));
		goto cleanup0;
	}

	cf_socket *sock = cf_malloc(sizeof(cf_socket));

	if (sock == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
	}

	sock->fd = fd;
	fd = -1;

	cf_socket_fix_client(sock);
	safe_setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	if (iaddr != NULL) {
		cf_info(CF_SOCKET, "Setting multicast interface address: %s", cf_ip_addr_print(iaddr));

		if (cf_socket_mcast_set_inter(sock, iaddr) < 0) {
			cf_warning(CF_SOCKET, "Error while binding to interface %s", cf_ip_addr_print(iaddr));
			goto cleanup1;
		}
	}

	uint8_t ttl = mconf->ttl;

	if (ttl > 0) {
		cf_info(CF_SOCKET, "Setting multicast TTL: %d", ttl);

		if (cf_socket_mcast_set_ttl(sock, ttl) < 0) {
			cf_warning(CF_SOCKET, "Error while setting multicast TTL");
			goto cleanup1;
		}
	}

	while (bind(sock->fd, (struct sockaddr *)&sas,
			cf_socket_addr_len((struct sockaddr *)&sas)) < 0) {
		if (errno != EADDRINUSE) {
			cf_warning(CF_SOCKET, "Error while binding to %s:%d: %d (%s)",
					conf->addr, conf->port, errno, cf_strerror(errno));
			goto cleanup1;
		}

		cf_warning(CF_SOCKET, "Socket %s:%d in use, waiting", conf->addr, conf->port);
		usleep(5 * 1000 * 1000);
	}

	cf_info(CF_SOCKET, "Joining multicast group: %s", cf_ip_addr_print(&addr.addr));

	if (cf_socket_mcast_join_group(sock, iaddr, &addr.addr) < 0) {
		cf_warning(CF_SOCKET, "Error while joining multicast group %s",
				cf_ip_addr_print(&addr.addr));
		goto cleanup1;
	}

	conf->sock = sock;
	res = 0;
	goto cleanup0;

cleanup1:
	safe_close(sock->fd);
	cf_free(sock);

cleanup0:
	return res;
}

void
cf_socket_mcast_close(cf_socket_mcast_cfg *mconf)
{
	cf_socket_cfg *conf = &mconf->conf;
	safe_close(conf->sock->fd);
	conf->sock->fd = -1;
	cf_free(conf->sock);
}

// #define VERY_CHATTY

void
cf_poll_create(cf_poll *poll)
{
	int32_t fd = epoll_create(1);

	if (fd < 0) {
		cf_crash(CF_SOCKET, "Error while creating epoll instance: %d (%s)",
				errno, cf_strerror(errno));
	}

	*poll = (cf_poll){ .fd = fd };
	cf_debug(CF_SOCKET, "Created new epoll instance with FD %d", fd);
}

void
cf_poll_add_socket(cf_poll poll, cf_socket *sock, uint32_t events, void *data)
{
	cf_debug(CF_SOCKET, "Adding FD %d to epoll instance with FD %d, events = 0x%x",
			sock->fd, poll.fd, events);
	struct epoll_event ev = { .events = events, .data.ptr = data };

	if (epoll_ctl(poll.fd, EPOLL_CTL_ADD, sock->fd, &ev) < 0) {
		cf_crash(CF_SOCKET, "Error while adding FD %d to epoll instance %d: %d (%s)",
				sock->fd, poll.fd, errno, cf_strerror(errno));
	}
}

int32_t
cf_poll_modify_socket_forgiving(cf_poll poll, cf_socket *sock, uint32_t events, void *data,
		int32_t n_err_ok, int32_t *err_ok)
{
#if defined VERY_CHATTY
	cf_detail(CF_SOCKET, "Modifying FD %d in epoll instance with FD %d, events = 0x%x",
			sock.fd, poll.fd, events);
#endif

	struct epoll_event ev = { .events = events, .data.ptr = data };

	if (epoll_ctl(poll.fd, EPOLL_CTL_MOD, sock->fd, &ev) < 0) {
		for (int32_t i = 0; i < n_err_ok; ++i) {
			if (errno == err_ok[i]) {
				return errno;
			}
		}

		cf_crash(CF_SOCKET, "Error while modifying FD %d in epoll instance %d: %d (%s)",
				sock->fd, poll.fd, errno, cf_strerror(errno));
	}

	return 0;
}

int32_t
cf_poll_delete_socket_forgiving(cf_poll poll, cf_socket *sock, int32_t n_err_ok, int32_t *err_ok)
{
	cf_debug(CF_SOCKET, "Deleting FD %d from epoll instance with FD %d", sock->fd, poll.fd);

	if (epoll_ctl(poll.fd, EPOLL_CTL_DEL, sock->fd, NULL) < 0) {
		for (int32_t i = 0; i < n_err_ok; ++i) {
			if (errno == err_ok[i]) {
				return errno;
			}
		}

		cf_crash(CF_SOCKET, "Error while deleting FD %d from epoll instance %d: %d (%s)",
				sock->fd, poll.fd, errno, cf_strerror(errno));
	}

	return 0;
}

int32_t
cf_poll_wait(cf_poll poll, cf_poll_event *events, int32_t limit, int32_t timeout)
{
#if defined VERY_CHATTY
	cf_detail(CF_SOCKET, "Waiting on epoll instance with FD %d", poll.fd);
#endif

	while (true) {
		int32_t res = epoll_wait(poll.fd, (struct epoll_event *)events, limit, timeout);

		if (res >= 0) {
#if defined VERY_CHATTY
			if (cf_fault_filter[CF_SOCKET] >= CF_DETAIL) {
				cf_detail(CF_SOCKET, "Epoll instance with FD %d reports %d event(s)", poll.fd, res);

				for (int32_t i = 0; i < res; ++i) {
					cf_detail(CF_SOCKET, "Event #%d: 0x%x, %p",
							i, events[i].events, events[i].data);
				}
			}
#endif

			return res;
		}

		if (errno != EINTR) {
			cf_warning(CF_SOCKET, "Error while waiting for events on epoll instance %d: %d (%s)",
					poll.fd, errno, cf_strerror(errno));
			return -1;
		}
	}
}

void
cf_poll_destroy(cf_poll poll)
{
	cf_debug(CF_SOCKET, "Destroying epoll instance with FD %d", poll.fd);

	if (close(poll.fd) < 0) {
		cf_crash(CF_SOCKET, "Error while closing epoll instance: %d (%s)",
				errno, cf_strerror(errno));
	}
}

#define RESP_SIZE (2 * 1024 * 1024)
#define MAX_INTERS 50
#define MAX_ADDRS 50

typedef struct inter_entry_s {
	uint32_t index;
	char name[100];
	bool def_route;
	uint32_t mtu;
	uint32_t mac_addr_len;
	uint8_t mac_addr[100];
	uint32_t n_addrs;
	cf_ip_addr addrs[MAX_ADDRS];
} inter_entry;

typedef struct inter_info_s {
	uint32_t n_inters;
	inter_entry inters[MAX_INTERS];
} inter_info;

typedef struct inter_filter_s {
	bool allow_v6;
	bool def_route;
	const char *if_name;
} inter_filter;

typedef struct cb_context_s {
	bool has_label;
	bool has_address;
	bool has_local;
	bool has_index;
	bool has_priority;
	char curr_label[100];
	cf_ip_addr curr_address;
	uint32_t curr_index;
	uint32_t curr_priority;
	bool allow_v6;
	inter_info *inter;
} cb_context;

typedef void (*reset_cb)(cb_context *cont);
typedef void (*data_cb)(cb_context *cont, void *info, int32_t type, void *data, size_t len);
typedef void (*post_cb)(cb_context *cont);

static int32_t
netlink_dump(int32_t type, int32_t filter1, int32_t filter2a, int32_t filter2b, int32_t filter2c,
		size_t size, reset_cb reset_fn, data_cb data_fn, post_cb post_fn, cb_context *cont)
{
	int32_t res = -1;
	int32_t nls = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

	if (nls < 0) {
		cf_warning(CF_SOCKET, "Error while creating netlink socket: %d (%s)",
				errno, cf_strerror(errno));
		goto cleanup0;
	}

	struct sockaddr_nl loc;
	memset(&loc, 0, sizeof(loc));
	loc.nl_family = AF_NETLINK;

	if (bind(nls, (struct sockaddr *)&loc, sizeof(loc)) < 0) {
		cf_warning(CF_SOCKET, "Error while binding netlink socket: %d (%s)",
				errno, cf_strerror(errno));
		goto cleanup1;
	}

	static cf_atomic32 seq = 0;
	struct {
		struct nlmsghdr h;
		struct rtgenmsg m;
	} req;

	memset(&req, 0, sizeof(req));
	req.h.nlmsg_len = NLMSG_LENGTH(sizeof(req.m));
	req.h.nlmsg_type = type;
	req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	req.h.nlmsg_seq = cf_atomic32_add(&seq, 1);
	req.m.rtgen_family = PF_UNSPEC;

	struct sockaddr_nl rem;
	memset(&rem, 0, sizeof(rem));
	rem.nl_family = AF_NETLINK;

	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = &req;
	iov.iov_len = req.h.nlmsg_len;

	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_name = &rem;
	msg.msg_namelen = sizeof(rem);

	if (sendmsg(nls, &msg, 0) < 0) {
		cf_warning(CF_SOCKET, "Error while sending netlink request: %d (%s)",
				errno, cf_strerror(errno));
		goto cleanup1;
	}

	uint8_t *resp = cf_malloc(RESP_SIZE);

	if (resp == NULL) {
		cf_crash(CF_SOCKET, "Out of memory");
		goto cleanup1;
	}

	memset(resp, 0, RESP_SIZE);
	bool done = false;

	while (!done) {
		memset(&rem, 0, sizeof(rem));
		memset(&iov, 0, sizeof(iov));
		iov.iov_base = resp;
		iov.iov_len = RESP_SIZE;

		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_name = &rem;
		msg.msg_namelen = sizeof(rem);

		ssize_t len = recvmsg(nls, &msg, 0);

		if (len < 0) {
			cf_warning(CF_SOCKET, "Error while receiving netlink response: %d (%s)",
					errno, cf_strerror(errno));
			goto cleanup2;
		}

		if ((msg.msg_flags & MSG_TRUNC) != 0) {
			cf_warning(CF_SOCKET, "Received truncated netlink message");
			goto cleanup2;
		}

		struct nlmsghdr *h = (struct nlmsghdr *)resp;

		while (NLMSG_OK(h, len)) {
			if (h->nlmsg_type == NLMSG_NOOP) {
				h = NLMSG_NEXT(h, len);
				continue;
			}

			if (h->nlmsg_type == NLMSG_ERROR) {
				cf_warning(CF_SOCKET, "Received netlink error message");
				goto cleanup2;
			}

			if (h->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}

			if (h->nlmsg_type == NLMSG_OVERRUN) {
				cf_warning(CF_SOCKET, "Received netlink overrun message");
				goto cleanup2;
			}

			if (h->nlmsg_type == filter1) {
				if (reset_fn != NULL) {
					reset_fn(cont);
				}

				void *info = NLMSG_DATA(h);
				uint32_t a_len = h->nlmsg_len - NLMSG_LENGTH(size);
				struct rtattr *a = (struct rtattr *)((uint8_t *)info + NLMSG_ALIGN(size));

				while (RTA_OK(a, a_len)) {
					if (a->rta_type == filter2a || a->rta_type == filter2b ||
							a->rta_type == filter2c) {
						data_fn(cont, info, a->rta_type, RTA_DATA(a), RTA_PAYLOAD(a));
					}

					a = RTA_NEXT(a, a_len);
				}

				if (post_fn != NULL) {
					post_fn(cont);
				}
			}

			if ((h->nlmsg_flags & NLM_F_MULTI) == 0) {
				done = true;
				break;
			}

			h = NLMSG_NEXT(h, len);
		}
	}

	res = 0;

cleanup2:
	free(resp);

cleanup1:
	close(nls);

cleanup0:
	return res;
}

static void
reset_fn(cb_context *cont)
{
	cont->has_label = false;
	cont->has_address = false;
	cont->has_local = false;
	cont->has_index = false;
	cont->has_priority = false;
	memset(&cont->curr_label, 0, sizeof(cont->curr_label));
	cf_ip_addr_set_zero(&cont->curr_address);
	cont->curr_index = 0;
	cont->curr_priority = 0;
}

static void
link_fn(cb_context *cont, void *info_, int32_t type, void *data, size_t len)
{
	struct ifinfomsg *info = info_;
	inter_info *inter = cont->inter;
	inter_entry *entry = NULL;

	for (uint32_t i = 0; i < inter->n_inters; ++i) {
		if (inter->inters[i].index == info->ifi_index) {
			entry = &inter->inters[i];
			break;
		}
	}

	if (entry == NULL) {
		uint32_t i = inter->n_inters;

		if (i >= MAX_INTERS) {
			cf_crash(CF_SOCKET, "Too many interfaces");
		}

		entry = &inter->inters[i];
		++inter->n_inters;

		entry->index = info->ifi_index;
	}

	if (type == IFLA_IFNAME) {
		if (len > sizeof(entry->name)) {
			cf_crash(CF_SOCKET, "Interface name too long: %s", (char *)data);
		}

		// Length includes terminating NUL.
		memcpy(entry->name, data, len);
		cf_detail(CF_SOCKET, "Collected interface name %s", entry->name);
	}
	else if (type == IFLA_ADDRESS) {
		if (len > sizeof(entry->mac_addr)) {
			cf_crash(CF_SOCKET, "MAC address too long");
		}

		entry->mac_addr_len = (uint32_t)len;
		memcpy(entry->mac_addr, data, len);
	}
	else if (type == IFLA_MTU) {
		if (len != 4) {
			cf_crash(CF_SOCKET, "MTU value has invalid length: %zu", len);
		}

		memcpy(&entry->mtu, data, len);
		cf_detail(CF_SOCKET, "Collected interface MTU %s -> %u", entry->name, entry->mtu);
	}
}

static void
addr_fn(cb_context *cont, void *info_, int32_t type, void *data, size_t len)
{
	struct ifaddrmsg *info = info_;

	if (cont->curr_index == 0) {
		cont->curr_index = info->ifa_index;
	}

	if (type == IFA_LABEL) {
		if (len > sizeof(cont->curr_label)) {
			cf_crash(CF_SOCKET, "Interface label too long: %s", (char *)data);
		}

		// Length includes terminating NUL.
		memcpy(cont->curr_label, data, len);
		cont->has_label = true;
		cf_detail(CF_SOCKET, "Collected interface label %s", cont->curr_label);
	}
	else if (type == IFA_ADDRESS) {
		// IFA_LOCAL takes precedence over IFA_ADDRESS.
		if (cont->has_local) {
			cf_detail(CF_SOCKET, "Prioritizing local address");
			return;
		}

		if (cf_socket_parse_netlink(cont->allow_v6, info->ifa_family, info->ifa_flags,
				data, len, &cont->curr_address) < 0) {
			return;
		}

		cont->has_address = true;
		cf_detail(CF_SOCKET, "Considering interface address %s",
				cf_ip_addr_print(&cont->curr_address));
	}
	else if (type == IFA_LOCAL) {
		if (cf_socket_parse_netlink(cont->allow_v6, info->ifa_family, info->ifa_flags,
				data, len, &cont->curr_address) < 0) {
			return;
		}

		cont->has_local = true;
		cf_detail(CF_SOCKET, "Considering local interface address %s",
				cf_ip_addr_print(&cont->curr_address));
	}
}

static void
addr_fix_fn(cb_context *cont)
{
	if (!cont->has_address && !cont->has_local) {
		return;
	}

	inter_info *inter = cont->inter;
	inter_entry *by_index = NULL;
	inter_entry *by_label = NULL;

	for (uint32_t i = 0; i < inter->n_inters; ++i) {
		if (inter->inters[i].index == cont->curr_index) {
			by_index = &inter->inters[i];
			break;
		}
	}

	if (by_index == NULL) {
		cf_crash(CF_SOCKET, "Invalid interface index: %u", cont->curr_index);
	}

	if (cont->has_label) {
		for (uint32_t i = 0; i < inter->n_inters; ++i) {
			if (strcmp(inter->inters[i].name, cont->curr_label) == 0) {
				by_label = &inter->inters[i];
				break;
			}
		}

		if (by_label == NULL) {
			cf_detail(CF_SOCKET, "New interface for label %s", cont->curr_label);
			uint32_t i = inter->n_inters;

			if (i >= MAX_INTERS) {
				cf_crash(CF_SOCKET, "Too many interfaces");
			}

			by_label = &inter->inters[i];
			++inter->n_inters;

			by_label->index = by_index->index;
			memcpy(&by_label->mac_addr, &by_index->mac_addr, sizeof(by_label->mac_addr));
			by_label->mac_addr_len = by_index->mac_addr_len;
			by_label->mtu = by_index->mtu;

			memcpy(&by_label->name, cont->curr_label, sizeof(by_label->name));
		}
	}

	inter_entry *entry = by_label != NULL ? by_label : by_index;
	uint32_t i = entry->n_addrs;

	if (i >= MAX_ADDRS) {
		cf_crash(CF_SOCKET, "Too many addresses for interface %s", entry->name);
	}

	cf_ip_addr *addr = &entry->addrs[i];
	cf_ip_addr_copy(&cont->curr_address, addr);

	++entry->n_addrs;
	cf_detail(CF_SOCKET, "Collected interface address %s -> %s",
			entry->name, cf_ip_addr_print(addr));
}

static void
route_fn(cb_context *cont, void *info_, int32_t type, void *data, size_t len)
{
	struct rtmsg *info = info_;

	// Ignore entries with RTM_F_CLONED, because they are route cache entries.
	if ((info->rtm_flags & RTM_F_CLONED) != 0) {
		return;
	}

	if (type == RTA_DST) {
		if (cf_socket_parse_netlink(cont->allow_v6, info->rtm_family, 0,
				data, len, &cont->curr_address) < 0) {
			// If the address is not allowed, set to a non-zero address, because
			// zero means default route.
			cf_ip_addr_set_loopback(&cont->curr_address);
		}

		cont->has_address = true;
	}
	else if (type == RTA_OIF) {
		if (len != 4) {
			cf_detail(CF_SOCKET, "Invalid interface index");
			return;
		}

		cont->curr_index = *(uint32_t *)data;
		cont->has_index = true;
	}
	else if (type == RTA_PRIORITY) {
		if (len != 4) {
			cf_detail(CF_SOCKET, "Invalid route priority");
			return;
		}

		cont->curr_priority = *(uint32_t *)data;
		cont->has_priority = true;
	}
}

static void
route_fix_fn(cb_context *cont)
{
	// It's not a default route, if it has an address and the address isn't zero.
	if (cont->has_address && !cf_ip_addr_is_zero(&cont->curr_address)) {
		return;
	}

	// It's one of the catch-all entries.
	if (cont->has_priority && cont->curr_priority == UINT32_MAX) {
		return;
	}

	// It doesn't have an interface index.
	if (!cont->has_index) {
		return;
	}

	inter_info *inter = cont->inter;
	bool found = false;

	for (uint32_t i = 0; i < inter->n_inters; ++i) {
		inter_entry *entry = &inter->inters[i];

		if (inter->inters[i].index == cont->curr_index) {
			found = true;
			entry->def_route = true;
			cf_detail(CF_SOCKET, "Collected default route %s -> %s",
					entry->name, cf_ip_addr_print(&cont->curr_address));
			// Don't stop after the first match. Aliases share the same index.
		}
	}

	if (!found) {
		cf_crash(CF_SOCKET, "Invalid interface index: %u", cont->curr_index);
	}
}

static void
enumerate_inter(inter_info *inter, bool allow_v6)
{
	cb_context cont;
	memset(&cont, 0, sizeof(cont));
	cont.inter = inter;
	cont.allow_v6 = allow_v6;

	reset_fn(&cont);

	if (netlink_dump(RTM_GETLINK, RTM_NEWLINK, IFLA_IFNAME, IFLA_ADDRESS, IFLA_MTU,
			sizeof(struct ifinfomsg), NULL, link_fn, NULL, &cont) < 0) {
		cf_crash(CF_SOCKET, "Error while enumerating network links");
	}

	if (netlink_dump(RTM_GETADDR, RTM_NEWADDR, IFA_LABEL, IFA_ADDRESS, IFA_LOCAL,
			sizeof(struct ifaddrmsg), reset_fn, addr_fn, addr_fix_fn, &cont) < 0) {
		cf_crash(CF_SOCKET, "Error while enumerating network addresses");
	}

	if (netlink_dump(RTM_GETROUTE, RTM_NEWROUTE, RTA_DST, RTA_OIF, RTA_PRIORITY,
			sizeof(struct rtmsg), reset_fn, route_fn, route_fix_fn, &cont) < 0) {
		cf_crash(CF_SOCKET, "Error while enumerating network routes");
	}

	for (int32_t i = 0; i < inter->n_inters; ++i) {
		inter_entry *entry = &inter->inters[i];
		cf_ip_addr_sort(entry->addrs, entry->n_addrs);
	}

	if (cf_fault_filter[CF_SOCKET] >= CF_DETAIL) {
		cf_detail(CF_SOCKET, "%u interface(s)", inter->n_inters);

		for (uint32_t i = 0; i < inter->n_inters; ++i) {
			inter_entry *entry = &inter->inters[i];
			cf_detail(CF_SOCKET, "Name = %s", entry->name);
			cf_detail(CF_SOCKET, "MAC address = %02x:%02x:%02x:%02x:%02x:%02x",
					entry->mac_addr[0], entry->mac_addr[1], entry->mac_addr[2],
					entry->mac_addr[3], entry->mac_addr[4], entry->mac_addr[5]);
			cf_detail(CF_SOCKET, "Default route = %d", (int32_t)entry->def_route);
			cf_detail(CF_SOCKET, "MTU = %u", entry->mtu);

			for (int32_t k = 0; k < entry->n_addrs; ++k) {
				cf_ip_addr *addr = &entry->addrs[k];
				cf_detail(CF_SOCKET, "Address = %s", cf_ip_addr_print(addr));
			}
		}
	}

	// -------------------- BEGIN PARANOIA --------------------

	// This double-checks that our new method returns interfaces in exactly the
	// same order as does glibc.

	struct ifaddrs *legacy;

	if (getifaddrs(&legacy) < 0) {
		cf_crash(CF_SOCKET, "Error while legacy-enumerating interfaces: %d (%s)",
				errno, cf_strerror(errno));
	}

	uint32_t n = 0;

	for (struct ifaddrs *it = legacy; it != NULL; it = it->ifa_next) {
		cf_detail(CF_SOCKET, "Checking legacy-enumerated interface %s", it->ifa_name);
		bool found = false;

		for (uint32_t i = 0; i < n; ++i) {
			inter_entry *entry = &inter->inters[i];

			if (strcmp(entry->name, it->ifa_name) == 0) {
				cf_detail(CF_SOCKET, "Interface name matches a previous name");
				found = true;
				break;
			}
		}

		if (found) {
			continue;
		}

		cf_detail(CF_SOCKET, "Encountered new interface name");

		if (n == inter->n_inters) {
			cf_crash(CF_SOCKET, "Missed legacy-enumerated interface %s", it->ifa_name);
		}

		inter_entry *entry = &inter->inters[n];
		cf_detail(CF_SOCKET, "Expecting interface name %s", entry->name);

		if (strcmp(entry->name, it->ifa_name) != 0) {
			cf_crash(CF_SOCKET, "Unexpected legacy-enumerated interface %s", it->ifa_name);
		}

		++n;
	}

	if (n < inter->n_inters) {
		inter_entry *entry = &inter->inters[n];
		cf_crash(CF_SOCKET, "Extraneous interface %s", entry->name);
	}

	freeifaddrs(legacy);

	// --------------------- END PARANOIA ---------------------
}

static int32_t
inter_get_addr(cf_ip_addr *addrs, uint32_t *n_addrs, inter_filter *filter)
{
	inter_info inter;
	memset(&inter, 0, sizeof(inter));
	enumerate_inter(&inter, filter->allow_v6);

	uint32_t count = 0;

	for (uint32_t i = 0; i < inter.n_inters; ++i) {
		inter_entry *entry = &inter.inters[i];

		if (filter->def_route && !entry->def_route) {
			continue;
		}

		if (filter->if_name != NULL && strcmp(filter->if_name, entry->name) != 0) {
			continue;
		}

		for (uint32_t k = 0; k < entry->n_addrs; ++k) {
			cf_ip_addr *addr = &entry->addrs[k];

			if (count >= *n_addrs) {
				cf_warning(CF_SOCKET, "Buffer overflow while enumerating interface addresses");
				return -1;
			}

			cf_ip_addr_copy(addr, &addrs[count]);
			++count;
		}
	}

	*n_addrs = count;
	return 0;
}

int32_t
cf_inter_get_addr_all(cf_ip_addr *addrs, uint32_t *n_addrs)
{
	static inter_filter filter = {
		.allow_v6 = true, .def_route = false, .if_name = NULL
	};

	return inter_get_addr(addrs, n_addrs, &filter);
}

int32_t
cf_inter_get_addr_def(cf_ip_addr *addrs, uint32_t *n_addrs)
{
	static inter_filter filter = {
		.allow_v6 = true, .def_route = true, .if_name = NULL
	};

	return inter_get_addr(addrs, n_addrs, &filter);
}

int32_t
cf_inter_get_addr_def_legacy(cf_ip_addr *addrs, uint32_t *n_addrs)
{
	static inter_filter filter = {
		.allow_v6 = false, .def_route = true, .if_name = NULL
	};

	return inter_get_addr(addrs, n_addrs, &filter);
}

int32_t
cf_inter_get_addr_name(cf_ip_addr *addrs, uint32_t *n_addrs, const char *if_name)
{
	inter_filter filter = {
		.allow_v6 = true, .def_route = false, .if_name = if_name
	};

	return inter_get_addr(addrs, n_addrs, &filter);
}

bool cf_inter_is_inter_name(const char *if_name)
{
	inter_info inter;
	memset(&inter, 0, sizeof(inter));
	enumerate_inter(&inter, true);

	for (uint32_t i = 0; i < inter.n_inters; ++i) {
		if (strcmp(inter.inters[i].name, if_name) == 0) {
			return true;
		}
	}

	return false;
}

int32_t
cf_inter_addr_to_index_and_name(const cf_ip_addr *addr, int32_t *index, char **name)
{
	inter_info inter;
	memset(&inter, 0, sizeof(inter));
	enumerate_inter(&inter, true);

	for (uint32_t i = 0; i < inter.n_inters; ++i) {
		inter_entry *entry = &inter.inters[i];

		for (uint32_t k = 0; k < entry->n_addrs; ++k) {
			if (cf_ip_addr_compare(&entry->addrs[k], addr) == 0) {
				if (name != NULL) {
					*name = safe_strdup(entry->name);
				}

				if (index != NULL) {
					*index = (int32_t)entry->index;
				}

				return 0;
			}
		}
	}

	return -1;
}

int32_t
cf_inter_mtu(const cf_ip_addr *inter_addr)
{
	inter_info inter;
	memset(&inter, 0, sizeof(inter));
	enumerate_inter(&inter, true);

	for (uint32_t i = 0; i < inter.n_inters; ++i) {
		inter_entry *entry = &inter.inters[i];

		for (uint32_t k = 0; k < entry->n_addrs; ++k) {
			cf_ip_addr *entry_addr = &entry->addrs[k];

			if (cf_ip_addr_compare(inter_addr, entry_addr) == 0) {
				return entry->mtu;
			}
		}
	}

	return -1;
}

int32_t
cf_inter_min_mtu(void)
{
	uint32_t min = UINT32_MAX;
	inter_info inter;
	memset(&inter, 0, sizeof(inter));
	enumerate_inter(&inter, true);

	for (uint32_t i = 0; i < inter.n_inters; ++i) {
		inter_entry *entry = &inter.inters[i];

		if (entry->mtu < min) {
			min = entry->mtu;
		}
	}

	return (int32_t)min;
}

static const char *if_in_order[] = {
	"eth", "bond", "wlan",
	NULL
};

static const char *if_default[] = {
	"^eth[[:digit:]]+$", "^bond[[:digit:]]+$", "^wlan[[:digit:]]+$",
	"^em[[:digit:]]+_[[:digit:]]+$", "^p[[:digit:]]+p[[:digit:]]+_[[:digit:]]+$",
	NULL
};

static const char *if_default2[] = {
	"^em[[:digit:]]+$", "^p[[:digit:]]+p[[:digit:]]+$", NULL
};

static const char *if_any[] = {
	"^.*$",
	NULL
};

static bool
validate_inter(inter_entry *entry)
{
	cf_debug(CF_SOCKET, "Validating interface %s", entry->name);

	if (entry->n_addrs == 0) {
		cf_debug(CF_SOCKET, "No IP addresses");
		return false;
	}

	if (entry->mac_addr_len != 6) {
		cf_debug(CF_SOCKET, "Invalid MAC address length: %d", entry->mac_addr_len);
		return false;
	}

	static const uint8_t all0[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t all1[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	if (memcmp(entry->mac_addr, all0, 6) == 0 || memcmp(entry->mac_addr, all1, 6) == 0) {
		cf_debug(CF_SOCKET, "Invalid MAC address: %02x:%02x:%02x:%02x:%02x:%02x",
				entry->mac_addr[0], entry->mac_addr[1], entry->mac_addr[2],
				entry->mac_addr[3], entry->mac_addr[4], entry->mac_addr[5]);
		return false;
	}

	cf_debug(CF_SOCKET, "Interface OK");
	return true;
}

static inter_entry *
find_inter(inter_info *inter, const char *name, bool validate)
{
	cf_debug(CF_SOCKET, "Looking for %s", name);

	for (uint32_t i = 0; i < inter->n_inters; ++i) {
		inter_entry *entry = &inter->inters[i];
		cf_debug(CF_SOCKET, "Checking %s", entry->name);

		if (strcmp(entry->name, name) == 0 && (!validate || validate_inter(entry))) {
			return entry;
		}
	}

	return NULL;
}

static inter_entry *
match_inter(inter_info *inter, const char **patterns)
{
	for (uint32_t i = 0; i < inter->n_inters; ++i) {
		inter_entry *entry = &inter->inters[i];
		cf_debug(CF_SOCKET, "Matching %s", entry->name);

		for (uint32_t k = 0; patterns[k] != NULL; ++k) {
			cf_debug(CF_SOCKET, "Matching with %s", patterns[k]);
			regex_t rex;

			if (regcomp(&rex, patterns[k], REG_EXTENDED | REG_NOSUB) != 0) {
				cf_crash(CF_SOCKET, "Error while compiling regular expression %s", patterns[k]);
			}

			bool ok = regexec(&rex, entry->name, 0, NULL, 0) == 0 && validate_inter(entry);
			regfree(&rex);

			if (ok) {
				return entry;
			}
		}
	}

	return NULL;
}

int32_t
cf_node_id_get(cf_ip_port port, const char *if_hint, cf_node *id, char **ip_addr)
{
	cf_debug(CF_SOCKET, "Getting node ID");
	inter_info inter;
	memset(&inter, 0, sizeof(inter));
	enumerate_inter(&inter, true);

	inter_entry *entry;

	if (if_hint != NULL) {
		cf_debug(CF_SOCKET, "Checking user-specified interface %s", if_hint);
		entry = find_inter(&inter, if_hint, false);

		if (entry != NULL) {
			goto success;
		}

		cf_warning(CF_SOCKET, "Unable to find interface %s specified in configuration file",
				if_hint);
		return -1;
	}

	cf_debug(CF_SOCKET, "Trying default interfaces in order");

	for (int32_t i = 0; if_in_order[i] != NULL; ++i) {
		for (int32_t k = 0; k < 11; ++k) {
			char tmp[100];
			snprintf(tmp, sizeof(tmp), "%s%d", if_in_order[i], k);
			entry = find_inter(&inter, tmp, true);

			if (entry != NULL) {
				goto success;
			}
		}
	}

	cf_debug(CF_SOCKET, "Trying default interfaces");
	entry = match_inter(&inter, if_default);

	if (entry != NULL) {
		goto success;
	}

	cf_debug(CF_SOCKET, "Trying secondary default interfaces");
	entry = match_inter(&inter, if_default2);

	if (entry != NULL) {
		goto success;
	}

	cf_debug(CF_SOCKET, "Trying any interface");
	entry = match_inter(&inter, if_any);

	if (entry != NULL) {
		goto success;
	}

	cf_warning(CF_SOCKET, "Unable to find any suitable network device for node ID");
	return -1;

success:
	;
	uint8_t *buff = (uint8_t *)id;
	memcpy(buff, entry->mac_addr, 6);
	memcpy(buff + 6, &port, 2);

	char tmp[1000];
	cf_ip_addr_to_string_safe(&entry->addrs[0], tmp, sizeof(tmp));
	*ip_addr = cf_strdup(tmp);

	cf_info(CF_SOCKET, "Node port %d, node ID %" PRIx64 ", node IP address %s", port, *id, tmp);
	return 0;
}
