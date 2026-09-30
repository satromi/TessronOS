/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ser.c
 *	The serial ports, as devices "sera", "serb", ... (design 10.5)
 *
 *	Every port here is a PL011. Which there are comes from the device
 *	tree on QEMU virt (each /pl011@ node, the console's first) and from
 *	the board's own table on the Raspberry Pi 5: the three pin debug
 *	UART and UART0 of the RP1 on the 40 pin header.
 *
 *	What comes in is taken off the port's FIFO by its interrupt into a
 *	ring of SER_RING bytes, where a read finds it. A port without an
 *	interrupt the driver can use -- the RP1's, whose interrupts come
 *	through the RP1's own controller -- is read when a read asks, and a
 *	read that waits looks again every SER_POLL_MS. What goes out is
 *	written into the FIFO as there is room; the console's port is the
 *	one the monitor writes on too, and the two are not kept apart.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ser.h>

#define SER_RING	4096		/* bytes kept of what came in */
#define SER_POLL_MS	10

/* PL011 */
#define PL_DR		0x00
#define PL_FR		0x18
#define PL_IBRD		0x24
#define PL_FBRD		0x28
#define PL_LCRH		0x2c
#define PL_CR		0x30
#define PL_IFLS		0x34
#define PL_IMSC		0x38
#define PL_ICR		0x44
#define PL_PERIPHID0	0xFE0

#define FR_BUSY		0x0008
#define FR_RXFE		0x0010
#define FR_TXFF		0x0020
#define LCRH_FEN	0x0010
#define LCRH_WLEN_8	0x0060
#define CR_UARTEN	0x0001
#define CR_TXE		0x0100
#define CR_RXE		0x0200
#define IM_RX		0x0010		/* the FIFO passed its mark */
#define IM_RT		0x0040		/* bytes have waited in it */

#define FLG_RX		0x0001

typedef struct {
	UB		devnm[L_DEVNM + 1];
	UBINT		base;		/* registers */
	UINT		intno;		/* 0: polled */
	UW		clock;		/* what the divisor is taken from, 0: unknown */
	T_SERINFO	info;
	UB		ring[SER_RING];
	UW		head, tail;	/* taken at head, put at tail */
	T_SPLOCK	lk;		/* the ring, against the handler on another processor */
	ID		flgid;		/* something came in */
	ID		txmtx;		/* one writer at a time */
	TMO		rcvtmo;
	ID		devid;
} SERPORT;

LOCAL SERPORT	ser_port[SER_MAX_PORT];
LOCAL INT	ser_n = 0;

IMPORT UBINT	tm_uart_base;

LOCAL UW rd( SERPORT *p, UW off )
{
	return in_w(p->base + off);
}

LOCAL void wr( SERPORT *p, UW off, UW v )
{
	out_w(p->base + off, v);
}

/* ---------------------------------------------------------------- what came in */

/* The FIFO emptied into the ring; the caller holds the ring's lock */
LOCAL INT drain_locked( SERPORT *p )
{
	INT	n = 0;
	UW	next;

	while ( ( rd(p, PL_FR) & FR_RXFE ) == 0 ) {
		UB	c = (UB)rd(p, PL_DR);

		next = ( p->tail + 1 ) % SER_RING;
		if ( next == p->head ) {
			p->info.lost++;		/* full: the newest is dropped */
			continue;
		}
		p->ring[p->tail] = c;
		p->tail = next;
		n++;
	}
	return n;
}

LOCAL INT drain( SERPORT *p )
{
	UINT	s = disint();
	INT	n;

	SpinLock(&p->lk);
	n = drain_locked(p);
	SpinUnlock(&p->lk);
	enaint(s);
	return n;
}

/* What the ring holds, up to `max` bytes, taken */
LOCAL INT take( SERPORT *p, UB *buf, INT max )
{
	UINT	s = disint();
	INT	n = 0;

	SpinLock(&p->lk);
	while ( n < max && p->head != p->tail ) {
		buf[n++] = p->ring[p->head];
		p->head = ( p->head + 1 ) % SER_RING;
	}
	SpinUnlock(&p->lk);
	enaint(s);
	return n;
}

/*
 * The monitor's input, when the console's port is ours: what the ring
 * holds, the FIFO taken in first so that it works with interrupts off.
 */
IMPORT INT	(*tm_getc_hook)( void );

LOCAL INT console_getc( void )
{
	INT	i;
	UB	c;

	for ( i = 0; i < ser_n; i++ ) {
		if ( ser_port[i].info.console ) {
			(void)drain(&ser_port[i]);
			return ( take(&ser_port[i], &c, 1) == 1 ) ? (INT)c : -1;
		}
	}
	return -1;
}

LOCAL void ser_inthdr( UINT intno, UW iar )
{
	INT	i, n;

	for ( i = 0; i < ser_n; i++ ) {
		SERPORT	*p = &ser_port[i];

		if ( p->intno != intno ) {
			continue;
		}
		SpinLock(&p->lk);
		n = drain_locked(p);
		SpinUnlock(&p->lk);
		wr(p, PL_ICR, IM_RX | IM_RT);
		if ( n > 0 ) {
			tk_set_flg(p->flgid, FLG_RX);
		}
	}
	EndOfInt(intno);
}

/*
 * A read: what has come in, or, when nothing has, what comes first
 * within the receive time-out.
 */
LOCAL INT ser_read( SERPORT *p, UB *buf, INT size )
{
	UINT	ptn;
	INT	n, waited = 0;

	if ( size <= 0 ) {
		return 0;
	}
	for ( ;; ) {
		if ( !p->info.polled ) {
			(void)tk_clr_flg(p->flgid, ~(UINT)FLG_RX);
		}
		/*
		 * The FIFO is emptied here as well as by the interrupt: bytes
		 * that came without one (fewer than the FIFO's trigger level,
		 * and no receive time-out raised for them) are not left there.
		 */
		(void)drain(p);
		n = take(p, buf, size);
		if ( n > 0 || p->rcvtmo == TMO_POL ) {
			return n;
		}
		if ( p->info.polled ) {
			if ( p->rcvtmo != TMO_FEVR && waited >= p->rcvtmo ) {
				return 0;
			}
			tk_dly_tsk(SER_POLL_MS);
			waited += SER_POLL_MS;
		} else if ( tk_wai_flg(p->flgid, FLG_RX, TWF_ORW | TWF_BITCLR, &ptn,
				       ( p->rcvtmo == TMO_FEVR || p->rcvtmo > SER_POLL_MS * 10 )
				       ? SER_POLL_MS * 10 : p->rcvtmo) < E_OK ) {
			/* nothing said so within the while: look at the FIFO itself */
			(void)drain(p);
			n = take(p, buf, size);
			if ( n > 0 || p->rcvtmo != TMO_FEVR ) {
				waited += SER_POLL_MS * 10;
				if ( n > 0 || waited >= p->rcvtmo ) return n;
			}
		}
	}
}

/* ---------------------------------------------------------------- what goes out */

LOCAL void ser_write( SERPORT *p, CONST UB *buf, INT size )
{
	INT	i;

	tk_loc_mtx(p->txmtx, TMO_FEVR);
	for ( i = 0; i < size; i++ ) {
		while ( ( rd(p, PL_FR) & FR_TXFF ) != 0 ) {
			;			/* the FIFO is full */
		}
		wr(p, PL_DR, buf[i]);
	}
	tk_unl_mtx(p->txmtx);
}

/*
 * The speed set: the divisor is the clock over sixteen times the speed,
 * kept in six fractional bits, which clk * 4 / speed gives at once. The
 * console's port stays as the monitor set it up.
 */
LOCAL ER ser_speed( SERPORT *p, UW speed )
{
	UW	div;

	if ( speed < 300 || speed > 4000000 ) {
		return E_PAR;
	}
	if ( p->info.console ) {
		return ( speed == p->info.speed ) ? E_OK : E_NOSPT;
	}
	if ( p->clock != 0 ) {
		tk_loc_mtx(p->txmtx, TMO_FEVR);
		while ( ( rd(p, PL_FR) & FR_BUSY ) != 0 ) {
			;
		}
		wr(p, PL_CR, 0);
		div = (UW)( ( (UD)p->clock * 4 ) / speed );
		wr(p, PL_IBRD, div / 64);
		wr(p, PL_FBRD, div % 64);
		wr(p, PL_LCRH, LCRH_WLEN_8 | LCRH_FEN);	/* takes the divisor in */
		wr(p, PL_CR, CR_UARTEN | CR_TXE | CR_RXE);
		tk_unl_mtx(p->txmtx);
	}
	p->info.speed = speed;
	return E_OK;
}

/* ---------------------------------------------------------------- the driver */

LOCAL ER ser_open( ID devid, UINT omode, void *exinf )
{
	return E_OK;
}

LOCAL ER ser_close( ID devid, UINT option, void *exinf )
{
	return E_OK;
}

LOCAL ER ser_attr( SERPORT *p, T_DEVREQ *req )
{
	BOOL	rd_req = (BOOL)( req->cmd == TDC_READ );

	switch ( req->start ) {
	case TDN_SER_SPEED:
		if ( req->size < (SZ)sizeof(UW) ) return E_PAR;
		req->asize = sizeof(UW);
		if ( rd_req ) {
			*(UW *)req->buf = p->info.speed;
			return E_OK;
		}
		return ser_speed(p, *(UW *)req->buf);
	case TDN_SER_RCVTMO:
		if ( req->size < (SZ)sizeof(TMO) ) return E_PAR;
		req->asize = sizeof(TMO);
		if ( rd_req ) {
			*(TMO *)req->buf = p->rcvtmo;
		} else {
			p->rcvtmo = *(TMO *)req->buf;
		}
		return E_OK;
	case TDN_SER_INFO:
		if ( !rd_req || req->size < (SZ)sizeof(T_SERINFO) ) return E_PAR;
		*(T_SERINFO *)req->buf = p->info;
		req->asize = sizeof(T_SERINFO);
		return E_OK;
	default:
		return E_PAR;
	}
}

LOCAL ER ser_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	SERPORT	*p = (SERPORT *)exinf;

	req->asize = 0;
	req->error = E_OK;
	if ( req->start < 0 ) {
		req->error = ser_attr(p, req);
	} else if ( req->cmd == TDC_READ ) {
		req->asize = ser_read(p, (UB *)req->buf, (INT)req->size);
	} else {
		ser_write(p, (CONST UB *)req->buf, (INT)req->size);
		req->asize = req->size;
	}
	return E_OK;
}

LOCAL INT ser_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER ser_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

LOCAL INT ser_event( INT evttyp, void *evtinf, void *exinf )
{
	return E_NOSPT;
}

/* ---------------------------------------------------------------- start-up */

LOCAL void put_label( SERPORT *p, CONST char *what, UD pa )
{
	CONST char	*hex = "0123456789abcdef";
	INT		n = 0, k;

	while ( what[n] != 0 && n < SER_LABEL_MAX - 14 ) {
		p->info.label[n] = (UB)what[n];
		n++;
	}
	p->info.label[n++] = ' ';
	p->info.label[n++] = '0';
	p->info.label[n++] = 'x';
	for ( k = 36; k >= 0; k -= 4 ) {
		if ( ( pa >> k ) != 0 || k == 0 ) {
			p->info.label[n++] = (UB)hex[( pa >> k ) & 0xF];
		}
	}
	p->info.label[n] = 0;
}

/* One port set up and registered as the next name */
LOCAL ER port_add( UD pa, UINT intno, UW clock, CONST char *what, BOOL polled )
{
	SERPORT	*p;
	T_DDEV	ddev;
	T_CMTX	cmtx;
	T_CFLG	cflg;

	if ( ser_n >= SER_MAX_PORT ) {
		return E_LIMIT;
	}
	p = &ser_port[ser_n];
	knl_memset(p, 0, sizeof(*p));
	p->base = DEV_BASE(pa);
	p->intno = polled ? 0 : intno;
	p->clock = clock;
	p->rcvtmo = TMO_POL;
	p->info.console = (BOOL)( p->base == tm_uart_base );
	p->info.polled = (BOOL)( p->intno == 0 );
	p->info.speed = 115200;
	put_label(p, what, pa);
	p->devnm[0] = 's';  p->devnm[1] = 'e';  p->devnm[2] = 'r';
	p->devnm[3] = (UB)( 'a' + ser_n );
	p->devnm[4] = 0;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	p->txmtx = tk_cre_mtx(&cmtx);
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	p->flgid = tk_cre_flg(&cflg);
	if ( p->txmtx <= 0 || p->flgid <= 0 ) {
		return E_LIMIT;
	}

	/* the FIFOs on, the port on; what is already set up stays */
	if ( ( rd(p, PL_CR) & CR_UARTEN ) == 0 ) {
		wr(p, PL_LCRH, LCRH_WLEN_8 | LCRH_FEN);
		wr(p, PL_CR, CR_UARTEN | CR_TXE | CR_RXE);
	}
	wr(p, PL_IMSC, 0);
	wr(p, PL_ICR, 0x7FF);
	ser_n++;			/* counted before the interrupt can come */
	if ( !p->info.polled ) {
		wr(p, PL_IFLS, 0);		/* a mark of an eighth */
		knl_define_inthdr((INT)p->intno, TA_HLNG, (FP)ser_inthdr);
		EnableInt(p->intno, INTPRI_DEVICE);
		wr(p, PL_IMSC, IM_RX | IM_RT);
	}

	ddev.exinf   = p;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_SERIAL;
	ddev.nsub    = 0;
	ddev.blksz   = 1;
	ddev.openfn  = (FP)ser_open;
	ddev.closefn = (FP)ser_close;
	ddev.execfn  = (FP)ser_exec;
	ddev.waitfn  = (FP)ser_wait;
	ddev.abortfn = (FP)ser_abort;
	ddev.eventfn = (FP)ser_event;
	p->devid = tk_def_dev(p->devnm, &ddev, NULL);
	if ( p->devid <= 0 ) {
		if ( !p->info.polled ) {
			wr(p, PL_IMSC, 0);
			DisableInt(p->intno);
		}
		ser_n--;
		return (ER)p->devid;
	}
	tm_printf((UB *)"TessronOS: %s %s%s\n", p->devnm, p->info.label,
		  p->info.console ? " (console)" : "");
	if ( p->info.console ) {
		tm_getc_hook = console_getc;	/* the monitor reads through us now */
	}
	return E_OK;
}

#ifdef QEMU_VIRT
IMPORT UD	knl_pl011_pa[];
IMPORT UW	knl_pl011_int[];
IMPORT INT	knl_pl011_n;

#define QEMU_PL011_CLOCK	24000000	/* apb_pclk of the machine's UARTs */
#endif

#ifdef RPI5
IMPORT ER rp1_uart_init( UW baud );

#define RP1_UART_CLOCK		50000000
#endif

/*
 * Every port of the machine, the console's first so that it is "sera".
 */
EXPORT INT knl_ser_init( void )
{
#ifdef QEMU_VIRT
	INT	i;

	if ( knl_pl011_n == 0 ) {
		(void)port_add(PL011_UART0_PA, PL011_UART0_INTNO, QEMU_PL011_CLOCK,
			       "PL011", FALSE);
	}
	for ( i = 0; i < knl_pl011_n; i++ ) {
		if ( knl_pl011_pa[i] == DBG_UART_PA ) {
			(void)port_add(knl_pl011_pa[i], knl_pl011_int[i], QEMU_PL011_CLOCK,
				       "PL011", FALSE);
		}
	}
	for ( i = 0; i < knl_pl011_n; i++ ) {
		if ( knl_pl011_pa[i] != DBG_UART_PA && knl_pl011_pa[i] != 0 ) {
			(void)port_add(knl_pl011_pa[i], knl_pl011_int[i], QEMU_PL011_CLOCK,
				       "PL011", ( knl_pl011_int[i] == 0 ));
		}
	}
#endif
#ifdef RPI5
	BOOL	rp1_console = (BOOL)( tm_uart_base == RP1_UART0_BASE );

	if ( rp1_console ) {
		(void)port_add(RP1_UART0_PA, 0, RP1_UART_CLOCK, "RP1 UART0", TRUE);
	}
	(void)port_add(PL011_UART10_PA, PL011_UART10_INTNO, PL011_UART10_CLOCK,
		       "debug UART", FALSE);
	if ( !rp1_console && CNF_RP1_UART0 ) {
		ER	er;

#if CNF_BOOT_TRACE
		tm_printf((UB *)"TessronOS: > rp1 uart0\n");
#endif
		er = rp1_uart_init(115200);
		if ( er >= E_OK ) {
			(void)port_add(RP1_UART0_PA, 0, RP1_UART_CLOCK, "RP1 UART0", TRUE);
		} else {
			tm_printf((UB *)"TessronOS: RP1 UART0 not set up (%d)\n", (INT)er);
		}
	}
#endif
	return ser_n;
}
