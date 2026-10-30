/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ser.c (x64)
 *	The serial ports of the PC, as devices "sera", "serb", ... (design
 *	10.5): the 16550 compatible ports at the ISA addresses COM1 to COM4
 *	that answer, COM1 first, which is the console's when the monitor
 *	found it (tm_com.c). The port is reached through I/O ports, its
 *	interrupt through the IOAPIC input the ACPI tables give its ISA
 *	interrupt (COM1 and COM3 on 4, COM2 and COM4 on 3).
 *	What comes in is taken off the FIFO by the interrupt into a ring of
 *	SER_RING bytes, where a read finds it, and a read empties the FIFO
 *	itself as well, for what came without an interrupt. What goes out is
 *	written as the transmitter has room; the console's port is the one
 *	the monitor writes on too, and the two are not kept apart.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ser.h>
#include <ts/ob.h>
#include "../../../kernel/sysdepend/acpi/acpi.h"

#define SER_RING	4096		/* bytes kept of what came in */
#define SER_POLL_MS	10
#define SER_CLOCK	1843200		/* the divisor latch counts this over sixteen */

/* 16550 registers, from the port's base (UART_* of sysdef.h, and these) */
#define UART_IIR	2		/* read: what is pending */
#define UART_DLL	0		/* with LCR_DLAB */
#define UART_DLM	1
#define UART_SCR	7
#define LCR_8N1		0x03
#define LCR_DLAB	0x80
#define FCR_ON		0x07		/* FIFOs on and emptied, trigger at 1 byte */
#define MCR_DTR_RTS	0x03
#define MCR_OUT2	0x08		/* lets the interrupt out to the board */
#define IER_RX		0x01		/* data or its time-out */
#define LSR_TEMT	0x40

#define FLG_RX		0x0001

typedef struct {
	UB		devnm[L_DEVNM + 1];
	UH		port;		/* I/O base */
	UINT		intno;		/* 0: polled */
	T_SERINFO	info;
	UB		ring[SER_RING];
	UW		head, tail;	/* taken at head, put at tail */
	T_SPLOCK	lk;		/* the ring, against the handler on another processor */
	ID		flgid;		/* something came in */
	ID		txmtx;		/* one writer at a time */
	TMO		rcvtmo;
	ID		devid;
	T_OBEDGE	rx_edge;	/* what came in, told (ts/ob.h): armed by a read that found nothing */
	volatile UW	rx_kick;	/* the edge to be kicked once the ring's lock is let go */
} SERPORT;

/*
 * The ports, as many as the machine has: each is allocated when it is
 * found and never freed, and the table of them is made whole before it
 * is put in place (the one before is kept), so that the interrupt
 * handler walks it without a lock.
 */
LOCAL SERPORT	**ser_tbl = NULL;
LOCAL INT	ser_n = 0, ser_cap = 0;

#define SER_AT(i)	( ser_tbl[(i)] )

IMPORT UH	tm_uart_port;
IMPORT BOOL	tm_uart_present;

LOCAL UB rd( SERPORT *p, UW off )
{
	return in_port_b((UH)( p->port + off ));
}

LOCAL void wr( SERPORT *p, UW off, UB v )
{
	out_port_b((UH)( p->port + off ), v);
}

/* ---------------------------------------------------------------- what came in */

/* The FIFO emptied into the ring; the caller holds the ring's lock */
LOCAL INT drain_locked( SERPORT *p )
{
	INT	n = 0;
	UW	next;

	while ( ( rd(p, UART_LSR) & UART_LSR_DR ) != 0 ) {
		UB	c = rd(p, UART_RBR);

		next = ( p->tail + 1 ) % SER_RING;
		if ( next == p->head ) {
			p->info.lost++;		/* full: the newest is dropped */
			continue;
		}
		p->ring[p->tail] = c;
		p->tail = next;
		n++;
	}
	if ( n > 0 && knl_obedge_came(&p->rx_edge) ) {
		p->rx_kick = 1;
	}
	return n;
}

/*
 * The edge of what came in (ts/ob.h, T_OBEDGE), kicked once the ring's
 * lock is let go: the kick may set the flag of the task that tells it
 */
LOCAL void ser_kick( SERPORT *p )
{
	if ( __atomic_exchange_n(&p->rx_kick, 0, __ATOMIC_SEQ_CST) != 0 ) {
		knl_ob_edge_kick(&p->rx_edge);
	}
}

LOCAL INT drain( SERPORT *p )
{
	UINT	s = disint();
	INT	n;

	SpinLock(&p->lk);
	n = drain_locked(p);
	SpinUnlock(&p->lk);
	enaint(s);
	ser_kick(p);
	return n;
}

/*
 * A read found nothing: the edge armed. What is in the ring by now
 * (taken in from the FIFO here) makes the notice due at once.
 */
LOCAL void ser_arm( SERPORT *p )
{
	UINT	s = disint();

	SpinLock(&p->lk);
	(void)drain_locked(p);
	knl_obedge_arm(&p->rx_edge);
	if ( p->head != p->tail && knl_obedge_came(&p->rx_edge) ) {
		p->rx_kick = 1;
	}
	SpinUnlock(&p->lk);
	enaint(s);
	ser_kick(p);
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
		if ( SER_AT(i)->info.console ) {
			(void)drain(SER_AT(i));
			return ( take(SER_AT(i), &c, 1) == 1 ) ? (INT)c : -1;
		}
	}
	return -1;
}

/* Two ports share an ISA interrupt: each that has something is emptied */
LOCAL void ser_inthdr( UINT intno, UW iar )
{
	INT	i, n;

	for ( i = 0; i < ser_n; i++ ) {
		SERPORT	*p = SER_AT(i);

		if ( p->intno != intno ) {
			continue;
		}
		SpinLock(&p->lk);
		n = drain_locked(p);
		SpinUnlock(&p->lk);
		ser_kick(p);
		(void)rd(p, UART_IIR);
		if ( n > 0 ) {
			tk_set_flg(p->flgid, FLG_RX);
		}
	}
	EndOfInt(intno);
}

/*
 * The ports without an interrupt that a reader waits to be told of,
 * looked at by the task that tells the edges: TRUE while there is one
 */
LOCAL BOOL ser_look( void )
{
	BOOL	any = FALSE;
	INT	i;

	for ( i = 0; i < ser_n; i++ ) {
		SERPORT	*p = SER_AT(i);

		if ( p->info.polled && __atomic_load_n(&p->rx_edge.armed, __ATOMIC_SEQ_CST) != 0 ) {
			(void)drain(p);
			any = TRUE;
		}
	}
	return any;
}

/*
 * A read: what has come in, or, when nothing has, what comes first
 * within the receive time-out.
 */
LOCAL INT ser_read( SERPORT *p, UB *buf, INT size )
{
	UINT	ptn;
	INT	n, waited = 0;
	ER	er;

	if ( size <= 0 ) {
		return 0;
	}
	for ( ;; ) {
		if ( !p->info.polled ) {
			(void)tk_clr_flg(p->flgid, ~(UINT)FLG_RX);
		}
		(void)drain(p);
		n = take(p, buf, size);
		if ( n > 0 || p->rcvtmo == TMO_POL ) {
			if ( n == 0 ) {
				ser_arm(p);	/* told (OB_E_CHANGE of record 1) when something comes */
			}
			return n;
		}
		if ( p->info.polled ) {
			if ( p->rcvtmo != TMO_FEVR && waited >= p->rcvtmo ) {
				return 0;
			}
			/* a wait cut by someone else (tk_rel_wai) ends the read with what there is */
			if ( tk_dly_tsk(SER_POLL_MS) == E_RLWAI ) {
				return 0;
			}
			waited += SER_POLL_MS;
		} else if ( ( er = tk_wai_flg(p->flgid, FLG_RX, TWF_ORW | TWF_BITCLR, &ptn,
				       ( p->rcvtmo == TMO_FEVR || p->rcvtmo > SER_POLL_MS * 10 )
				       ? SER_POLL_MS * 10 : p->rcvtmo) ) < E_OK ) {
			if ( er == E_RLWAI ) {
				return 0;
			}
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
		while ( ( rd(p, UART_LSR) & UART_LSR_THRE ) == 0 ) {
			CpuRelax();		/* the holding register is full */
		}
		wr(p, UART_THR, buf[i]);
	}
	tk_unl_mtx(p->txmtx);
}

/* The speed set through the divisor latch. The console's port stays as the monitor set it up */
LOCAL ER ser_speed( SERPORT *p, UW speed )
{
	UW	div;

	if ( speed < 300 || speed > 115200 ) {
		return E_PAR;
	}
	if ( p->info.console ) {
		return ( speed == p->info.speed ) ? E_OK : E_NOSPT;
	}
	div = SER_CLOCK / 16 / speed;
	if ( div == 0 || div > 0xffff ) {
		return E_PAR;
	}
	tk_loc_mtx(p->txmtx, TMO_FEVR);
	while ( ( rd(p, UART_LSR) & LSR_TEMT ) == 0 ) {
		CpuRelax();
	}
	wr(p, UART_LCR, LCR_DLAB | LCR_8N1);
	wr(p, UART_DLL, (UB)div);
	wr(p, UART_DLM, (UB)( div >> 8 ));
	wr(p, UART_LCR, LCR_8N1);
	tk_unl_mtx(p->txmtx);
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

/* Whether a 16550 answers at the port: its scratch register keeps what is written */
LOCAL BOOL port_answers( UH port )
{
	out_port_b((UH)( port + UART_SCR ), 0x5a);
	if ( in_port_b((UH)( port + UART_SCR )) != 0x5a ) return FALSE;
	out_port_b((UH)( port + UART_SCR ), 0xa5);
	return in_port_b((UH)( port + UART_SCR )) == 0xa5;
}

LOCAL void put_label( SERPORT *p, CONST char *what )
{
	INT	n = 0;

	while ( what[n] != 0 && n < SER_LABEL_MAX - 1 ) {
		p->info.label[n] = (UB)what[n];
		n++;
	}
	p->info.label[n] = 0;
}

/* The name of the port at place n: "sera" to "serz", then "seraa" on */
LOCAL void ser_name( INT n, UB *out )
{
	UB	t[L_DEVNM];
	INT	k = 0, i;

	n++;
	while ( n > 0 && k < L_DEVNM - 3 ) {
		n--;
		t[k++] = (UB)( 'a' + n % 26 );
		n /= 26;
	}
	out[0] = 's';
	out[1] = 'e';
	out[2] = 'r';
	for ( i = 0; i < k; i++ ) {
		out[3 + i] = t[k - 1 - i];
	}
	out[3 + k] = 0;
}

/* One port set up and registered as the next name */
LOCAL ER port_add( UH port, UB irq, CONST char *what )
{
	SERPORT	*p;
	T_DDEV	ddev;
	T_CMTX	cmtx;
	T_CFLG	cflg;
	UH	flags = 0;
	UW	gsi;

	if ( ser_n == ser_cap ) {
		INT	cap = ( ser_cap > 0 ) ? ser_cap * 2 : 4, i;
		SERPORT	**t = (SERPORT **)Kcalloc((SZ)cap, sizeof(SERPORT *));

		if ( t == NULL ) {
			return E_NOMEM;
		}
		for ( i = 0; i < ser_n; i++ ) {
			t[i] = ser_tbl[i];
		}
		__sync_synchronize();
		ser_tbl = t;			/* the table before is not freed */
		ser_cap = cap;
	}
	if ( ser_tbl[ser_n] == NULL ) {
		ser_tbl[ser_n] = (SERPORT *)Kcalloc(1, sizeof(SERPORT));
		if ( ser_tbl[ser_n] == NULL ) {
			return E_NOMEM;
		}
	}
	p = ser_tbl[ser_n];
	knl_memset(p, 0, sizeof(*p));
	p->port = port;
	gsi = knl_acpi_isa_gsi(irq, &flags);
	p->intno = ( gsi < INTVEC_GSI_NUM ) ? INTVEC_GSI(gsi) : 0;
	p->rcvtmo = TMO_POL;
	p->info.console = (BOOL)( tm_uart_present && port == tm_uart_port );
	p->info.polled = (BOOL)( p->intno == 0 );
	p->info.speed = 115200;
	put_label(p, what);
	ser_name(ser_n, p->devnm);

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	p->txmtx = tk_cre_mtx(&cmtx);
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	p->flgid = tk_cre_flg(&cflg);
	(void)knl_ob_edge_add(&p->rx_edge, NULL, p->devnm, 1, p->info.polled ? ser_look : NULL);
	if ( p->txmtx <= 0 || p->flgid <= 0 ) {
		return E_LIMIT;
	}

	/* 8 bits, no parity, one stop bit, the FIFOs on; the console's speed stays as it is */
	wr(p, UART_IER, 0);
	if ( !p->info.console ) {
		wr(p, UART_LCR, LCR_DLAB | LCR_8N1);
		wr(p, UART_DLL, (UB)( SER_CLOCK / 16 / 115200 ));
		wr(p, UART_DLM, 0);
	}
	wr(p, UART_LCR, LCR_8N1);
	wr(p, UART_FCR, FCR_ON);
	wr(p, UART_MCR, MCR_DTR_RTS | MCR_OUT2);
	(void)rd(p, UART_LSR);
	(void)rd(p, UART_RBR);
	(void)rd(p, UART_IIR);
	ser_n++;			/* counted before the interrupt can come */
	if ( !p->info.polled ) {
		knl_define_inthdr((INT)p->intno, TA_HLNG, (FP)ser_inthdr);
		EnableInt(p->intno, INTPRI_DEVICE);
		wr(p, UART_IER, IER_RX);
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
		wr(p, UART_IER, 0);
		ser_n--;
		return (ER)p->devid;
	}
	tm_printf((UB *)"TessronOS: %s %s, interrupt %d%s\n", p->devnm, p->info.label,
		  (INT)p->intno, p->info.console ? " (console)" : "");
	if ( p->info.console ) {
		tm_getc_hook = console_getc;	/* the monitor reads through us now */
	}
	return E_OK;
}

/* Every port of the machine that answers, COM1 first so that the console's is "sera" */
EXPORT INT knl_ser_init( void )
{
	LOCAL CONST struct {
		UH	port;
		UB	irq;
		CONST char *name;
	} com[4] = {
		{ 0x3f8, 4, "COM1 0x3f8" },
		{ 0x2f8, 3, "COM2 0x2f8" },
		{ 0x3e8, 4, "COM3 0x3e8" },
		{ 0x2e8, 3, "COM4 0x2e8" },
	};
	INT	i;

	for ( i = 0; i < 4; i++ ) {
		if ( port_answers(com[i].port) ) {
			(void)port_add(com[i].port, com[i].irq, com[i].name);
		}
	}
	return ser_n;
}
