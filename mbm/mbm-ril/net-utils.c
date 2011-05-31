/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); 
 * you may not use this file except in compliance with the License. 
 * You may obtain a copy of the License at 
 *
 *     http://www.apache.org/licenses/LICENSE-2.0 
 *
 * Unless required by applicable law or agreed to in writing, software 
 * distributed under the License is distributed on an "AS IS" BASIS, 
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
 * See the License for the specific language governing permissions and 
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/route.h>
#include <linux/wireless.h>

#define LOG_TAG "mbm-netutils"
#include <cutils/log.h>
#include <cutils/properties.h>

static int ifc_ctl_sock = -1;

static const char *ipaddr_to_string(in_addr_t addr)
{
    struct in_addr in_addr;

    in_addr.s_addr = addr;
    return inet_ntoa(in_addr);
}

int ifc_init(void)
{
    if (ifc_ctl_sock == -1) {
	ifc_ctl_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (ifc_ctl_sock < 0) {
	    LOGE("socket() failed: %s\n", strerror(errno));
	}
    }
    return ifc_ctl_sock < 0 ? -1 : 0;
}

void ifc_close(void)
{
    if (ifc_ctl_sock != -1) {
	(void) close(ifc_ctl_sock);
	ifc_ctl_sock = -1;
    }
}

static void ifc_init_ifr(const char *name, struct ifreq *ifr)
{
    memset(ifr, 0, sizeof(struct ifreq));
    strncpy(ifr->ifr_name, name, IFNAMSIZ);
    ifr->ifr_name[IFNAMSIZ - 1] = 0;
}

static int ifc_set_flags(const char *name, unsigned set, unsigned clr)
{
    struct ifreq ifr;
    ifc_init_ifr(name, &ifr);

    if (ioctl(ifc_ctl_sock, SIOCGIFFLAGS, &ifr) < 0)
	return -1;
    ifr.ifr_flags = (ifr.ifr_flags & (~clr)) | set;
    return ioctl(ifc_ctl_sock, SIOCSIFFLAGS, &ifr);
}

int ifc_up(const char *name)
{
    return ifc_set_flags(name, IFF_UP, 0);
}

int ifc_down(const char *name)
{
    return ifc_set_flags(name, 0, IFF_UP);
}

static void init_sockaddr_in(struct sockaddr *sa, in_addr_t addr)
{
    struct sockaddr_in *sin = (struct sockaddr_in *) sa;
    sin->sin_family = AF_INET;
    sin->sin_port = 0;
    sin->sin_addr.s_addr = addr;
}

int ifc_set_addr(const char *name, in_addr_t addr)
{
    struct ifreq ifr;

    ifc_init_ifr(name, &ifr);
    init_sockaddr_in(&ifr.ifr_addr, addr);

    return ioctl(ifc_ctl_sock, SIOCSIFADDR, &ifr);
}

int ifc_set_mask(const char *name, in_addr_t mask)
{
    struct ifreq ifr;

    ifc_init_ifr(name, &ifr);
    init_sockaddr_in(&ifr.ifr_addr, mask);

    return ioctl(ifc_ctl_sock, SIOCSIFNETMASK, &ifr);
}

in_addr_t get_ipv4_netmask(int prefix_length)
{
    in_addr_t mask = 0;

    mask = ~mask << (32 - prefix_length);
    mask = htonl(mask);

    return mask;
}

int ifc_add_ipv4_route(const char *ifname, struct in_addr dst,
		       int prefix_length, struct in_addr gw)
{
    struct rtentry rt;
    int result;
    in_addr_t netmask;

    memset(&rt, 0, sizeof(rt));

    rt.rt_dst.sa_family = AF_INET;
    rt.rt_dev = (void *) ifname;

    netmask = get_ipv4_netmask(prefix_length);
    init_sockaddr_in(&rt.rt_genmask, netmask);
    init_sockaddr_in(&rt.rt_dst, dst.s_addr);
    rt.rt_flags = RTF_UP;

    if (prefix_length == 32) {
	rt.rt_flags |= RTF_HOST;
    }

    if (gw.s_addr != 0) {
	rt.rt_flags |= RTF_GATEWAY;
	init_sockaddr_in(&rt.rt_gateway, gw.s_addr);
    }

    ifc_init();

    if (ifc_ctl_sock < 0) {
	return -errno;
    }

    result = ioctl(ifc_ctl_sock, SIOCADDRT, &rt);
    if (result < 0) {
	if (errno == EEXIST) {
	    result = 0;
	} else {
	    result = -errno;
	}
    }
    ifc_close();
    return result;
}

int ifc_create_default_route(const char *name, in_addr_t gw)
{
    struct in_addr in_dst, in_gw;

    in_dst.s_addr = 0;
    in_gw.s_addr = gw;

    return ifc_add_ipv4_route(name, in_dst, 0, in_gw);
}

int ifc_add_host_route(const char *name, in_addr_t dst)
{
    struct in_addr in_dst, in_gw;

    in_dst.s_addr = dst;
    in_gw.s_addr = 0;

    return ifc_add_ipv4_route(name, in_dst, 32, in_gw);
}


int ifc_configure(const char *ifname,
		  in_addr_t address,
		  in_addr_t gateway, in_addr_t dns1, in_addr_t dns2)
{

    char dns_prop_name[PROPERTY_KEY_MAX];
    in_addr_t netmask = ~0;

    ifc_init();

    if (ifc_up(ifname)) {
	LOGE("ifc_configure: failed to turn on interface %s: %s\n", ifname,
	     strerror(errno));
	ifc_close();
	return -1;
    }
    if (ifc_set_addr(ifname, address)) {
	LOGE("ifc_configure: failed to set ipaddr %s: %s\n",
	     ipaddr_to_string(address), strerror(errno));
	ifc_down(ifname);
	ifc_close();
	return -1;
    }
    if (ifc_set_mask(ifname, netmask)) {
	LOGE("ifc_configure: failed to set netmask %s: %s\n",
	     ipaddr_to_string(netmask), strerror(errno));
	ifc_down(ifname);
	ifc_close();
	return -1;
    }
    if (ifc_add_host_route(ifname, gateway)) {
	LOGE("ifc_configure: failed to set default route %s: %s\n",
	     ipaddr_to_string(gateway), strerror(errno));
	ifc_down(ifname);
	ifc_close();
	return -1;
    }

    ifc_close();

    snprintf(dns_prop_name, sizeof(dns_prop_name), "net.%s.gw", ifname);
    property_set(dns_prop_name, gateway ? ipaddr_to_string(gateway) : "");

    snprintf(dns_prop_name, sizeof(dns_prop_name), "net.%s.dns1", ifname);
    property_set(dns_prop_name, dns1 ? ipaddr_to_string(dns1) : "");
    property_set("net.dns1", dns1 ? ipaddr_to_string(dns1) : "");

    snprintf(dns_prop_name, sizeof(dns_prop_name), "net.%s.dns2", ifname);
    property_set(dns_prop_name, dns2 ? ipaddr_to_string(dns2) : "");
    property_set("net.dns2", dns2 ? ipaddr_to_string(dns2) : "");



    return 0;
}
