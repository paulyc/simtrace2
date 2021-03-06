/* ISO7816-3 state machine for the card side
 *
 * (C) 2010-2017 by Harald Welte <laforge@gnumonks.org>
 * (C) 2018 by sysmocom -s.f.m.c. GmbH, Author: Kevin Redon <kredon@sysmocom.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA
 */
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

#include "utils.h"
#include "trace.h"
#include "iso7816_fidi.h"
#include "tc_etu.h"
#include "card_emu.h"
#include "simtrace_prot.h"
#include "usb_buf.h"
#include <osmocom/core/linuxlist.h>
#include <osmocom/core/msgb.h>


#define NUM_SLOTS		2

#define	ISO7816_3_INIT_WTIME	9600
#define ISO7816_3_DEFAULT_WI	10
#define ISO7816_3_ATR_LEN_MAX	(1+32)	/* TS plus 32 chars */

#define ISO7816_3_PB_NULL	0x60

enum iso7816_3_card_state {
	ISO_S_WAIT_POWER,	/* waiting for power being applied */
	ISO_S_WAIT_CLK,		/* waiting for clock being applied */
	ISO_S_WAIT_RST,		/* waiting for reset being released */
	ISO_S_WAIT_ATR,		/* waiting for start of ATR */
	ISO_S_IN_ATR,		/* transmitting ATR to reader */
	ISO_S_IN_PTS,		/* transmitting ATR to reader */
	ISO_S_WAIT_TPDU,	/* waiting for data from reader */
	ISO_S_IN_TPDU,		/* inside a TPDU */
};

const struct value_string iso7816_3_card_state_names[] = {
	{
		.value = ISO_S_WAIT_POWER,
		.str = "WAIT_POWER",
	},
	{
		.value = ISO_S_WAIT_CLK,
		.str = "WAIT_CLK",
	},
	{
		.value = ISO_S_WAIT_RST,
		.str = "WAIT_RST",
	},
	{
		.value = ISO_S_WAIT_ATR,
		.str = "WAIT_ATR",
	},
	{
		.value = ISO_S_IN_ATR,
		.str = "IN_ATR",
	},
	{
		.value = ISO_S_IN_PTS,
		.str = "IN_PTS",
	},
	{
		.value = ISO_S_WAIT_TPDU,
		.str = "WAIT_TPDU",
	},
	{
		.value = ISO_S_IN_TPDU,
		.str = "IN_TPDU",
	},
	{
		.value = 0,
		.str = NULL,
	},
};


/* detailed sub-states of ISO_S_IN_PTS */
enum pts_state {
	PTS_S_WAIT_REQ_PTSS,
	PTS_S_WAIT_REQ_PTS0,
	PTS_S_WAIT_REQ_PTS1,
	PTS_S_WAIT_REQ_PTS2,
	PTS_S_WAIT_REQ_PTS3,
	PTS_S_WAIT_REQ_PCK,
	PTS_S_WAIT_RESP_PTSS = PTS_S_WAIT_REQ_PTSS | 0x10,
	PTS_S_WAIT_RESP_PTS0 = PTS_S_WAIT_REQ_PTS0 | 0x10,
	PTS_S_WAIT_RESP_PTS1 = PTS_S_WAIT_REQ_PTS1 | 0x10,
	PTS_S_WAIT_RESP_PTS2 = PTS_S_WAIT_REQ_PTS2 | 0x10,
	PTS_S_WAIT_RESP_PTS3 = PTS_S_WAIT_REQ_PTS3 | 0x10,
	PTS_S_WAIT_RESP_PCK = PTS_S_WAIT_REQ_PCK | 0x10,
};

/* PTS field byte index */
#define _PTSS	0
#define _PTS0	1
#define _PTS1	2
#define _PTS2	3
#define _PTS3	4
#define _PCK	5

/* T-PDU state machine states */
enum tpdu_state {
	TPDU_S_WAIT_CLA,	/* waiting for CLA byte from reader */
	TPDU_S_WAIT_INS,	/* waiting for INS byte from reader */
	TPDU_S_WAIT_P1,		/* waiting for P1 byte from reader */
	TPDU_S_WAIT_P2,		/* waiting for P2 byte from reader */
	TPDU_S_WAIT_P3,		/* waiting for P3 byte from reader */
	TPDU_S_WAIT_PB,		/* waiting for Tx of procedure byte */
	TPDU_S_WAIT_RX,		/* waiting for more data from reader */
	TPDU_S_WAIT_TX,		/* waiting for more data to reader */
};

const struct value_string tpdu_state_names[] = {
	{
		.value = TPDU_S_WAIT_CLA,
		.str = "WAIT_CLA",
	},
	{
		.value = TPDU_S_WAIT_INS,
		.str = "WAIT_INS",
	},
	{
		.value = TPDU_S_WAIT_P1,
		.str = "WAIT_P1",
	},
	{
		.value = TPDU_S_WAIT_P2,
		.str = "WAIT_P2",
	},
	{
		.value = TPDU_S_WAIT_P3,
		.str = "WAIT_P3",
	},
	{
		.value = TPDU_S_WAIT_PB,
		.str = "WAIT_PB",
	},
	{
		.value = TPDU_S_WAIT_RX,
		.str = "WAIT_RX",
	},
	{
		.value = TPDU_S_WAIT_TX,
		.str = "WAIT_TX",
	},
	{
		.value = 0,
		.str = NULL,
	},
};

/* TPDU field byte index */
#define	_CLA	0
#define	_INS	1
#define	_P1	2
#define	_P2	3
#define	_P3	4

struct card_handle {
	unsigned int num;

	enum iso7816_3_card_state state;

	/* signal levels */
	uint8_t vcc_active;	/* 1 = on, 0 = off */
	uint8_t in_reset;	/* 1 = RST low, 0 = RST high */
	uint8_t clocked;	/* 1 = active, 0 = inactive */

	/* timing parameters, from PTS */
	uint8_t fi;
	uint8_t di;
	uint8_t wi;

	uint8_t tc_chan;	/* TC channel number */
	uint8_t uart_chan;	/* UART channel */

	uint8_t in_ep;		/* USB IN EP */
	uint8_t irq_ep;		/* USB IN EP */

	uint32_t waiting_time;	/* in clocks */

	/* ATR state machine */
	struct {
		uint8_t idx;
		uint8_t len;
		//uint8_t hist_len;
		//uint8_t last_td;
		uint8_t atr[ISO7816_3_ATR_LEN_MAX];
	} atr;

	/* PPS / PTS support */
	struct {
		enum pts_state state;
		uint8_t req[6];		/* request bytes */
		uint8_t resp[6];	/* response bytes */
	} pts;

	/* TPDU */
	struct {
		enum tpdu_state state;
		uint8_t hdr[5];		/* CLA INS P1 P2 P3 */
	} tpdu;

	struct msgb *uart_rx_msg;	/* UART RX -> USB TX */
	struct msgb *uart_tx_msg;	/* USB RX -> UART TX */

	struct llist_head uart_tx_queue;

	struct {
		uint32_t tx_bytes;
		uint32_t rx_bytes;
		uint32_t pps;
	} stats;
};

struct llist_head *card_emu_get_uart_tx_queue(struct card_handle *ch)
{
	return &ch->uart_tx_queue;
}

static void set_tpdu_state(struct card_handle *ch, enum tpdu_state new_ts);
static void set_pts_state(struct card_handle *ch, enum pts_state new_ptss);

/* update simtrace header msg_len and submit USB buffer */
void usb_buf_upd_len_and_submit(struct msgb *msg)
{
	struct simtrace_msg_hdr *sh = (struct simtrace_msg_hdr *) msg->l1h;

	sh->msg_len = msgb_length(msg);

	usb_buf_submit(msg);
}

/* Allocate USB buffer and push + initialize simtrace_msg_hdr */
struct msgb *usb_buf_alloc_st(uint8_t ep, uint8_t msg_class, uint8_t msg_type)
{
	struct msgb *msg;
	struct simtrace_msg_hdr *sh;

	msg = usb_buf_alloc(ep);
	if (!msg)
		return NULL;

	msg->l1h = msgb_put(msg, sizeof(*sh));
	sh = (struct simtrace_msg_hdr *) msg->l1h;
	memset(sh, 0, sizeof(*sh));
	sh->msg_class = msg_class;
	sh->msg_type = msg_type;
	msg->l2h = msg->l1h + sizeof(*sh);

	return msg;
}

/* Update cardemu_usb_msg_rx_data length + submit buffer */
static void flush_rx_buffer(struct card_handle *ch)
{
	struct msgb *msg;
	struct cardemu_usb_msg_rx_data *rd;
	uint32_t data_len;

	msg = ch->uart_rx_msg;
	if (!msg)
		return;

	ch->uart_rx_msg = NULL;

	/* store length of data payload field in header */
	rd = (struct cardemu_usb_msg_rx_data *) msg->l2h;
	rd->data_len = msgb_l2len(msg) - sizeof(*rd);

	TRACE_INFO("%u: %s (%u)\n\r",
			ch->num, __func__, rd->data_len);

	usb_buf_upd_len_and_submit(msg);
}

/* convert a non-contiguous PTS request/response into a contiguous
 * buffer, returning the number of bytes used in the buffer */
static int serialize_pts(uint8_t *out,  const uint8_t *in)
{
	int i = 0;

	out[i++] = in[_PTSS];
	out[i++] = in[_PTS0];
	if (in[_PTS0] & (1 << 4))
		out[i++] = in[_PTS1];
	if (in[_PTS0] & (1 << 5))
		out[i++] = in[_PTS2];
	if (in[_PTS0] & (1 << 6))
		out[i++] = in[_PTS3];
	out[i++] = in[_PCK];

	return i;
}

static uint8_t csum_pts(const uint8_t *in)
{
	uint8_t out[6];
	int len = serialize_pts(out, in);
	uint8_t csum = 0;
	int i;

	/* we don't include the PCK byte in the checksumming process */
	len -= 1;

	for (i = 0; i < len; i++)
		csum = csum ^ out[i];

	return csum;
}

static void flush_pts(struct card_handle *ch)
{
	struct msgb *msg;
	struct cardemu_usb_msg_pts_info *ptsi;

	msg = usb_buf_alloc_st(ch->in_ep, SIMTRACE_MSGC_CARDEM, SIMTRACE_MSGT_DO_CEMU_PTS);
	if (!msg)
		return;

	ptsi = (struct cardemu_usb_msg_pts_info *) msgb_put(msg, sizeof(*ptsi));
	ptsi->pts_len = serialize_pts(ptsi->req, ch->pts.req);
	serialize_pts(ptsi->resp, ch->pts.resp);

	usb_buf_upd_len_and_submit(msg);
}

static void emu_update_fidi(struct card_handle *ch)
{
	int rc;

	rc = compute_fidi_ratio(ch->fi, ch->di);
	if (rc > 0 && rc < 0x400) {
		TRACE_INFO("%u: computed Fi(%u) Di(%u) ratio: %d\r\n",
			    ch->num, ch->fi, ch->di, rc);
		/* make sure UART uses new F/D ratio */
		card_emu_uart_update_fidi(ch->uart_chan, rc);
		/* notify ETU timer about this */
		tc_etu_set_etu(ch->tc_chan, rc);
	} else
		TRACE_INFO("%u: computed FiDi ration %d unsupported\r\n",
			   ch->num, rc);
}

/* Update the ISO 7816-3 TPDU receiver state */
static void card_set_state(struct card_handle *ch,
			   enum iso7816_3_card_state new_state)
{
	if (ch->state == new_state)
		return;

	TRACE_DEBUG("%u: 7816 card state %s -> %s\r\n", ch->num,
		    get_value_string(iso7816_3_card_state_names, ch->state),
		    get_value_string(iso7816_3_card_state_names, new_state));
	ch->state = new_state;

	switch (new_state) {
	case ISO_S_WAIT_POWER:
	case ISO_S_WAIT_CLK:
	case ISO_S_WAIT_RST:
		/* disable Rx and Tx of UART */
		card_emu_uart_enable(ch->uart_chan, 0);
		break;
	case ISO_S_WAIT_ATR:
		/* Reset to initial Fi / Di ratio */
		ch->fi = 1;
		ch->di = 1;
		emu_update_fidi(ch);
		/* the ATR should only be sent 400 to 40k clock cycles after the RESET.
		 * we use the tc_etu mechanism to wait this time.
		 * since the initial ETU is Fd=372/Dd=1 clock cycles long, we have to wait 2-107 ETU.
		 */
		tc_etu_set_wtime(ch->tc_chan, 2);
		/* ensure the TC_ETU timer is enabled */
		tc_etu_enable(ch->tc_chan);
		break;
	case ISO_S_IN_ATR:
		/* initialize to default WI, this will be overwritten if we
		 * send TC2, and it will be programmed into hardware after
		 * ATR is finished */
		ch->wi = ISO7816_3_DEFAULT_WI;
		/* update waiting time to initial waiting time */
		ch->waiting_time = ISO7816_3_INIT_WTIME;
		/* set initial waiting time */
		tc_etu_set_wtime(ch->tc_chan, ch->waiting_time);
		/* Set ATR sub-state to initial state */
		ch->atr.idx = 0;
		/* enable USART transmission to reader */
		card_emu_uart_enable(ch->uart_chan, ENABLE_TX);
		/* trigger USART TX IRQ to sent first ATR byte TS */
		card_emu_uart_interrupt(ch->uart_chan);
		break;
	case ISO_S_WAIT_TPDU:
		/* enable the receiver, disable transmitter */
		set_tpdu_state(ch, TPDU_S_WAIT_CLA);
		card_emu_uart_enable(ch->uart_chan, ENABLE_RX);
		break;
	case ISO_S_IN_PTS:
	case ISO_S_IN_TPDU:
		/* do nothing */
		break;
	}
}

/**********************************************************************
 * ATR handling
 **********************************************************************/

/*! Transmit ATR data to reader
 *  @param[in] ch card interface connected to reader
 *  @return numbers of bytes transmitted
 */
static int tx_byte_atr(struct card_handle *ch)
{
	if (NULL == ch) {
		TRACE_ERROR("ATR TX: no card handle provided\n\r");
		return 0;
	}
	if (ISO_S_IN_ATR != ch->state) {
		TRACE_ERROR("%u: ATR TX: no in ATR state\n\r", ch->num);
		return 0;
	}

	/* Transmit ATR */
	if (ch->atr.idx < ch->atr.len) {
		uint8_t byte = ch->atr.atr[ch->atr.idx++];
		card_emu_uart_tx(ch->uart_chan, byte);
		return 1;
	} else { /* The ATR has been completely transmitted */
		/* search for TC2 to updated WI */
		ch->wi = ISO7816_3_DEFAULT_WI;
		if (ch->atr.len >= 2 && ch->atr.atr[1] & 0xf0) { /* Y1 has some data */
			uint8_t atr_td1 = 2;
			if (ch->atr.atr[1] & 0x10) { /* TA1 is present */
				atr_td1++;
			}
			if (ch->atr.atr[1] & 0x20) { /* TB1 is present */
				atr_td1++;
			}
			if (ch->atr.atr[1] & 0x40) { /* TC1 is present */
				atr_td1++;
			}
			if (ch->atr.atr[1] & 0x80) { /* TD1 is present */
				if (ch->atr.len > atr_td1 && ch->atr.atr[atr_td1] & 0xf0) { /* Y2 has some data */
					uint8_t atr_tc2 = atr_td1+1;
					if (ch->atr.atr[atr_td1] & 0x10) { /* TA2 is present */
						atr_tc2++;
					}
					if (ch->atr.atr[atr_td1] & 0x20) { /* TB2 is present */
						atr_tc2++;
					}
					if (ch->atr.atr[atr_td1] & 0x40) { /* TC2 is present */
						if (ch->atr.len > atr_tc2 && ch->atr.atr[atr_tc2]) { /* TC2 encodes WI */
							ch->wi = ch->atr.atr[atr_tc2]; /* set WI */
						}
					}
				}
			}
		}
		/* update waiting time (see ISO 7816-3 10.2) */
		ch->waiting_time = ch->wi * 960 * ch->fi;
		tc_etu_set_wtime(ch->tc_chan, ch->waiting_time);
		/* reset PTS to initial state */
		set_pts_state(ch, PTS_S_WAIT_REQ_PTSS);
		/* go to next state */
		card_set_state(ch, ISO_S_WAIT_TPDU);
		return 0;
	}

	/* return number of bytes transmitted */
	return 1;
}

/**********************************************************************
 * PTS / PPS handling
 **********************************************************************/

/* Update the PTS sub-state */
static void set_pts_state(struct card_handle *ch, enum pts_state new_ptss)
{
	TRACE_DEBUG("%u: 7816 PTS state %u -> %u\r\n",
		    ch->num, ch->pts.state, new_ptss);
	ch->pts.state = new_ptss;
}

/* Determine the next PTS state */
static enum pts_state next_pts_state(struct card_handle *ch)
{
	uint8_t is_resp = ch->pts.state & 0x10;
	uint8_t sstate = ch->pts.state & 0x0f;
	uint8_t *pts_ptr;

	if (!is_resp)
		pts_ptr = ch->pts.req;
	else
		pts_ptr = ch->pts.resp;

	switch (sstate) {
	case PTS_S_WAIT_REQ_PTSS:
		goto from_ptss;
	case PTS_S_WAIT_REQ_PTS0:
		goto from_pts0;
	case PTS_S_WAIT_REQ_PTS1:
		goto from_pts1;
	case PTS_S_WAIT_REQ_PTS2:
		goto from_pts2;
	case PTS_S_WAIT_REQ_PTS3:
		goto from_pts3;
	}

	if (ch->pts.state == PTS_S_WAIT_REQ_PCK)
		return PTS_S_WAIT_RESP_PTSS;

from_ptss:
	return PTS_S_WAIT_REQ_PTS0 | is_resp;
from_pts0:
	if (pts_ptr[_PTS0] & (1 << 4))
		return PTS_S_WAIT_REQ_PTS1 | is_resp;
from_pts1:
	if (pts_ptr[_PTS0] & (1 << 5))
		return PTS_S_WAIT_REQ_PTS2 | is_resp;
from_pts2:
	if (pts_ptr[_PTS0] & (1 << 6))
		return PTS_S_WAIT_REQ_PTS3 | is_resp;
from_pts3:
	return PTS_S_WAIT_REQ_PCK | is_resp;
}


static int
process_byte_pts(struct card_handle *ch, uint8_t byte)
{
	switch (ch->pts.state) {
	case PTS_S_WAIT_REQ_PTSS:
		ch->pts.req[_PTSS] = byte;
		break;
	case PTS_S_WAIT_REQ_PTS0:
		ch->pts.req[_PTS0] = byte;
		break;
	case PTS_S_WAIT_REQ_PTS1:
		ch->pts.req[_PTS1] = byte;
		break;
	case PTS_S_WAIT_REQ_PTS2:
		ch->pts.req[_PTS2] = byte;
		break;
	case PTS_S_WAIT_REQ_PTS3:
		ch->pts.req[_PTS3] = byte;
		break;
	case PTS_S_WAIT_REQ_PCK:
		ch->pts.req[_PCK] = byte;
		if (ch->pts.req[_PCK] != csum_pts(ch->pts.req)) {
			TRACE_ERROR("%u: Error in PTS Checksum!\r\n",
				    ch->num);
			/* Wait for the next TPDU */
			set_pts_state(ch, PTS_S_WAIT_REQ_PTSS);
			return ISO_S_WAIT_TPDU;
		}
		/* FIXME: check if proposal matches capabilities in ATR */
		memcpy(ch->pts.resp, ch->pts.req, sizeof(ch->pts.resp));
		break;
	default:
		TRACE_ERROR("%u: process_byte_pts() in invalid state %u\r\n",
			    ch->num, ch->pts.state);
		break;
	}
	/* calculate the next state and set it */
	set_pts_state(ch, next_pts_state(ch));

	if (ch->pts.state == PTS_S_WAIT_RESP_PTSS) {
		flush_pts(ch);
		/* activate UART TX to transmit PTS response */
		card_emu_uart_enable(ch->uart_chan, ENABLE_TX);
		/* don't fall-through to the 'return ISO_S_IN_PTS'
		 * below, rather keep ISO7816 state as-is, it will be
		 * further updated by the tx-completion handler */
		return -1;
	}

	return ISO_S_IN_PTS;
}

/* return a single byte to be transmitted to the reader */
static int tx_byte_pts(struct card_handle *ch)
{
	uint8_t byte;

	/* 1: Determine the next transmit byte */
	switch (ch->pts.state) {
	case PTS_S_WAIT_RESP_PTSS:
		byte = ch->pts.resp[_PTSS];
		break;
	case PTS_S_WAIT_RESP_PTS0:
		byte = ch->pts.resp[_PTS0];
		break;
	case PTS_S_WAIT_RESP_PTS1:
		byte = ch->pts.resp[_PTS1];
		/* This must be TA1 */
		ch->fi = byte >> 4;
		ch->di = byte & 0xf;
		TRACE_DEBUG("%u: found Fi=%u Di=%u\r\n", ch->num,
			    ch->fi, ch->di);
		break;
	case PTS_S_WAIT_RESP_PTS2:
		byte = ch->pts.resp[_PTS2];
		break;
	case PTS_S_WAIT_RESP_PTS3:
		byte = ch->pts.resp[_PTS3];
		break;
	case PTS_S_WAIT_RESP_PCK:
		byte = ch->pts.resp[_PCK];
		break;
	default:
		TRACE_ERROR("%u: get_byte_pts() in invalid state %u\r\n",
			    ch->num, ch->pts.state);
		return 0;
	}

	/* 2: Transmit the byte */
	card_emu_uart_tx(ch->uart_chan, byte);

	/* 3: Update the state */

	switch (ch->pts.state) {
	case PTS_S_WAIT_RESP_PCK:
		card_emu_uart_wait_tx_idle(ch->uart_chan);
		/* update baud rate generator with Fi/Di */
		emu_update_fidi(ch);
		/* Wait for the next TPDU */
		card_set_state(ch, ISO_S_WAIT_TPDU);
		set_pts_state(ch, PTS_S_WAIT_REQ_PTSS);
		break;
	default:
		/* calculate the next state and set it */
		set_pts_state(ch, next_pts_state(ch));
		break;
	}

	/* return number of bytes transmitted */
	return 1;
}


/**********************************************************************
 * TPDU handling
 **********************************************************************/


/* compute number of data bytes according to Chapter 10.3.2 of 7816-3 */
static unsigned int t0_num_data_bytes(uint8_t p3, int reader_to_card)
{
	if (reader_to_card) {
		return p3;
	} else {
		if (p3 == 0)
			return 256;
		else
			return p3;
	}
}

/* add a just-received TPDU byte (from reader) to USB buffer */
static void add_tpdu_byte(struct card_handle *ch, uint8_t byte)
{
	struct msgb *msg;
	struct cardemu_usb_msg_rx_data *rd;
	unsigned int num_data_bytes = t0_num_data_bytes(ch->tpdu.hdr[_P3], 0);

	/* ensure we have a buffer */
	if (!ch->uart_rx_msg) {
		msg = ch->uart_rx_msg = usb_buf_alloc_st(ch->in_ep, SIMTRACE_MSGC_CARDEM,
							 SIMTRACE_MSGT_DO_CEMU_RX_DATA);
		if (!ch->uart_rx_msg) {
			TRACE_ERROR("%u: Received UART byte but ENOMEM\r\n",
				    ch->num);
			return;
		}
		msgb_put(msg, sizeof(*rd));
	} else
		msg = ch->uart_rx_msg;

	rd = (struct cardemu_usb_msg_rx_data *) msg->l2h;
	msgb_put_u8(msg, byte);

	/* check if the buffer is full. If so, send it */
	if (msgb_l2len(msg) >= sizeof(*rd) + num_data_bytes) {
		rd->flags |= CEMU_DATA_F_FINAL;
		flush_rx_buffer(ch);
		/* We need to transmit the SW now, */
		set_tpdu_state(ch, TPDU_S_WAIT_TX);
	} else if (msgb_tailroom(msg) <= 0)
		flush_rx_buffer(ch);
}

static void set_tpdu_state(struct card_handle *ch, enum tpdu_state new_ts)
{
	if (ch->tpdu.state == new_ts)
		return;

	TRACE_DEBUG("%u: 7816 TPDU state %s -> %s\r\n", ch->num,
		get_value_string(tpdu_state_names, ch->tpdu.state),
		get_value_string(tpdu_state_names, new_ts));
	ch->tpdu.state = new_ts;

	switch (new_ts) {
	case TPDU_S_WAIT_CLA:
	case TPDU_S_WAIT_RX:
		card_emu_uart_enable(ch->uart_chan, ENABLE_RX);
		break;
	case TPDU_S_WAIT_PB:
		/* we just completed the TPDU header from reader to card
		 * and now need to disable the receiver, enable the
		 * transmitter and transmit the procedure byte */
		card_emu_uart_enable(ch->uart_chan, ENABLE_TX);
		break;
	default:
		break;
	}
}

static enum tpdu_state next_tpdu_state(struct card_handle *ch)
{
	switch (ch->tpdu.state) {
	case TPDU_S_WAIT_CLA:
		return TPDU_S_WAIT_INS;
	case TPDU_S_WAIT_INS:
		return TPDU_S_WAIT_P1;
	case TPDU_S_WAIT_P1:
		return TPDU_S_WAIT_P2;
	case TPDU_S_WAIT_P2:
		return TPDU_S_WAIT_P3;
	case TPDU_S_WAIT_P3:
		return TPDU_S_WAIT_PB;
	/* simply stay in Rx or Tx by default */
	case TPDU_S_WAIT_PB:
		return TPDU_S_WAIT_PB;
	case TPDU_S_WAIT_RX:
		return TPDU_S_WAIT_RX;
	case TPDU_S_WAIT_TX:
		return TPDU_S_WAIT_TX;
	}
	/* we should never reach here */
	assert(0);
	return -1;
}

static void send_tpdu_header(struct card_handle *ch)
{
	struct msgb *msg;
	struct cardemu_usb_msg_rx_data *rd;
	uint8_t *cur;

	TRACE_INFO("%u: %s: %02x %02x %02x %02x %02x\r\n",
			ch->num, __func__,
			ch->tpdu.hdr[0], ch->tpdu.hdr[1],
			ch->tpdu.hdr[2], ch->tpdu.hdr[3],
			ch->tpdu.hdr[4]);

	/* if we already/still have a context, send it off */
	if (ch->uart_rx_msg) {
		TRACE_DEBUG("%u: have old buffer\r\n", ch->num);
		if (msgb_l2len(ch->uart_rx_msg)) {
			TRACE_DEBUG("%u: flushing old buffer\r\n", ch->num);
			flush_rx_buffer(ch);
		}
	}
	TRACE_DEBUG("%u: allocating new buffer\r\n", ch->num);
	/* ensure we have a new buffer */
	ch->uart_rx_msg = usb_buf_alloc_st(ch->in_ep, SIMTRACE_MSGC_CARDEM,
					   SIMTRACE_MSGT_DO_CEMU_RX_DATA);
	if (!ch->uart_rx_msg) {
		TRACE_ERROR("%u: %s: ENOMEM\r\n", ch->num, __func__);
		return;
	}
	msg = ch->uart_rx_msg;
	rd = (struct cardemu_usb_msg_rx_data *) msgb_put(msg, sizeof(*rd));

	/* initialize header */
	rd->flags = CEMU_DATA_F_TPDU_HDR;

	/* copy TPDU header to data field */
	cur = msgb_put(msg, sizeof(ch->tpdu.hdr));
	memcpy(cur, ch->tpdu.hdr, sizeof(ch->tpdu.hdr));
	/* rd->data_len is set in flush_rx_buffer() */

	flush_rx_buffer(ch);
}

static enum iso7816_3_card_state
process_byte_tpdu(struct card_handle *ch, uint8_t byte)
{
	switch (ch->tpdu.state) {
	case TPDU_S_WAIT_CLA:
		ch->tpdu.hdr[_CLA] = byte;
		set_tpdu_state(ch, next_tpdu_state(ch));
		break;
	case TPDU_S_WAIT_INS:
		ch->tpdu.hdr[_INS] = byte;
		set_tpdu_state(ch, next_tpdu_state(ch));
		break;
	case TPDU_S_WAIT_P1:
		ch->tpdu.hdr[_P1] = byte;
		set_tpdu_state(ch, next_tpdu_state(ch));
		break;
	case TPDU_S_WAIT_P2:
		ch->tpdu.hdr[_P2] = byte;
		set_tpdu_state(ch, next_tpdu_state(ch));
		break;
	case TPDU_S_WAIT_P3:
		ch->tpdu.hdr[_P3] = byte;
		set_tpdu_state(ch, next_tpdu_state(ch));
		/* FIXME: start timer to transmit further 0x60 */
		/* send the TPDU header as part of a procedure byte
		 * request to the USB host */
		send_tpdu_header(ch);
		break;
	case TPDU_S_WAIT_RX:
		add_tpdu_byte(ch, byte);
		break;
	default:
		TRACE_ERROR("%u: process_byte_tpdu() in invalid state %u\r\n",
			    ch->num, ch->tpdu.state);
	}

	/* ensure we stay in TPDU ISO state */
	return ISO_S_IN_TPDU;
}

/* tx a single byte to be transmitted to the reader */
static int tx_byte_tpdu(struct card_handle *ch)
{
	struct msgb *msg;
	struct cardemu_usb_msg_tx_data *td;
	uint8_t byte;

	/* ensure we are aware of any data that might be pending for
	 * transmit */
	if (!ch->uart_tx_msg) {
		/* uart_tx_queue is filled from main loop, so no need
		 * for irq-safe operations */
		if (llist_empty(&ch->uart_tx_queue))
			return 0;

		/* dequeue first at head */
		ch->uart_tx_msg = msgb_dequeue(&ch->uart_tx_queue);
		ch->uart_tx_msg->l1h = ch->uart_tx_msg->head;
		ch->uart_tx_msg->l2h = ch->uart_tx_msg->l1h + sizeof(struct simtrace_msg_hdr);
		msg = ch->uart_tx_msg;
		/* remove the header */
		msgb_pull(msg, sizeof(struct simtrace_msg_hdr) + sizeof(*td));
	}
	msg = ch->uart_tx_msg;
	td = (struct cardemu_usb_msg_tx_data *) msg->l2h;

	/* take the next pending byte out of the msgb */
	byte = msgb_pull_u8(msg);

	card_emu_uart_tx(ch->uart_chan, byte);

	/* this must happen _after_ the byte has been transmitted */
	switch (ch->tpdu.state) {
	case TPDU_S_WAIT_PB:
		/* if we just transmitted the procedure byte, we need to decide
		 * if we want to continue to receive or transmit */
		if (td->flags & CEMU_DATA_F_PB_AND_TX)
			set_tpdu_state(ch, TPDU_S_WAIT_TX);
		else if (td->flags & CEMU_DATA_F_PB_AND_RX)
			set_tpdu_state(ch, TPDU_S_WAIT_RX);
		break;
	default:
		break;
	}

	/* check if the buffer has now been fully transmitted */
	if (msgb_length(msg) == 0) {
		if (td->flags & CEMU_DATA_F_PB_AND_RX) {
			/* we have just sent the procedure byte and now
			 * need to continue receiving */
			set_tpdu_state(ch, TPDU_S_WAIT_RX);
		} else {
			/* we have transmitted all bytes */
			if (td->flags & CEMU_DATA_F_FINAL) {
				/* this was the final part of the APDU, go
				 * back to state one */
				card_set_state(ch, ISO_S_WAIT_TPDU);
			}
		}
		usb_buf_free(msg);
		ch->uart_tx_msg = NULL;
	}

	return 1;
}

/**********************************************************************
 * Public API
 **********************************************************************/

/* process a single byte received from the reader */
void card_emu_process_rx_byte(struct card_handle *ch, uint8_t byte)
{
	int new_state = -1;

	ch->stats.rx_bytes++;

	switch (ch->state) {
	case ISO_S_WAIT_TPDU:
		if (byte == 0xff) {
			new_state = process_byte_pts(ch, byte);
			ch->stats.pps++;
			goto out_silent;
		}
		/* fall-through */
	case ISO_S_IN_TPDU:
		new_state = process_byte_tpdu(ch, byte);
		break;
	case ISO_S_IN_PTS:
		new_state = process_byte_pts(ch, byte);
		goto out_silent;
	default:
		TRACE_ERROR("%u: Received UART char in invalid 7816 state "
			    "%u\r\n", ch->num, ch->state);
		break;
	}

out_silent:
	if (new_state != -1)
		card_set_state(ch, new_state);
}

/* transmit a single byte to the reader */
int card_emu_tx_byte(struct card_handle *ch)
{
	int rc = 0;

	switch (ch->state) {
	case ISO_S_IN_ATR:
		rc = tx_byte_atr(ch);
		break;
	case ISO_S_IN_PTS:
		rc = tx_byte_pts(ch);
		break;
	case ISO_S_IN_TPDU:
		rc = tx_byte_tpdu(ch);
		break;
	default:
		break;
	}

	if (rc)
		ch->stats.tx_bytes++;

	/* if we return 0 here, the UART needs to disable transmit-ready
	 * interrupts */
	return rc;
}

void card_emu_have_new_uart_tx(struct card_handle *ch)
{
	switch (ch->state) {
	case ISO_S_IN_TPDU:
		switch (ch->tpdu.state) {
		case TPDU_S_WAIT_TX:
		case TPDU_S_WAIT_PB:
			card_emu_uart_enable(ch->uart_chan, ENABLE_TX);
			break;
		default:
			break;
		}
	default:
		break;
	}
}

void card_emu_report_status(struct card_handle *ch)
{
	struct msgb *msg;
	struct cardemu_usb_msg_status *sts;

	msg = usb_buf_alloc_st(ch->in_ep, SIMTRACE_MSGC_CARDEM,
				SIMTRACE_MSGT_BD_CEMU_STATUS);
	if (!msg)
		return;

	sts = (struct cardemu_usb_msg_status *) msgb_put(msg, sizeof(*sts));
	sts->flags = 0;
	if (ch->vcc_active)
		sts->flags |= CEMU_STATUS_F_VCC_PRESENT;
	if (ch->clocked)
		sts->flags |= CEMU_STATUS_F_CLK_ACTIVE;
	if (ch->in_reset)
		sts->flags |= CEMU_STATUS_F_RESET_ACTIVE;
	/* FIXME: voltage + card insert */
	sts->fi = ch->fi;
	sts->di = ch->di;
	sts->wi = ch->wi;
	sts->waiting_time = ch->waiting_time;

	usb_buf_upd_len_and_submit(msg);
}

/* hardware driver informs us that a card I/O signal has changed */
void card_emu_io_statechg(struct card_handle *ch, enum card_io io, int active)
{
	switch (io) {
	case CARD_IO_VCC:
		if (active == 0 && ch->vcc_active == 1) {
			TRACE_INFO("%u: VCC deactivated\r\n", ch->num);
			tc_etu_disable(ch->tc_chan);
			card_set_state(ch, ISO_S_WAIT_POWER);
		} else if (active == 1 && ch->vcc_active == 0) {
			TRACE_INFO("%u: VCC activated\r\n", ch->num);
			card_set_state(ch, ISO_S_WAIT_CLK);
		}
		ch->vcc_active = active;
		break;
	case CARD_IO_CLK:
		if (active == 1 && ch->clocked == 0) {
			TRACE_INFO("%u: CLK activated\r\n", ch->num);
			if (ch->state == ISO_S_WAIT_CLK)
				card_set_state(ch, ISO_S_WAIT_RST);
		} else if (active == 0 && ch->clocked == 1) {
			TRACE_INFO("%u: CLK deactivated\r\n", ch->num);
		}
		ch->clocked = active;
		break;
	case CARD_IO_RST:
		if (active == 0 && ch->in_reset) {
			TRACE_INFO("%u: RST released\r\n", ch->num);
			if (ch->vcc_active && ch->clocked) {
				/* enable the TC/ETU counter once reset has been released */
				tc_etu_enable(ch->tc_chan);
				/* prepare to send the ATR */
				card_set_state(ch, ISO_S_WAIT_ATR);
			}
		} else if (active && !ch->in_reset) {
			TRACE_INFO("%u: RST asserted\r\n", ch->num);
			tc_etu_disable(ch->tc_chan);
		}
		ch->in_reset = active;
		break;
	}
}

/* User sets a new ATR to be returned during next card reset */
int card_emu_set_atr(struct card_handle *ch, const uint8_t *atr, uint8_t len)
{
	if (len > sizeof(ch->atr.atr))
		return -1;

	memcpy(ch->atr.atr, atr, len);
	ch->atr.len = len;
	ch->atr.idx = 0;

#if TRACE_LEVEL >= TRACE_LEVEL_INFO 
	uint8_t i;
	TRACE_INFO("%u: ATR set: ", ch->num);
	for (i = 0; i < ch->atr.len; i++) {
		TRACE_INFO_WP("%02x ", atr[i]);
	}
	TRACE_INFO_WP("\n\r");
#endif
	/* FIXME: race condition with transmitting ATR to reader? */

	return 0;
}

/* hardware driver informs us that one (more) ETU has expired */
void tc_etu_wtime_half_expired(void *handle)
{
	struct card_handle *ch = handle;
	/* transmit NULL procedure byte well before waiting time expires */
	switch (ch->state) {
	case ISO_S_IN_TPDU:
		switch (ch->tpdu.state) {
		case TPDU_S_WAIT_PB:
		case TPDU_S_WAIT_TX:
			putchar('N');
			card_emu_uart_tx(ch->uart_chan, ISO7816_3_PB_NULL);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

/* hardware driver informs us that one (more) ETU has expired */
void tc_etu_wtime_expired(void *handle)
{
	struct card_handle *ch = handle;
	switch (ch->state) {
	case ISO_S_WAIT_ATR:
		/* ISO 7816-3 6.2.1 time tc has passed, we can now send the ATR */
		card_set_state(ch, ISO_S_IN_ATR);
		break;
	default:
		TRACE_ERROR("%u: wtime_exp\r\n", ch->num);
		break;
	}
}

/* shortest ATR found in smartcard_list.txt */
static const uint8_t default_atr[] = { 0x3B, 0x02, 0x14, 0x50 };

static struct card_handle card_handles[NUM_SLOTS];

struct card_handle *card_emu_init(uint8_t slot_num, uint8_t tc_chan, uint8_t uart_chan,
				  uint8_t in_ep, uint8_t irq_ep)
{
	struct card_handle *ch;

	if (slot_num >= ARRAY_SIZE(card_handles))
		return NULL;

	ch = &card_handles[slot_num];

	memset(ch, 0, sizeof(*ch));

	INIT_LLIST_HEAD(&ch->uart_tx_queue);

	/* initialize the card_handle with reasonable defaults */
	ch->num = slot_num;
	ch->irq_ep = irq_ep;
	ch->in_ep = in_ep;
	ch->state = ISO_S_WAIT_POWER;
	ch->vcc_active = 0;
	ch->in_reset = 1;
	ch->clocked = 0;

	ch->fi = 0;
	ch->di = 1;
	ch->wi = ISO7816_3_DEFAULT_WI;

	ch->tc_chan = tc_chan;
	ch->uart_chan = uart_chan;
	ch->waiting_time = ISO7816_3_INIT_WTIME;

	ch->atr.idx = 0;
	ch->atr.len = sizeof(default_atr);
	memcpy(ch->atr.atr, default_atr, ch->atr.len);

	ch->pts.state = PTS_S_WAIT_REQ_PTSS;
	ch->tpdu.state = TPDU_S_WAIT_CLA;

	tc_etu_init(ch->tc_chan, ch);

	return ch;
}
