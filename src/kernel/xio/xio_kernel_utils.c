/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "xio_os.h"
#include "libxio.h"
#include "xio_common.h"
#include "xio_protocol.h"

#include <linux/kernel.h>
#include <linux/topology.h>
#include <linux/inet.h>

#ifndef IN6ADDR_ANY_INIT
#define IN6ADDR_ANY_INIT { { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } }
#endif

static int priv_parse_ip_addr(const char *str, size_t len, __be16 port,
			      struct sockaddr_storage *ss)
{
	const char *end;

	if (strnchr(str, len, '.')) {
		/* Try IPv4 */
		struct sockaddr_in *s4 = (struct sockaddr_in *) ss;
		if (in4_pton(str, len, (void *)&s4->sin_addr, -1, &end) > 0) {
			if (!*end) {
				/* reached the '\0' */
				s4->sin_family = AF_INET;
				s4->sin_port = port;
				return 0;
			}
		}
	} else if (strnchr(str, len, ':')) {
		/* Try IPv6 */
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *) ss;
		if (in6_pton(str, -1, (void *)&s6->sin6_addr, -1, &end) > 0) {
			if (!*end) {
				/* reached the '\0' */
				/* what about scope and flow */
				s6->sin6_family = AF_INET6;
				s6->sin6_port = port;
				return 1;
			}
		}
	}
	return -1;
}

#define NI_MAXSERV 32
#define NI_MAXHOST 1025

/*---------------------------------------------------------------------------*/
/* xio_uri_to_ss							     */
/*---------------------------------------------------------------------------*/
int xio_uri_to_ss(const char *uri, struct sockaddr_storage *ss)
{
	char		*start;
	char		host[NI_MAXHOST];
	char		port[NI_MAXSERV];
	unsigned long	portul;
	unsigned short	port16;
	__be16		port_be16;
	const char	*p1, *p2;
	size_t		len;
	int		ipv6_hint = 0;

	/* only supported protocol is rdma */
	start = strstr(uri, "://");
	if (start == NULL)
		return -1;

	if (*(start+3) == '[') {  /* IPv6 */
		ipv6_hint = 1;
		p1 = strstr(start + 3, "]:");
		if (p1 == NULL)
			return -1;

		len = p1-(start+4);
		strncpy(host, (start + 4), len);
		host[len] = 0;

		p2 = strchr(p1 + 2, '/');
		if (p2 == NULL) {
			strcpy(port, p1 + 2);
		} else {
			len = (p2-1)-(p1+2);
			strncpy(port, (p1 + 2), len);
			port[len] = 0;
		}
	} else {
		/* extract the resource */
		p1 = uri + strlen(uri);
		p2 = NULL;
		while (p1 != (start + 3)) {
			if (*p1 == '/')
				p2 = p1;
			p1--;
			if (p1 == uri)
				return  -1;
		}

		if (p2 == NULL) { /* no resource */
			p1 = strrchr(uri, ':');
			if (p1 == NULL || p1 == start)
				return -1;
			strcpy(port, (p1 + 1));
		} else {
			if (*p2 != '/')
				return -1;
			p1 = p2;
			while (*p1 != ':') {
				p1--;
				if (p1 == uri)
					return  -1;
			}

			len = p2 - (p1 + 1);

			strncpy(port, p1 + 1, len);
			port[len] = 0;
		}
		len = p1 - (start + 3);

		/* extract the address */
		strncpy(host, (start + 3), len);
		host[len] = 0;
	}

	/* debug */
	printk("host:%s port:%s\n", host, port);

	if (strict_strtoul(port, 10, &portul)) {
		ERROR_LOG("Invalid port specification(%s)\n", port);
		return -1;
	}
	if (portul > 0xFFFF) {
		ERROR_LOG("Invalid port specification(%s)\n", port);
		return -1;
	}
	port16 = portul;
	port_be16 = htons(port16);

	if (host[0] == '*' || host[0] == 0) {
		if (ipv6_hint) {
			struct sockaddr_in6 *s6 = (struct sockaddr_in6 *) ss;
			/* what about scope and flow */
			s6->sin6_family = AF_INET6;
			/* s6->sin6_addr	= IN6ADDR_ANY_INIT; */
			memset((void*) &s6->sin6_addr, 0, sizeof(s6->sin6_addr));
			s6->sin6_port	= port_be16;
		} else {
			struct sockaddr_in *s4 = (struct sockaddr_in *) ss;
			s4->sin_family		= AF_INET;
			s4->sin_addr.s_addr	= INADDR_ANY;
			s4->sin_port		= port_be16;
		}
	} else {
		if (priv_parse_ip_addr(host, len, port_be16, ss) < 0) {
			ERROR_LOG("unresolved address\n");
			return -1;
		}
	}

	return 0;
}

/*
 * xio_get_nodeid(cpuid) - This will return the node to which selected cpu
 * belongs
 */
unsigned int xio_get_nodeid(unsigned int cpu_id)
{
	return cpu_to_node(cpu_id);
}