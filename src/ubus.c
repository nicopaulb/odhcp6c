/****************************************************************************
**
** SPDX-License-Identifier: BSD-2-Clause-Patent
**
** SPDX-FileCopyrightText: Copyright (c) 2024 SoftAtHome
**
** Redistribution and use in source and binary forms, with or
** without modification, are permitted provided that the following
** conditions are met:
**
** 1. Redistributions of source code must retain the above copyright
** notice, this list of conditions and the following disclaimer.
**
** 2. Redistributions in binary form must reproduce the above
** copyright notice, this list of conditions and the following
** disclaimer in the documentation and/or other materials provided
** with the distribution.
**
** Subject to the terms and conditions of this license, each
** copyright holder and contributor hereby grants to those receiving
** rights under this license a perpetual, worldwide, non-exclusive,
** no-charge, royalty-free, irrevocable (except for failure to
** satisfy the conditions of this license) patent license to make,
** have made, use, offer to sell, sell, import, and otherwise
** transfer this software, where such license applies only to those
** patent claims, already acquired or hereafter acquired, licensable
** by such copyright holder or contributor that are necessarily
** infringed by:
**
** (a) their Contribution(s) (the licensed copyrights of copyright
** holders and non-copyrightable additions of contributors, in
** source or binary form) alone; or
**
** (b) combination of their Contribution(s) with the work of
** authorship to which such Contribution(s) was added by such
** copyright holder or contributor, if, at the time the Contribution
** is added, such addition causes such combination to be necessarily
** infringed. The patent license shall not apply to any other
** combinations which include the Contribution.
**
** Except as expressly stated above, no rights or licenses from any
** copyright holder or contributor is granted under this license,
** whether expressly, by implication, estoppel or otherwise.
**
** DISCLAIMER
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
** CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
** INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
** MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
** DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
** CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
** USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
** AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
** ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
**
****************************************************************************/
#include "odhcp6c.h"


#include <sys/types.h>
#include <arpa/inet.h>
#include <resolv.h>
#include <stdio.h>
#include <syslog.h>
#include <inttypes.h>
#include <libubus.h>
#include <libubox/blobmsg.h>

#include "ubus.h"

#define CHECK(stmt) \
	do { \
		int ret = (stmt); \
		if (ret != UBUS_STATUS_OK) \
		{ \
			syslog(LOG_ERR, "%s failed: %s (%d)", #stmt, ubus_strerror(ret), ret); \
			return ret; \
		} \
	} while (0)

#define CHECK_ALLOC(buf) \
	do { \
		if (buf == NULL) \
		{ \
			return UBUS_STATUS_NO_MEMORY; \
		} \
	} while (0)

enum entry_type {
	ENTRY_ADDRESS,
	ENTRY_HOST,
	ENTRY_ROUTE,
	ENTRY_PREFIX
};

struct ubus_context *ubus = NULL;
static struct blob_buf b;
static char ubus_name[24];

static int ubus_handle_get_state(struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method, struct blob_attr *msg);

static struct ubus_object_type odhcp6c_object_type = {
	.name = "odhcp6c"
};

static struct ubus_object odhcp6c_object = {
	.name = NULL,
	.type = &odhcp6c_object_type,
	.methods = odhcp6c_object_methods,
	.n_methods = ARRAY_SIZE(odhcp6c_object_methods),
};

static void ubus_disconnect_cb(struct ubus_context *ubus)
{
	int ret;

	ret = ubus_reconnect(ubus, NULL);
	if (ret) {
		syslog(LOG_ERR, "Cannot reconnect to ubus: %s", ubus_strerror(ret));
		ubus_destroy(ubus);
	}
}

char *ubus_init(const char* interface) 
{
	int ret = 0;

 	if (!(ubus = ubus_connect(NULL)))
		return NULL;

	snprintf(ubus_name, 24, "odhcp6c.%s", interface);
	odhcp6c_object.name = ubus_name;
	
	ret = ubus_add_object(ubus, &odhcp6c_object);
	if (ret) {
		ubus_destroy(ubus);
		return (char *)ubus_strerror(ret);
	}

	ubus->connection_lost = ubus_disconnect_cb;
	return NULL;
}

struct ubus_context *ubus_get_ctx(void)
{
	return ubus;
}

void ubus_destroy(struct ubus_context *ubus)
{
	syslog(LOG_NOTICE, "Disconnecting from ubus");
	
	if (ubus != NULL)
		ubus_free(ubus);
	ubus = NULL;

	/* Forces re-initialization when we're reusing the same definitions later on. */
	odhcp6c_object.id = 0;
	odhcp6c_object.id = 0;
}

static int ipv6_to_blob(const char *name,
		const struct in6_addr *addr, size_t cnt)
{
	void *arr = blobmsg_open_array(&b, name);
	
	for (size_t i = 0; i < cnt; ++i) {
		char *buf = blobmsg_alloc_string_buffer(&b, NULL, INET6_ADDRSTRLEN);
		CHECK_ALLOC(buf);
		inet_ntop(AF_INET6, &addr[i], buf, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);
	}

	blobmsg_close_array(&b, arr);
	return UBUS_STATUS_OK;
}

static int fqdn_to_blob(const char *name, const uint8_t *fqdn, size_t len)
{
	size_t buf_size = len > 255 ? 256 : (len + 1);
	const uint8_t *fqdn_end = fqdn + len;
	char *buf = NULL;

	void *arr = blobmsg_open_array(&b, name);

	while (fqdn < fqdn_end) {
		buf = blobmsg_alloc_string_buffer(&b, name, buf_size);
		CHECK_ALLOC(buf);
		int l = dn_expand(fqdn, fqdn_end, fqdn, buf, buf_size);
		if (l < 1) {
			buf[0] = '\0';
			blobmsg_add_string_buffer(&b);
			break;
		}
		buf[l] = '\0';
		blobmsg_add_string_buffer(&b);
		fqdn += l;
	}

	blobmsg_close_array(&b, arr);
	return UBUS_STATUS_OK;
}

static int bin_to_blob(uint8_t *opts, size_t len)
{
	uint8_t *oend = opts + len, *odata;
	uint16_t otype, olen;

	dhcpv6_for_each_option(opts, oend, otype, olen, odata) {
		char name[14];
		char *buf;

		snprintf(name, 14, "OPTION_%hu", otype);
		buf = blobmsg_alloc_string_buffer(&b, name, olen * 2);
		CHECK_ALLOC(buf);
		script_hexlify(buf, odata, olen);
		blobmsg_add_string_buffer(&b);
	}
	return UBUS_STATUS_OK;
}

static int entry_to_blob(const char *name, const void *data, size_t len, enum entry_type type)
{
	const struct odhcp6c_entry *e = data;

	void *arr = blobmsg_open_array(&b, name);

	for (size_t i = 0; i < len / sizeof(*e); ++i) {
		void *entry = blobmsg_open_table(&b, name);

		/*
		 * The only invalid entries allowed to be passed to the script are prefix entries.
		 * This will allow immediate removal of the old ipv6-prefix-assignment that might
		 * otherwise be kept for up to 2 hours (see L-13 requirement of RFC 7084).
		 */
		if (!e[i].valid && type != ENTRY_PREFIX)
			continue;

		char *buf = blobmsg_alloc_string_buffer(&b, "target", INET6_ADDRSTRLEN);
		CHECK_ALLOC(buf);
		inet_ntop(AF_INET6, &e[i].target, buf, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		if (type != ENTRY_HOST) {
			blobmsg_add_u8(&b, "length", e[i].length);
			if (type == ENTRY_ROUTE) {
				if (!IN6_IS_ADDR_UNSPECIFIED(&e[i].router)) {
					buf = blobmsg_alloc_string_buffer(&b, "router", INET6_ADDRSTRLEN);
					CHECK_ALLOC(buf);
					inet_ntop(AF_INET6, &e[i].router, buf, INET6_ADDRSTRLEN);
					blobmsg_add_string_buffer(&b);
				}

				blobmsg_add_u32(&b, "valid", e[i].valid);
				blobmsg_add_u16(&b, "priority", e[i].priority);
			} else {
				blobmsg_add_u32(&b, "valid", e[i].valid);
				blobmsg_add_u32(&b, "preferred", e[i].preferred);
				blobmsg_add_u32(&b, "t1", e[i].t1);
				blobmsg_add_u32(&b, "t2", e[i].t2);
			}

			if (type == ENTRY_PREFIX && ntohl(e[i].iaid) != 1) {
				blobmsg_add_u32(&b, "iaid", ntohl(e[i].iaid));
			}

			if (type == ENTRY_PREFIX && e[i].priority) {
				// priority and router are abused for prefix exclusion
				buf = blobmsg_alloc_string_buffer(&b, "excluded", INET6_ADDRSTRLEN);
				CHECK_ALLOC(buf);
				inet_ntop(AF_INET6, &e[i].router, buf, INET6_ADDRSTRLEN);
				blobmsg_add_string_buffer(&b);
				blobmsg_add_u16(&b, "excluded_length", e[i].priority);
			}
		}

		blobmsg_close_table(&b, entry);
	}

	blobmsg_close_array(&b, arr);
	return UBUS_STATUS_OK;
}

static int search_to_blob(const char *name, const uint8_t *start, size_t len)
{
	void *arr = blobmsg_open_array(&b, name);
	char *buf = NULL;

	for (struct odhcp6c_entry *e = (struct odhcp6c_entry*)start;
				(uint8_t*)e < &start[len] &&
				(uint8_t*)odhcp6c_next_entry(e) <= &start[len];
				e = odhcp6c_next_entry(e)) {
		if (!e->valid)
			continue;
		
		buf = blobmsg_alloc_string_buffer(&b, NULL, e->auxlen+1);
		CHECK_ALLOC(buf);
		buf = mempcpy(buf, e->auxtarget, e->auxlen);
		*buf = '\0';
		blobmsg_add_string_buffer(&b);
	}

	blobmsg_close_array(&b, arr);
	return UBUS_STATUS_OK;
}

static int s46_to_blob_portparams(const uint8_t *data, size_t len)
{
	uint8_t *odata;
	uint16_t otype, olen;

	dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
		if (otype == DHCPV6_OPT_S46_PORTPARAMS &&
				olen == sizeof(struct dhcpv6_s46_portparams)) {
			struct dhcpv6_s46_portparams *params = (void*)odata;
			blobmsg_add_u8(&b, "offset", params->offset);
			blobmsg_add_u8(&b, "psidlen", params->psid_len);
			blobmsg_add_u16(&b, "psid", ntohs(params->psid));
		}
	}
	return UBUS_STATUS_OK;
}

static int s46_to_blob(enum odhcp6c_state state, const uint8_t *data, size_t len)
{
	const char *name = (state == STATE_S46_MAPE) ? "MAPE" :
			(state == STATE_S46_MAPT) ? "MAPT" : "LW4O6";

	if (len == 0)
		return UBUS_STATUS_OK;

	char *buf = NULL;
	void *arr = blobmsg_open_array(&b, name);

	const char *type = (state == STATE_S46_MAPE) ? "map-e" :
			(state == STATE_S46_MAPT) ? "map-t" : "lw4o6";

	uint8_t *odata;
	uint16_t otype, olen;

	dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
		struct dhcpv6_s46_rule *rule = (struct dhcpv6_s46_rule*)odata;
		struct dhcpv6_s46_v4v6bind *bind = (struct dhcpv6_s46_v4v6bind*)odata;

		void *option = blobmsg_open_table(&b, NULL);

		if (state != STATE_S46_LW && otype == DHCPV6_OPT_S46_RULE &&
				olen >= sizeof(struct dhcpv6_s46_rule)) {
			struct in6_addr in6 = IN6ADDR_ANY_INIT;

			size_t prefix6len = rule->prefix6_len;
			prefix6len = (prefix6len % 8 == 0) ? prefix6len / 8 : prefix6len / 8 + 1;

			if (prefix6len > sizeof(in6) ||
				olen < sizeof(struct dhcpv6_s46_rule) + prefix6len)
				continue;

			memcpy(&in6, rule->ipv6_prefix, prefix6len);

			buf = blobmsg_alloc_string_buffer(&b, "ipv4prefix", INET_ADDRSTRLEN);
			CHECK_ALLOC(buf);
			inet_ntop(AF_INET, &rule->ipv4_prefix, buf, INET_ADDRSTRLEN);
			blobmsg_add_string_buffer(&b);

			buf = blobmsg_alloc_string_buffer(&b, "ipv6prefix", INET6_ADDRSTRLEN);
			CHECK_ALLOC(buf);
			inet_ntop(AF_INET6, &in6, buf, INET6_ADDRSTRLEN);
			blobmsg_add_string_buffer(&b);

			blobmsg_add_u8(&b, "fmr", rule->flags);
			blobmsg_add_string(&b, "type", type);
			blobmsg_add_u8(&b, "ealen", rule->ea_len);
			blobmsg_add_u8(&b, "prefix4len", rule->prefix4_len);
			blobmsg_add_u8(&b, "prefix6len", rule->prefix6_len);

			s46_to_blob_portparams(&rule->ipv6_prefix[prefix6len],
					olen - sizeof(*rule) - prefix6len);

			dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
				if (state != STATE_S46_MAPT && otype == DHCPV6_OPT_S46_BR &&
						olen == sizeof(struct in6_addr)) {
					buf = blobmsg_alloc_string_buffer(&b, "br", INET6_ADDRSTRLEN);
					CHECK_ALLOC(buf);
					inet_ntop(AF_INET6, odata, buf, INET6_ADDRSTRLEN);
					blobmsg_add_string_buffer(&b);
				} else if (state == STATE_S46_MAPT && otype == DHCPV6_OPT_S46_DMR &&
						olen >= sizeof(struct dhcpv6_s46_dmr)) {
					struct dhcpv6_s46_dmr *dmr = (struct dhcpv6_s46_dmr*)odata;
					memset(&in6, 0, sizeof(in6));
					size_t prefix6len = dmr->dmr_prefix6_len;
					prefix6len = (prefix6len % 8 == 0) ? prefix6len / 8 : prefix6len / 8 + 1;

					if (prefix6len > sizeof(in6) ||
						olen < sizeof(struct dhcpv6_s46_dmr) + prefix6len)
						continue;

					buf = blobmsg_alloc_string_buffer(&b, "dmr", INET6_ADDRSTRLEN);
					CHECK_ALLOC(buf);
					inet_ntop(AF_INET6, &in6, buf, INET6_ADDRSTRLEN);
					blobmsg_add_string_buffer(&b);
					blobmsg_add_u8(&b, "dmrprefix6len", dmr->dmr_prefix6_len);
				}
			}
		} else if (state == STATE_S46_LW && otype == DHCPV6_OPT_S46_V4V6BIND &&
				olen >= sizeof(struct dhcpv6_s46_v4v6bind)) {
			struct in6_addr in6 = IN6ADDR_ANY_INIT;

			size_t prefix6len = bind->bindprefix6_len;
			prefix6len = (prefix6len % 8 == 0) ? prefix6len / 8 : prefix6len / 8 + 1;

			if (prefix6len > sizeof(in6) ||
				olen < sizeof(struct dhcpv6_s46_v4v6bind) + prefix6len)
				continue;

			memcpy(&in6, bind->bind_ipv6_prefix, prefix6len);

			buf = blobmsg_alloc_string_buffer(&b, "ipv4prefix", INET_ADDRSTRLEN);
			CHECK_ALLOC(buf);
			inet_ntop(AF_INET, &bind->ipv4_address, buf, INET_ADDRSTRLEN);
			blobmsg_add_string_buffer(&b);

			buf = blobmsg_alloc_string_buffer(&b, "ipv6prefix", INET6_ADDRSTRLEN);
			CHECK_ALLOC(buf);
			inet_ntop(AF_INET6, &in6, buf, INET6_ADDRSTRLEN);
			blobmsg_add_string_buffer(&b);

			blobmsg_add_string(&b, "type", type);
			blobmsg_add_u8(&b, "prefix4len", 32);
			blobmsg_add_u8(&b, "prefix6len", bind->bindprefix6_len);

			s46_to_blob_portparams(&bind->bind_ipv6_prefix[prefix6len],
					olen - sizeof(*bind) - prefix6len);

			dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
				if (otype == DHCPV6_OPT_S46_BR && olen == sizeof(struct in6_addr)) {
					buf = blobmsg_alloc_string_buffer(&b, "br", INET6_ADDRSTRLEN);
					CHECK_ALLOC(buf);
					inet_ntop(AF_INET6, odata, buf, INET6_ADDRSTRLEN);
					blobmsg_add_string_buffer(&b);
				}
			}
		}
		blobmsg_close_table(&b, option);
	}

	blobmsg_close_array(&b, arr);
	return UBUS_STATUS_OK;
}

int ubus_dhcp_event(const char* status)
{
	if (!ubus || !odhcp6c_object.has_subscribers)
    	return UBUS_STATUS_UNKNOWN_ERROR;

	char *buf = NULL;

	size_t dns_len, search_len, custom_len, sntp_ip_len, ntp_ip_len, ntp_dns_len;
	size_t sip_ip_len, sip_fqdn_len, aftr_name_len, cer_len, addr_len;
	size_t s46_mapt_len, s46_mape_len, s46_lw_len, passthru_len;
	struct in6_addr *addr = odhcp6c_get_state(STATE_SERVER_ADDR, &addr_len);
	struct in6_addr *dns = odhcp6c_get_state(STATE_DNS, &dns_len);
	uint8_t *search = odhcp6c_get_state(STATE_SEARCH, &search_len);
	uint8_t *custom = odhcp6c_get_state(STATE_CUSTOM_OPTS, &custom_len);
	struct in6_addr *sntp = odhcp6c_get_state(STATE_SNTP_IP, &sntp_ip_len);
	struct in6_addr *ntp = odhcp6c_get_state(STATE_NTP_IP, &ntp_ip_len);
	uint8_t *ntp_dns = odhcp6c_get_state(STATE_NTP_FQDN, &ntp_dns_len);
	struct in6_addr *sip = odhcp6c_get_state(STATE_SIP_IP, &sip_ip_len);
	uint8_t *sip_fqdn = odhcp6c_get_state(STATE_SIP_FQDN, &sip_fqdn_len);
	uint8_t *aftr_name = odhcp6c_get_state(STATE_AFTR_NAME, &aftr_name_len);
	struct in6_addr *cer = odhcp6c_get_state(STATE_CER, &cer_len);
	uint8_t *s46_mapt = odhcp6c_get_state(STATE_S46_MAPT, &s46_mapt_len);
	uint8_t *s46_mape = odhcp6c_get_state(STATE_S46_MAPE, &s46_mape_len);
	uint8_t *s46_lw = odhcp6c_get_state(STATE_S46_LW, &s46_lw_len);
	uint8_t *passthru = odhcp6c_get_state(STATE_PASSTHRU, &passthru_len);

	size_t prefix_len, address_len, ra_pref_len,
		ra_route_len, ra_dns_len, ra_search_len;
	uint8_t *prefix = odhcp6c_get_state(STATE_IA_PD, &prefix_len);
	uint8_t *address = odhcp6c_get_state(STATE_IA_NA, &address_len);
	uint8_t *ra_pref = odhcp6c_get_state(STATE_RA_PREFIX, &ra_pref_len);
	uint8_t *ra_route = odhcp6c_get_state(STATE_RA_ROUTE, &ra_route_len);
	uint8_t *ra_dns = odhcp6c_get_state(STATE_RA_DNS, &ra_dns_len);
	uint8_t *ra_search = odhcp6c_get_state(STATE_RA_SEARCH, &ra_search_len);

 	blob_buf_init(&b, BLOBMSG_TYPE_TABLE);

	CHECK(ipv6_to_blob("SERVER", addr, addr_len / sizeof(*addr)));
	CHECK(ipv6_to_blob("RDNSS", dns, dns_len / sizeof(*dns)));
	CHECK(ipv6_to_blob("SNTP_IP", sntp, sntp_ip_len / sizeof(*sntp)));
	CHECK(ipv6_to_blob("NTP_IP", ntp, ntp_ip_len / sizeof(*ntp)));
	CHECK(fqdn_to_blob("NTP_FQDN", ntp_dns, ntp_dns_len));
	CHECK(ipv6_to_blob("SIP_IP", sip, sip_ip_len / sizeof(*sip)));
	CHECK(fqdn_to_blob("DOMAINS", search, search_len));
	CHECK(fqdn_to_blob("SIP_DOMAIN", sip_fqdn, sip_fqdn_len));
	CHECK(fqdn_to_blob("AFTR", aftr_name, aftr_name_len));
	CHECK(ipv6_to_blob("CER", cer, cer_len / sizeof(*cer)));
	CHECK(s46_to_blob(STATE_S46_MAPE, s46_mape, s46_mape_len));
	CHECK(s46_to_blob(STATE_S46_MAPT, s46_mapt, s46_mapt_len));
	CHECK(s46_to_blob(STATE_S46_LW, s46_lw, s46_lw_len));
	CHECK(bin_to_blob(custom, custom_len));

	if (odhcp6c_is_bound()) {
		CHECK(entry_to_blob("PREFIXES", prefix, prefix_len, ENTRY_PREFIX));
		CHECK(entry_to_blob("ADDRESSES", address, address_len, ENTRY_ADDRESS));
	}

	CHECK(entry_to_blob("RA_ADDRESSES", ra_pref, ra_pref_len, ENTRY_ADDRESS));
	CHECK(entry_to_blob("RA_ROUTES", ra_route, ra_route_len, ENTRY_ROUTE));
	CHECK(entry_to_blob("RA_DNS", ra_dns, ra_dns_len, ENTRY_HOST));
	CHECK(search_to_blob("RA_DOMAINS", ra_search, ra_search_len));

	blobmsg_add_u32(&b, "RA_HOPLIMIT", ra_get_hoplimit());
	blobmsg_add_u32(&b, "RA_MTU", ra_get_mtu());
	blobmsg_add_u32(&b, "RA_REACHABLE", ra_get_reachable());
	blobmsg_add_u32(&b, "RA_RETRANSMIT", ra_get_retransmit());
	
	buf = blobmsg_alloc_string_buffer(&b, "PASSTHRU", passthru_len * 2);
	CHECK_ALLOC(buf);
	script_hexlify(buf, passthru, passthru_len);
	blobmsg_add_string_buffer(&b);
	CHECK(ubus_notify(ubus, &odhcp6c_object, status, b.head, -1));
	blob_buf_free(&b);

	return UBUS_STATUS_OK;
}
