/* test routines for gbproxy
 * send NS messages to the gbproxy and dumps what happens
 * (C) 2013 by sysmocom s.f.m.c. GmbH
 * Author: Jacob Erlbeck <jerlbeck@sysmocom.de>
 */

#undef _GNU_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/application.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/signal.h>
#include <osmocom/core/rate_ctr.h>
#include <osmocom/gprs/gprs_msgb.h>
#include <osmocom/gprs/gprs_ns.h>
#include <osmocom/gprs/gprs_bssgp.h>

#include <openbsc/gb_proxy.h>

#define REMOTE_BSS_ADDR 0x01020304
#define REMOTE_SGSN_ADDR 0x05060708

#define SGSN_NSEI 0x0100

struct gbproxy_config gbcfg;

static int gprs_process_message(struct gprs_ns_inst *nsi, const char *text,
				struct sockaddr_in *peer, const unsigned char* data,
				size_t data_len);

static void send_ns_reset(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr,
			  enum ns_cause cause, uint16_t nsvci, uint16_t nsei)
{
	/* GPRS Network Service, PDU type: NS_RESET,
	 */
	unsigned char msg[12] = {
		0x02, 0x00, 0x81, 0x01, 0x01, 0x82, 0x11, 0x22,
		0x04, 0x82, 0x11, 0x22
	};

	msg[3] = cause;
	msg[6] = nsvci / 256;
	msg[7] = nsvci % 256;
	msg[10] = nsei / 256;
	msg[11] = nsei % 256;

	gprs_process_message(nsi, "RESET", src_addr, msg, sizeof(msg));
}

static void send_ns_reset_ack(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr,
			      uint16_t nsvci, uint16_t nsei)
{
	/* GPRS Network Service, PDU type: NS_RESET_ACK,
	 */
	unsigned char msg[9] = {
		0x03, 0x01, 0x82, 0x11, 0x22,
		0x04, 0x82, 0x11, 0x22
	};

	msg[3] = nsvci / 256;
	msg[4] = nsvci % 256;
	msg[7] = nsei / 256;
	msg[8] = nsei % 256;

	gprs_process_message(nsi, "RESET_ACK", src_addr, msg, sizeof(msg));
}

static void send_ns_alive(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr)
{
	/* GPRS Network Service, PDU type: NS_ALIVE */
	unsigned char msg[1] = {
		0x0a
	};

	gprs_process_message(nsi, "ALIVE", src_addr, msg, sizeof(msg));
}

static void send_ns_alive_ack(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr)
{
	/* GPRS Network Service, PDU type: NS_ALIVE_ACK */
	unsigned char msg[1] = {
		0x0b
	};

	gprs_process_message(nsi, "ALIVE_ACK", src_addr, msg, sizeof(msg));
}

static void send_ns_unblock(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr)
{
	/* GPRS Network Service, PDU type: NS_UNBLOCK */
	unsigned char msg[1] = {
		0x06
	};

	gprs_process_message(nsi, "UNBLOCK", src_addr, msg, sizeof(msg));
}

static void send_ns_unblock_ack(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr)
{
	/* GPRS Network Service, PDU type: NS_UNBLOCK_ACK */
	unsigned char msg[1] = {
		0x07
	};

	gprs_process_message(nsi, "UNBLOCK_ACK", src_addr, msg, sizeof(msg));
}

static void send_ns_unitdata(struct gprs_ns_inst *nsi, const char *text,
			     struct sockaddr_in *src_addr, uint16_t nsbvci,
			     const unsigned char *bssgp_msg, size_t bssgp_msg_size)
{
	/* GPRS Network Service, PDU type: NS_UNITDATA */
	unsigned char msg[4096] = {
		0x00, 0x00, 0x00, 0x00
	};

	OSMO_ASSERT(bssgp_msg_size <= sizeof(msg) - 4);

	msg[2] = nsbvci / 256;
	msg[3] = nsbvci % 256;
	memcpy(msg + 4, bssgp_msg, bssgp_msg_size);

	gprs_process_message(nsi, text ? text : "UNITDATA", src_addr, msg, bssgp_msg_size + 4);
}

static void send_bssgp_reset(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr,
			     uint16_t bvci)
{
	/* GPRS Network Service, PDU type: NS_UNITDATA, BVCI 0
	 * BSSGP RESET */
	unsigned char msg[22] = {
		0x22, 0x04, 0x82, 0x4a,
		0x2e, 0x07, 0x81, 0x08, 0x08, 0x88, 0x10, 0x20,
		0x30, 0x40, 0x50, 0x60, 0x10, 0x00
	};

	msg[3] = bvci / 256;
	msg[4] = bvci % 256;

	send_ns_unitdata(nsi, "BVC_RESET", src_addr, 0, msg, sizeof(msg));
}

static void send_bssgp_reset_ack(struct gprs_ns_inst *nsi,
				 struct sockaddr_in *src_addr, uint16_t bvci)
{
	/* GPRS Network Service, PDU type: NS_UNITDATA, BVCI 0
	 * BSSGP RESET_ACK */
	static unsigned char msg[5] = {
		0x23, 0x04, 0x82, 0x00,
		0x00
	};

	msg[3] = bvci / 256;
	msg[4] = bvci % 256;

	send_ns_unitdata(nsi, "BVC_RESET_ACK", src_addr, 0, msg, sizeof(msg));
}

static void setup_ns(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr,
		     uint16_t nsvci, uint16_t nsei)
{
	printf("Setup NS-VC: remote 0x%08x:%d, "
	       "NSVCI 0x%04x(%d), NSEI 0x%04x(%d)\n\n",
	       ntohl(src_addr->sin_addr.s_addr), ntohs(src_addr->sin_port),
	       nsvci, nsvci, nsei, nsei);

	send_ns_reset(nsi, src_addr, NS_CAUSE_OM_INTERVENTION, nsvci, nsei);
	send_ns_alive(nsi, src_addr);
	send_ns_unblock(nsi, src_addr);
	send_ns_alive_ack(nsi, src_addr);
}

static void setup_bssgp(struct gprs_ns_inst *nsi, struct sockaddr_in *src_addr,
		     uint16_t bvci)
{
	printf("Setup BSSGP: remote 0x%08x:%d, "
	       "BVCI 0x%04x(%d)\n\n",
	       ntohl(src_addr->sin_addr.s_addr), ntohs(src_addr->sin_port),
	       bvci, bvci);

	send_bssgp_reset(nsi, src_addr, bvci);
}

int gprs_ns_rcvmsg(struct gprs_ns_inst *nsi, struct msgb *msg,
		   struct sockaddr_in *saddr, enum gprs_ns_ll ll);

/* override */
int gprs_ns_callback(enum gprs_ns_evt event, struct gprs_nsvc *nsvc,
			 struct msgb *msg, uint16_t bvci)
{
	printf("CALLBACK, event %d, msg length %d, bvci 0x%04x\n%s\n\n",
			event, msgb_bssgp_len(msg), bvci,
			osmo_hexdump(msgb_bssgph(msg), msgb_bssgp_len(msg)));

	switch (event) {
	case GPRS_NS_EVT_UNIT_DATA:
		return gbprox_rcvmsg(msg, nsvc->nsei, bvci, nsvc->nsvci);
	default:
		break;
	}
	return 0;
}

/* override */
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
		const struct sockaddr *dest_addr, socklen_t addrlen)
{
	typedef ssize_t (*sendto_t)(int, const void *, size_t, int,
			const struct sockaddr *, socklen_t);
	static sendto_t real_sendto = NULL;
	uint32_t dest_host = htonl(((struct sockaddr_in *)dest_addr)->sin_addr.s_addr);
	int      dest_port = htons(((struct sockaddr_in *)dest_addr)->sin_port);

	if (!real_sendto)
		real_sendto = dlsym(RTLD_NEXT, "sendto");

	if (dest_host == REMOTE_BSS_ADDR)
		printf("MESSAGE to BSS at 0x%08x:%d, msg length %d\n%s\n\n",
		       dest_host, dest_port,
		       len, osmo_hexdump(buf, len));
	else if (dest_host == REMOTE_SGSN_ADDR)
		printf("MESSAGE to SGSN at 0x%08x:%d, msg length %d\n%s\n\n",
		       dest_host, dest_port,
		       len, osmo_hexdump(buf, len));
	else
		return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);

	return len;
}

/* override */
int gprs_ns_sendmsg(struct gprs_ns_inst *nsi, struct msgb *msg)
{
	typedef int (*gprs_ns_sendmsg_t)(struct gprs_ns_inst *nsi, struct msgb *msg);
	static gprs_ns_sendmsg_t real_gprs_ns_sendmsg = NULL;
	uint16_t bvci = msgb_bvci(msg);
	uint16_t nsei = msgb_nsei(msg);

	unsigned char *buf = msg->data;
	size_t len = msg->len;

	if (!real_gprs_ns_sendmsg)
		real_gprs_ns_sendmsg = dlsym(RTLD_NEXT, "gprs_ns_sendmsg");

	if (nsei == SGSN_NSEI)
		printf("NS UNITDATA MESSAGE to SGSN, BVCI 0x%04x, msg length %d\n%s\n\n",
		       bvci, len, osmo_hexdump(buf, len));
	else
		printf("NS UNITDATA MESSAGE to BSS, BVCI 0x%04x, msg length %d\n%s\n\n",
		       bvci, len, osmo_hexdump(buf, len));

	return real_gprs_ns_sendmsg(nsi, msg);
}

static void dump_rate_ctr_group(FILE *stream, const char *prefix,
			    struct rate_ctr_group *ctrg)
{
	unsigned int i;

	for (i = 0; i < ctrg->desc->num_ctr; i++) {
		struct rate_ctr *ctr = &ctrg->ctr[i];
		if (ctr->current && !strchr(ctrg->desc->ctr_desc[i].name, '.'))
			fprintf(stream, " %s%s: %llu%s",
				prefix, ctrg->desc->ctr_desc[i].description,
				(long long)ctr->current,
				"\n");
	};
}

/* Signal handler for signals from NS layer */
static int test_signal(unsigned int subsys, unsigned int signal,
		  void *handler_data, void *signal_data)
{
	struct ns_signal_data *nssd = signal_data;
	int rc;

	if (subsys != SS_L_NS)
		return 0;

	switch (signal) {
	case S_NS_RESET:
		printf("==> got signal NS_RESET, NS-VC 0x%04x/%s\n",
		       nssd->nsvc->nsvci,
		       gprs_ns_ll_str(nssd->nsvc));
		break;

	case S_NS_ALIVE_EXP:
		printf("==> got signal NS_ALIVE_EXP, NS-VC 0x%04x/%s\n",
		       nssd->nsvc->nsvci,
		       gprs_ns_ll_str(nssd->nsvc));
		break;

	case S_NS_BLOCK:
		printf("==> got signal NS_BLOCK, NS-VC 0x%04x/%s\n",
		       nssd->nsvc->nsvci,
		       gprs_ns_ll_str(nssd->nsvc));
		break;

	case S_NS_UNBLOCK:
		printf("==> got signal NS_UNBLOCK, NS-VC 0x%04x/%s\n",
		       nssd->nsvc->nsvci,
		       gprs_ns_ll_str(nssd->nsvc));
		break;

	case S_NS_REPLACED:
		printf("==> got signal NS_REPLACED: 0x%04x/%s",
		       nssd->nsvc->nsvci,
		       gprs_ns_ll_str(nssd->nsvc));
		printf(" -> 0x%04x/%s\n",
		       nssd->old_nsvc->nsvci,
		       gprs_ns_ll_str(nssd->old_nsvc));
		break;

	default:
		printf("==> got signal %d, NS-VC 0x%04x/%s\n", signal,
		       nssd->nsvc->nsvci,
		       gprs_ns_ll_str(nssd->nsvc));
		break;
	}
	printf("\n");
	rc = gbprox_signal(subsys, signal, handler_data, signal_data);
	return rc;
}

static int gprs_process_message(struct gprs_ns_inst *nsi, const char *text, struct sockaddr_in *peer, const unsigned char* data, size_t data_len)
{
	struct msgb *msg;
	int ret;
	if (data_len > NS_ALLOC_SIZE - NS_ALLOC_HEADROOM) {
		fprintf(stderr, "message too long: %d\n", data_len);
		return -1;
	}

	msg = gprs_ns_msgb_alloc();
	memmove(msg->data, data, data_len);
	msg->l2h = msg->data;
	msgb_put(msg, data_len);

	printf("PROCESSING %s from 0x%08x:%d\n%s\n\n",
	       text, ntohl(peer->sin_addr.s_addr), ntohs(peer->sin_port),
	       osmo_hexdump(data, data_len));

	ret = gprs_ns_rcvmsg(nsi, msg, peer, GPRS_NS_LL_UDP);

	printf("result (%s) = %d\n\n", text, ret);

	msgb_free(msg);

	return ret;
}

static void gprs_dump_nsi(struct gprs_ns_inst *nsi)
{
	struct gprs_nsvc *nsvc;

	printf("Current NS-VCIs:\n");
	llist_for_each_entry(nsvc, &nsi->gprs_nsvcs, list) {
		struct sockaddr_in *peer = &(nsvc->ip.bts_addr);
		printf("    VCI 0x%04x, NSEI 0x%04x, peer 0x%08x:%d%s%s\n",
		       nsvc->nsvci, nsvc->nsei,
		       ntohl(peer->sin_addr.s_addr), ntohs(peer->sin_port),
		       nsvc->state & NSE_S_BLOCKED ? ", blocked" : "",
		       nsvc->state & NSE_S_ALIVE   ? "" : ", dead"
		      );
		dump_rate_ctr_group(stdout, "        ", nsvc->ctrg);
	}
	printf("\n");
}

static void test_gbproxy()
{
	struct gprs_ns_inst *nsi = gprs_ns_instantiate(gprs_ns_callback, NULL);
	struct sockaddr_in bss_peer[4] = {{0},};
	struct sockaddr_in sgsn_peer= {0};

	bssgp_nsi = nsi;
	gbcfg.nsi = bssgp_nsi;
	gbcfg.nsip_sgsn_nsei = SGSN_NSEI;

	sgsn_peer.sin_family = AF_INET;
	sgsn_peer.sin_port = htons(32000);
	sgsn_peer.sin_addr.s_addr = htonl(REMOTE_SGSN_ADDR);

	bss_peer[0].sin_family = AF_INET;
	bss_peer[0].sin_port = htons(1111);
	bss_peer[0].sin_addr.s_addr = htonl(REMOTE_BSS_ADDR);
	bss_peer[1].sin_family = AF_INET;
	bss_peer[1].sin_port = htons(2222);
	bss_peer[1].sin_addr.s_addr = htonl(REMOTE_BSS_ADDR);
	bss_peer[2].sin_family = AF_INET;
	bss_peer[2].sin_port = htons(3333);
	bss_peer[2].sin_addr.s_addr = htonl(REMOTE_BSS_ADDR);
	bss_peer[3].sin_family = AF_INET;
	bss_peer[3].sin_port = htons(4444);
	bss_peer[3].sin_addr.s_addr = htonl(REMOTE_BSS_ADDR);

	printf("--- Initialise SGSN ---\n\n");

	gprs_ns_nsip_connect(nsi, &sgsn_peer, SGSN_NSEI, SGSN_NSEI+1);
	send_ns_reset_ack(nsi, &sgsn_peer, SGSN_NSEI+1, SGSN_NSEI);
	send_ns_alive_ack(nsi, &sgsn_peer);
	send_ns_unblock_ack(nsi, &sgsn_peer);
	send_ns_alive(nsi, &sgsn_peer);
	gprs_dump_nsi(nsi);

	printf("--- Initialise BSS 1 ---\n\n");

	setup_ns(nsi, &bss_peer[0], 0x1001, 0x1000);
	setup_bssgp(nsi, &bss_peer[0], 0x1002);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	send_bssgp_reset_ack(nsi, &sgsn_peer, 0x1002);

	printf("--- Initialise BSS 2 ---\n\n");

	setup_ns(nsi, &bss_peer[1], 0x2001, 0x2000);
	setup_bssgp(nsi, &bss_peer[1], 0x2002);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	send_bssgp_reset_ack(nsi, &sgsn_peer, 0x2002);

	printf("--- Move BSS 1 to new port ---\n\n");

	setup_ns(nsi, &bss_peer[2], 0x1001, 0x1000);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Move BSS 2 to former BSS 1 port ---\n\n");

	setup_ns(nsi, &bss_peer[0], 0x2001, 0x2000);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Move BSS 1 to current BSS 2 port ---\n\n");

	setup_ns(nsi, &bss_peer[0], 0x2001, 0x2000);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Move BSS 2 to new port ---\n\n");

	setup_ns(nsi, &bss_peer[3], 0x2001, 0x2000);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Move BSS 2 to former BSS 1 port ---\n\n");

	setup_ns(nsi, &bss_peer[2], 0x2001, 0x2000);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Move BSS 1 to original BSS 1 port ---\n\n");

	setup_ns(nsi, &bss_peer[0], 0x1001, 0x1000);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Reset BSS 1 with a new BVCI ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], 0x1012);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	send_bssgp_reset_ack(nsi, &sgsn_peer, 0x1012);

	printf("--- Reset BSS 1 with the old BVCI ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], 0x1002);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	send_bssgp_reset_ack(nsi, &sgsn_peer, 0x1002);

	printf("--- Reset BSS 1 with the old BVCI again ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], 0x1002);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	send_bssgp_reset_ack(nsi, &sgsn_peer, 0x1002);

	printf("--- Send message from BSS 1 to SGSN, BVCI 0x1012 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], 0x1012, (uint8_t *)"", 0);

	printf("--- Send message from SGSN to BSS 1, BVCI 0x1012 ---\n\n");

	send_ns_unitdata(nsi, NULL, &sgsn_peer, 0x1012, (uint8_t *)"", 0);

	printf("--- Send message from BSS 1 to SGSN, BVCI 0x1002 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], 0x1012, (uint8_t *)"", 0);

	printf("--- Send message from SGSN to BSS 1, BVCI 0x1002 ---\n\n");

	send_ns_unitdata(nsi, NULL, &sgsn_peer, 0x1012, (uint8_t *)"", 0);

	printf("--- Send message from BSS 2 to SGSN, BVCI 0x2002 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], 0x2002, (uint8_t *)"", 0);

	printf("--- Send message from SGSN to BSS 2, BVCI 0x2002 ---\n\n");

	send_ns_unitdata(nsi, NULL, &sgsn_peer, 0x2002, (uint8_t *)"", 0);

	printf("--- Reset BSS 1 with the old BVCI on BSS2's link ---\n\n");

	setup_bssgp(nsi, &bss_peer[2], 0x1002);
	gprs_dump_nsi(nsi);
	gbprox_dump_peers(stdout, 0, 1);

	gbprox_dump_global(stdout, 0, 1);

	send_bssgp_reset_ack(nsi, &sgsn_peer, 0x1002);

	printf("--- Send message from BSS 1 to SGSN, BVCI 0x1002 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], 0x1012, (uint8_t *)"", 0);

	printf("--- Send message from SGSN to BSS 1, BVCI 0x1002 ---\n\n");

	send_ns_unitdata(nsi, NULL, &sgsn_peer, 0x1012, (uint8_t *)"", 0);

	printf("--- Send message from SGSN to BSS 1, BVCI 0x10ff (invalid) ---\n\n");

	send_ns_unitdata(nsi, NULL, &sgsn_peer, 0x10ff, (uint8_t *)"", 0);

	gbprox_dump_global(stdout, 0, 1);

	gprs_ns_destroy(nsi);
	nsi = NULL;
	gbprox_reset();
}

static void test_gbproxy_ident_changes()
{
	struct gprs_ns_inst *nsi = gprs_ns_instantiate(gprs_ns_callback, NULL);
	struct sockaddr_in bss_peer[1] = {{0},};
	struct sockaddr_in sgsn_peer= {0};
	uint16_t nsei[2] = {0x1000, 0x2000};
	uint16_t nsvci[2] = {0x1001, 0x2001};
	uint16_t bvci[4] = {0x1002, 0x2002, 0x3002, 0x4002};

	bssgp_nsi = nsi;
	gbcfg.nsi = bssgp_nsi;
	gbcfg.nsip_sgsn_nsei = SGSN_NSEI;

	sgsn_peer.sin_family = AF_INET;
	sgsn_peer.sin_port = htons(32000);
	sgsn_peer.sin_addr.s_addr = htonl(REMOTE_SGSN_ADDR);

	bss_peer[0].sin_family = AF_INET;
	bss_peer[0].sin_port = htons(1111);
	bss_peer[0].sin_addr.s_addr = htonl(REMOTE_BSS_ADDR);

	printf("--- Initialise SGSN ---\n\n");

	gprs_ns_nsip_connect(nsi, &sgsn_peer, SGSN_NSEI, SGSN_NSEI+1);
	send_ns_reset_ack(nsi, &sgsn_peer, SGSN_NSEI+1, SGSN_NSEI);
	send_ns_alive_ack(nsi, &sgsn_peer);
	send_ns_unblock_ack(nsi, &sgsn_peer);
	send_ns_alive(nsi, &sgsn_peer);
	gprs_dump_nsi(nsi);

	printf("--- Initialise BSS 1 ---\n\n");

	setup_ns(nsi, &bss_peer[0], nsvci[0], nsei[0]);
	gprs_dump_nsi(nsi);

	printf("--- Setup BVCI 1 ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], bvci[0]);
	send_bssgp_reset_ack(nsi, &sgsn_peer, bvci[0]);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Setup BVCI 2 ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], bvci[1]);
	send_bssgp_reset_ack(nsi, &sgsn_peer, bvci[1]);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 1 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[0], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[0], (uint8_t *)"", 0);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 2 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[1], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[1], (uint8_t *)"", 0);

	printf("--- Change NSEI ---\n\n");

	setup_ns(nsi, &bss_peer[0], nsvci[0], nsei[1]);
	gprs_dump_nsi(nsi);

	printf("--- Setup BVCI 1 ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], bvci[0]);
	send_bssgp_reset_ack(nsi, &sgsn_peer, bvci[0]);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Setup BVCI 3 ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], bvci[2]);
	send_bssgp_reset_ack(nsi, &sgsn_peer, bvci[2]);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 1 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[0], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[0], (uint8_t *)"", 0);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 2 "
	       " (should fail) ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[1], (uint8_t *)"", 0);
	gbprox_dump_peers(stdout, 0, 1);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[1], (uint8_t *)"", 0);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 3 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[2], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[2], (uint8_t *)"", 0);

	printf("--- Change NSVCI ---\n\n");

	setup_ns(nsi, &bss_peer[0], nsvci[1], nsei[1]);
	gprs_dump_nsi(nsi);

	printf("--- Setup BVCI 1 ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], bvci[0]);
	send_bssgp_reset_ack(nsi, &sgsn_peer, bvci[0]);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Setup BVCI 4 ---\n\n");

	setup_bssgp(nsi, &bss_peer[0], bvci[3]);
	send_bssgp_reset_ack(nsi, &sgsn_peer, bvci[3]);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 1 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[0], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[0], (uint8_t *)"", 0);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 2 "
	       " (should fail) ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[1], (uint8_t *)"", 0);
	gbprox_dump_peers(stdout, 0, 1);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[1], (uint8_t *)"", 0);
	gbprox_dump_peers(stdout, 0, 1);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 3 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[2], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[2], (uint8_t *)"", 0);

	printf("--- Send message from BSS 1 to SGSN and back, BVCI 4 ---\n\n");

	send_ns_unitdata(nsi, NULL, &bss_peer[0], bvci[3], (uint8_t *)"", 0);
	send_ns_unitdata(nsi, NULL, &sgsn_peer, bvci[3], (uint8_t *)"", 0);

	gbprox_dump_global(stdout, 0, 1);
	gbprox_dump_peers(stdout, 0, 1);

	gprs_ns_destroy(nsi);
	nsi = NULL;
	gbprox_reset();
}


static struct log_info info = {};

int main(int argc, char **argv)
{
	osmo_init_logging(&info);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_print_filename(osmo_stderr_target, 0);
	osmo_signal_register_handler(SS_L_NS, &test_signal, NULL);

	log_set_print_filename(osmo_stderr_target, 0);
	log_set_log_level(osmo_stderr_target, LOGL_INFO);

	rate_ctr_init(NULL);

	setlinebuf(stdout);

	printf("===== GbProxy test START\n");
	test_gbproxy();
	test_gbproxy_ident_changes();
	printf("===== GbProxy test END\n\n");

	exit(EXIT_SUCCESS);
}
