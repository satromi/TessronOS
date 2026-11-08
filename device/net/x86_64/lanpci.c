/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lanpci.c (x64 PC)
 *	The part the PCIe LAN drivers (re.c, rge.c, igc.c) share, in the shape
 *	of e1k.c: one ring of 64 receive buffers and one of 32 transmit
 *	buffers of 2KB, a frame copied in and out, a received frame raising
 *	the card's interrupt when it has one the machine can take, else looked
 *	for every millisecond while someone waits.
 *
 *	Each card has its own memory, locks and counters here, so several may
 *	be in one machine. The console says the link's state at the start and
 *	at each change in one form, "net: <chip> <vendor>:<device> link ...",
 *	which is what is asked for from a machine these drivers have not been
 *	tried on.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "sysman/pfalloc.h"
#include "nicring.h"
#include "lanpci.h"

LOCAL void *pages( UINT order, UD *p_pa )
{
	PFRAME	*pf = knl_alloc_pages(order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);

	if ( pf == NULL ) {
		return NULL;
	}
	*p_pa = knl_pf_to_pa(pf);
	return PA2VA(*p_pa);
}

EXPORT void knl_lan_inthdr( T_LAN *l, UINT n )
{
	BOOL	link = l->ops->irq_ack(l);

	l->n_irq++;
	tk_set_flg(l->flg, 1);
	if ( link ) knl_net_link_said();
	EndOfInt(n);
}

EXPORT void knl_lan_say( T_LAN *l )
{
	INT	mbps = 0;
	BOOL	fdx = FALSE, up;

	up = l->ops->link(l, &mbps, &fdx);
	tm_printf((UB *)"net: %s %04x:%04x link %s %d %s, rev %08x, %02x:%02x:%02x:%02x:%02x:%02x, %s %d, irqs %u\n",
		  l->chip, l->pci.vendor, l->pci.device, up ? "up" : "down", mbps, up ? ( fdx ? "full" : "half" ) : "-",
		  l->rev, l->mac[0], l->mac[1], l->mac[2], l->mac[3], l->mac[4], l->mac[5],
		  ts_pcie_irq_name(l->irq_kind), l->intno, l->n_irq);
	l->link_was = up;
	l->mbps_was = mbps;
}

EXPORT void knl_lan_refuse( CONST char *chip, CONST T_PCIDEV *pci, UW rev, CONST char *why )
{
	tm_printf((UB *)"net: %s %04x:%04x link none: rev %08x %s\n", chip, pci->vendor, pci->device, rev, why);
}

/* The link looked at: said on the console, and by the caller told when it changed */
LOCAL void link_look( T_LAN *l )
{
	INT	mbps = 0;
	BOOL	fdx = FALSE, up = l->ops->link(l, &mbps, &fdx);

	if ( up != l->link_was || ( up && mbps != l->mbps_was ) ) knl_lan_say(l);
}

/* ---------------------------------------------------------------- what netsel.c calls */

LOCAL ER lan_get_mac( void *ctx, UB *m )
{
	T_LAN	*l = ctx;

	if ( !l->ok ) return E_NOEXS;
	knl_memcpy(m, l->mac, NET_MAC_LEN);
	return E_OK;
}

LOCAL ER lan_send( void *ctx, CONST void *frame, SZ len )
{
	T_LAN	*l = ctx;
	UB	*b;
	UINT	i;
	INT	k, n = (INT)len;

	if ( !l->ok ) return E_NOEXS;
	if ( frame == NULL || len < NIC_FRAME_MIN || len > NET_FRAME_MAX ) return E_PAR;
	tk_loc_mtx(l->tx_mtx, TMO_FEVR);
	i = l->tx_next;
	for ( k = 0; k < 1000 && !l->ops->tx_free(l, i); k++ ) WaitUsec(10);
	if ( !l->ops->tx_free(l, i) ) {
		/* the chip has not sent what the slot holds (no link, or stopped): this frame is lost, the slot is not taken */
		l->n_drop++;
		tk_unl_mtx(l->tx_mtx);
		return E_BUSY;
	}
	b = l->txbuf + i * LAN_BUF;
	knl_memcpy(b, frame, n);
	if ( n < NIC_FRAME_PAD ) {
		knl_memset(b + n, 0, NIC_FRAME_PAD - n);
		n = NIC_FRAME_PAD;
	}
	l->tx_next = ( i + 1 ) % LAN_NTX;
	l->ops->tx_go(l, i, (UINT)n);
	l->n_sent++;
	tk_unl_mtx(l->tx_mtx);
	return E_OK;
}

LOCAL INT lan_recv( void *ctx, void *frame, SZ size, TMO tmout )
{
	T_LAN	*l = ctx;
	UINT	ptn;
	INT	waited = 0, len = 0;

	if ( !l->ok ) return E_NOEXS;
	if ( tk_loc_mtx(l->rx_mtx, tmout) < E_OK ) return E_BUSY;
	for ( ;; ) {
		UINT	i = l->rx_next;
		INT	n;

		if ( l->asleep ) {
			if ( tmout == TMO_POL ) {
				len = E_TMOUT;
				break;
			}
			tk_dly_tsk(20);
			continue;
		}
		n = l->ops->rx_take(l, i);
		if ( n != NIC_RX_NONE ) {
			if ( n > 0 && n <= (INT)size ) {
				knl_memcpy(frame, l->rxbuf + i * LAN_BUF, n);
				l->n_recv++;
				len = n;
			} else {
				l->n_drop++;
				len = 0;
			}
			l->rx_next = ( i + 1 ) % LAN_NRX;
			l->ops->rx_give(l, i);
			if ( len > 0 ) break;
			continue;
		}
		if ( tmout == TMO_POL || ( tmout != TMO_FEVR && waited >= tmout ) ) {
			len = E_TMOUT;
			break;
		}
		if ( l->intno != 0 ) {
			(void)tk_wai_flg(l->flg, 1, TWF_ORW | TWF_BITCLR, &ptn, 10);
			waited += 10;
		} else {
			tk_dly_tsk(1);
			waited += 1;
		}
	}
	tk_unl_mtx(l->rx_mtx);
	return len;
}

LOCAL ER lan_stat( void *ctx, T_NETSTAT *st )
{
	T_LAN	*l = ctx;
	INT	mbps;
	BOOL	fdx;

	knl_memset(st, 0, sizeof(*st));
	if ( !l->ok ) return E_NOEXS;
	st->sent = l->n_sent;
	st->recv = l->n_recv;
	st->drop = l->n_drop;
	st->up = FALSE;
	if ( !l->asleep ) {
		st->up = l->ops->link(l, &mbps, &fdx);
		link_look(l);
	}
	return E_OK;
}

/* Nothing tells of the link of a card without its interrupt: the adapters' manager looks every second */
LOCAL BOOL lan_told( void *ctx )
{
	return ((T_LAN *)ctx)->intno != 0;
}

LOCAL INT lan_speed( void *ctx )
{
	T_LAN	*l = ctx;
	INT	mbps = 0;
	BOOL	fdx;

	if ( !l->ok || l->asleep || !l->ops->link(l, &mbps, &fdx) ) return 0;
	return mbps;
}

LOCAL INT lan_label( void *ctx, UB *t, INT max )
{
	T_LAN	*l = ctx;
	static CONST char hex[] = "0123456789abcdef";
	INT	n = 0, k;
	CONST char *s;
	UH	v[2];

	v[0] = l->pci.vendor;
	v[1] = l->pci.device;
	for ( s = l->chip; *s != '\0' && n < max - 1; s++ ) t[n++] = (UB)*s;
	if ( n + 11 >= max ) return n;
	t[n++] = ' ';
	for ( k = 0; k < 2; k++ ) {
		t[n++] = (UB)hex[( v[k] >> 12 ) & 15];
		t[n++] = (UB)hex[( v[k] >> 8 ) & 15];
		t[n++] = (UB)hex[( v[k] >> 4 ) & 15];
		t[n++] = (UB)hex[v[k] & 15];
		if ( k == 0 ) t[n++] = ':';
	}
	return n;
}

/* ---------------------------------------------------------------- the machine's sleep */

LOCAL ER lan_pm_suspend( void *arg, INT how )
{
	T_LAN	*l = arg;

	(void)how;
	if ( !l->ok ) return E_OK;
	tk_loc_mtx(l->tx_mtx, TMO_FEVR);
	l->asleep = TRUE;
	tk_dly_tsk(30);
	if ( l->intno != 0 ) l->ops->irq_off(l);
	l->ops->hw_stop(l);
	return E_OK;
}

LOCAL void lan_pm_resume( void *arg, INT how )
{
	T_LAN	*l = arg;

	(void)how;
	if ( !l->ok ) return;
	l->rx_next = 0;
	l->tx_next = 0;
	l->ops->hw_start(l);
	if ( l->intno != 0 ) l->ops->irq_on(l);
	l->asleep = FALSE;
	tk_unl_mtx(l->tx_mtx);
	knl_lan_say(l);
}

/* ---------------------------------------------------------------- start-up */

EXPORT INT knl_lan_start( T_LAN *l, INT c )
{
	T_CFLG	cf;
	T_CMTX	cm;

	l->rx = pages(0, &l->rx_pa);
	l->tx = pages(0, &l->tx_pa);
	l->rxbuf = (UB *)pages(5, &l->rxbuf_pa);	/* 64 buffers of 2KB */
	l->txbuf = (UB *)pages(4, &l->txbuf_pa);	/* 32 */
	if ( l->rx == NULL || l->tx == NULL || l->rxbuf == NULL || l->txbuf == NULL ) {
		return E_NOMEM;
	}
	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	l->flg = tk_cre_flg(&cf);
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	l->tx_mtx = tk_cre_mtx(&cm);
	l->rx_mtx = tk_cre_mtx(&cm);
	if ( l->flg <= 0 || l->tx_mtx <= 0 || l->rx_mtx <= 0 ) {
		return E_LIMIT;
	}
	l->ops->hw_start(l);
	if ( l->intno != 0 ) l->ops->irq_on(l);

	l->pm.name = l->card;
	l->pm.order = PM_ORD_NET;
	l->pm.suspend = lan_pm_suspend;
	l->pm.resume = lan_pm_resume;
	l->pm.arg = l;
	(void)ts_pm_register(&l->pm);

	l->drv.get_mac = lan_get_mac;
	l->drv.send = lan_send;
	l->drv.recv = lan_recv;
	l->drv.stat = lan_stat;
	l->drv.told = lan_told;
	l->drv.speed = lan_speed;
	l->drv.label = lan_label;
	l->drv.ctx = l;
	l->ok = TRUE;
	knl_netsel_add(c, &l->drv);
	knl_lan_say(l);
	return 1;
}

#endif /* X64 */
