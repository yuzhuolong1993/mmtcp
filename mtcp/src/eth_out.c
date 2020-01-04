#include <stdio.h>

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <linux/if_ether.h>
#include <linux/tcp.h>
#include <netinet/ip.h>

#include "mtcp.h"
#include "arp.h"
#include "eth_out.h"
#include "debug.h"

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#define MAX_WINDOW_SIZE 65535

/*----------------------------------------------------------------------------*/
uint8_t *
EthernetOutput(struct mtcp_manager *mtcp, uint16_t h_proto, 
		int nif, unsigned char* dst_haddr, uint16_t iplen)
{
	uint8_t *buf;
	struct ethhdr *ethh;
	int i, eidx;

	/* 
 	 * -sanity check- 
	 * return early if no interface is set (if routing entry does not exist)
	 */
	if (nif < 0) {
		TRACE_INFO("No interface set!\n");
		return NULL;
	}

	eidx = CONFIG.nif_to_eidx[nif];
	if (eidx < 0) {
		TRACE_INFO("No interface selected!\n");
		return NULL;
	}
	
	buf = mtcp->iom->get_wptr(mtcp->ctx, eidx, iplen + ETHERNET_HEADER_LEN);
	if (!buf) {
		//TRACE_DBG("Failed to get available write buffer\n");
		return NULL;
	}
	//memset(buf, 0, ETHERNET_HEADER_LEN + iplen);

#if 0
	TRACE_DBG("dst_hwaddr: %02X:%02X:%02X:%02X:%02X:%02X\n",
				dst_haddr[0], dst_haddr[1], 
				dst_haddr[2], dst_haddr[3], 
				dst_haddr[4], dst_haddr[5]);
#endif

	ethh = (struct ethhdr *)buf;
	// ** CHANGE HERE ** //
	// ** Put current sending rate into the dst ethernet address ** //
	uint32_t ntohl_bpms;
	ntohl_bpms = ntohl(mtcp->ms_nstat.byts_send_ms[eidx]);
	dst_haddr[0] = 0;
	dst_haddr[1] = 0;
	dst_haddr[2] = (ntohl_bpms) & 0xFF;
	dst_haddr[3] = (ntohl_bpms >> 8) & 0xFF;
	dst_haddr[4] = (ntohl_bpms >> 16) & 0xFF;
	dst_haddr[5] = (ntohl_bpms >> 24) & 0xFF;

	for (i = 0; i < ETH_ALEN; i++) {
		ethh->h_source[i] = i+1;
		// ethh->h_source[i] = CONFIG.eths[eidx].haddr[i];
		ethh->h_dest[i] = dst_haddr[i];
		

	}
	ethh->h_proto = htons(h_proto);

	return (uint8_t *)(ethh + 1);
}
/*----------------------------------------------------------------------------*/
#ifdef MS_RATE_CAL // ** put in flow_id actually
uint8_t *
EthernetOutputWithFlowID(struct mtcp_manager *mtcp, uint16_t h_proto, 
		int nif, unsigned char* dst_haddr, uint16_t iplen, uint32_t flow_id, uint32_t tenant_id)
{
	uint8_t *buf;
	struct ethhdr *ethh;
	int i, eidx;

	/* 
 	 * -sanity check- 
	 * return early if no interface is set (if routing entry does not exist)
	 */
	if (nif < 0) {
		TRACE_INFO("No interface set!\n");
		return NULL;
	}

	eidx = CONFIG.nif_to_eidx[nif];
	if (eidx < 0) {
		TRACE_INFO("No interface selected!\n");
		return NULL;
	}
	
	buf = mtcp->iom->get_wptr(mtcp->ctx, eidx, iplen + ETHERNET_HEADER_LEN);
	if (!buf) {
		//TRACE_DBG("Failed to get available write buffer\n");
		return NULL;
	}
	//memset(buf, 0, ETHERNET_HEADER_LEN + iplen);

#if 0
	TRACE_DBG("dst_hwaddr: %02X:%02X:%02X:%02X:%02X:%02X\n",
				dst_haddr[0], dst_haddr[1], 
				dst_haddr[2], dst_haddr[3], 
				dst_haddr[4], dst_haddr[5]);
#endif

	ethh = (struct ethhdr *)buf;
	// ** CHANGE HERE ** //
	// ** Put current sending rate into the dst ethernet address ** //
	uint32_t ntohl_bpms;
	ntohl_bpms = ntohl(mtcp->ms_nstat.byts_send_ms[eidx]);
	dst_haddr[0] = 0;
	dst_haddr[1] = 0;
	dst_haddr[2] = (ntohl_bpms) & 0xFF;
	dst_haddr[3] = (ntohl_bpms >> 8) & 0xFF;
	dst_haddr[4] = (ntohl_bpms >> 16) & 0xFF;
	dst_haddr[5] = (ntohl_bpms >> 24) & 0xFF;
	// ** CHANGE HERE ** //
	// ** put flow_id and tenant_id into the src ethernet address ** //
	unsigned char src_haddr[6];
	tenant_id = ntohs(tenant_id);
	flow_id = ntohl(flow_id);
	src_haddr[0] = tenant_id & 0xFF;
	src_haddr[1] = (tenant_id >> 8) & 0xFF;
	src_haddr[2] = flow_id & 0xFF;
	src_haddr[3] = (flow_id >> 8) & 0xFF;
	src_haddr[4] = (flow_id >> 16) & 0xFF;
	src_haddr[5] = (flow_id >> 24) & 0xFF;

	for (i = 0; i < ETH_ALEN; i++) {
		ethh->h_source[i] = src_haddr[i];
		// ethh->h_source[i] = CONFIG.eths[eidx].haddr[i];
		ethh->h_dest[i] = dst_haddr[i];
	}
	ethh->h_proto = htons(h_proto);

	return (uint8_t *)(ethh + 1);
}
#endif
/*----------------------------------------------------------------------------*/
