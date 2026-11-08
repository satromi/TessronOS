/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	disp_ext.c (x64 PC)
 *	External screens on Intel's integrated graphics of the 9th generation:
 *	what is plugged into DDI B, C and D, found by the PCH's hot plug
 *	detection, and its EDID read, through DP AUX (DisplayPort, Type-C) or
 *	GMBUS (HDMI). Nothing is shown on them yet: pipe A and the panel are
 *	not touched, and the records say which port each connector is on, which
 *	the mode setting has to go by (docs/tessronos/06-device.md 6.9.1).
 *
 *	Only with the start-up setting ext=1. The DDI's IO power wells are
 *	asked for, since the AUX channels and GMBUS need them; the hot plug
 *	enables are set; everything else is read.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tm/tmonitor.h>
#include <tk/tkernel.h>
#include "sysdepend.h"

IMPORT UBINT	knl_igfx_mmio( void );			/* disp_igfx.c */

#define SDEISR			0xc4000
#define SDE_HPD(port)		( 1U << ( 20 + (port) ) )	/* port 1 B, 2 C, 3 D */
#define PCH_PORT_HOTPLUG	0xc4030
#define HP_ENABLE(port)		( 1U << ( 4 + ( (port) - 1 ) * 8 ) )

#define PWR_WELL_CTL2		0x45404
#define PW_REQ(i)		( 1U << ( 2 * (i) + 1 ) )
#define PW_STATE(i)		( 1U << ( 2 * (i) ) )
#define PW_MISC_IO		0
#define PW_DDI_B		2

#define AUX_CTL(port)		( 0x64010 + 0x100 * (port) )
#define AUX_DATA(port, i)	( AUX_CTL(port) + 4 + 4 * (i) )
#define AUX_BUSY		( 1U << 31 )
#define AUX_DONE		( 1U << 30 )
#define AUX_TIMEOUT_ERR		( 1U << 28 )
#define AUX_TIME_1600US		( 3U << 26 )
#define AUX_RECV_ERR		( 1U << 25 )
#define AUX_SIZE(n)		( (UW)(n) << 20 )
#define AUX_SYNC_SKL		( ( 32U - 1 ) << 5 | ( 32U - 1 ) )

#define AUX_I2C_WRITE		0x0
#define AUX_I2C_READ		0x1
#define AUX_I2C_MOT		0x4
#define AUX_NATIVE_READ		0x9
#define AUX_REPLY_ACK		0x00

#define GMBUS0			0xc5100
#define GMBUS1			0xc5104
#define GMBUS2			0xc5108
#define GMBUS3			0xc510c
#define GM_SW_RDY		( 1U << 30 )
#define GM_CYCLE_WAIT		( 1U << 25 )
#define GM_CYCLE_STOP		( 4U << 25 )
#define GM_READ			1U
#define GM_HW_RDY		( 1U << 11 )
#define GM_SATOER		( 1U << 10 )
#define GM_ACTIVE		( 1U << 9 )

#define EDID_ADDR		0x50
#define EDID_LEN		128

LOCAL UBINT	ex_mmio = 0;

LOCAL UW ex_rd( UW off )
{
	return in_w(ex_mmio + off);
}

LOCAL void ex_wr( UW off, UW v )
{
	out_w(ex_mmio + off, v);
}

/* One AUX transaction: tx bytes sent, the reply's bytes (its first the reply code) in rx. Their number or an error */
LOCAL INT aux_xfer( INT port, CONST UB *tx, INT ntx, UB *rx, INT max )
{
	UW	w = 0, st = 0;
	INT	i, k, n;

	if ( ex_rd(AUX_CTL(port)) & AUX_BUSY ) return E_BUSY;
	for ( i = 0; i < 5; i++ ) {
		w = 0;
		for ( k = 0; k < 4; k++ ) {
			if ( i * 4 + k < ntx ) w |= (UW)tx[i * 4 + k] << ( 24 - 8 * k );
		}
		ex_wr(AUX_DATA(port, i), w);
	}
	ex_wr(AUX_CTL(port), AUX_BUSY | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR | AUX_TIME_1600US
	      | AUX_SIZE(ntx) | AUX_SYNC_SKL);
	for ( i = 0; i < 100; i++ ) {
		WaitUsec(100);
		st = ex_rd(AUX_CTL(port));
		if ( ( st & AUX_BUSY ) == 0 ) break;
	}
	ex_wr(AUX_CTL(port), st | AUX_DONE | AUX_TIMEOUT_ERR | AUX_RECV_ERR);
	if ( st & ( AUX_BUSY | AUX_TIMEOUT_ERR ) ) return E_TMOUT;
	if ( st & AUX_RECV_ERR ) return E_IO;
	n = (INT)( ( st >> 20 ) & 0x1f );
	if ( n > max ) n = max;
	for ( i = 0; i < n; i++ ) {
		if ( i % 4 == 0 ) w = ex_rd(AUX_DATA(port, i / 4));
		rx[i] = (UB)( w >> ( 24 - 8 * ( i % 4 ) ) );
	}
	return n;
}

/* The receiver's first capability bytes, native AUX: its DPCD revision, the most lanes and rate */
LOCAL BOOL dp_found( INT port )
{
	UB	tx[4] = { AUX_NATIVE_READ << 4, 0, 0, 3 }, rx[8];
	INT	n, t;

	for ( t = 0; t < 3; t++ ) {
		n = aux_xfer(port, tx, 4, rx, sizeof(rx));
		if ( n >= 5 && rx[0] == AUX_REPLY_ACK ) {
			tm_printf((UB *)"ext: port %c: DisplayPort: DPCD revision %02x, rate code %02x, lanes %02x, down spread %02x\n",
				  'A' + port, rx[1], rx[2], rx[3], rx[4]);
			return TRUE;
		}
		tk_dly_tsk(2);
	}
	return FALSE;
}

/* The EDID through I2C over AUX, 16 bytes a time */
LOCAL BOOL edid_aux( INT port, UB *e )
{
	UB	tx[5], rx[20];
	INT	got = 0, n, tries = 0;

	tx[0] = ( AUX_I2C_WRITE | AUX_I2C_MOT ) << 4;
	tx[1] = 0;
	tx[2] = EDID_ADDR;
	tx[3] = 0;
	tx[4] = 0;
	if ( aux_xfer(port, tx, 5, rx, sizeof(rx)) < 1 || rx[0] != AUX_REPLY_ACK ) return FALSE;
	while ( got < EDID_LEN && tries < 200 ) {
		tries++;
		tx[0] = ( AUX_I2C_READ | AUX_I2C_MOT ) << 4;
		tx[1] = 0;
		tx[2] = EDID_ADDR;
		tx[3] = (UB)( ( EDID_LEN - got > 16 ? 16 : EDID_LEN - got ) - 1 );
		n = aux_xfer(port, tx, 4, rx, sizeof(rx));
		if ( n < 1 ) continue;
		if ( rx[0] != AUX_REPLY_ACK ) {
			tk_dly_tsk(1);		/* deferred */
			continue;
		}
		if ( n > 1 ) {
			knl_memcpy(e + got, rx + 1, n - 1);
			got += n - 1;
		}
	}
	tx[0] = AUX_I2C_READ << 4;
	tx[3] = 0;
	(void)aux_xfer(port, tx, 3, rx, sizeof(rx));
	return got == EDID_LEN;
}

/* GMBUS's pin pair of the port */
LOCAL UW gm_pin( INT port )
{
	return ( port == 1 ) ? 5 : ( port == 2 ) ? 4 : 6;
}

LOCAL BOOL edid_gmbus( INT port, UB *e )
{
	INT	got = 0, i, t;
	UW	st = 0, w;

	ex_wr(GMBUS0, gm_pin(port));
	ex_wr(GMBUS1, GM_SW_RDY | GM_CYCLE_WAIT | GM_CYCLE_STOP | ( (UW)EDID_LEN << 16 ) | ( EDID_ADDR << 1 ) | GM_READ);
	while ( got < EDID_LEN ) {
		for ( t = 0; t < 500; t++ ) {
			st = ex_rd(GMBUS2);
			if ( st & ( GM_HW_RDY | GM_SATOER ) ) break;
			WaitUsec(100);
		}
		if ( ( st & GM_HW_RDY ) == 0 || ( st & GM_SATOER ) != 0 ) {
			tm_printf((UB *)"ext: port %c: GMBUS stopped at byte %d: %08x\n", 'A' + port, got, st);
			ex_wr(GMBUS1, 1U << 31);
			ex_wr(GMBUS1, 0);
			ex_wr(GMBUS0, 0);
			return FALSE;
		}
		w = ex_rd(GMBUS3);
		for ( i = 0; i < 4 && got < EDID_LEN; i++ ) {
			e[got++] = (UB)( w >> ( 8 * i ) );
		}
	}
	ex_wr(GMBUS0, 0);
	return TRUE;
}

LOCAL void edid_say( INT port, CONST UB *e, CONST char *how )
{
	INT	i, k, sum = 0;
	char	name[16];

	for ( i = 0; i < EDID_LEN; i++ ) sum += e[i];
	name[0] = '\0';
	for ( i = 54; i < 126; i += 18 ) {
		if ( e[i] == 0 && e[i + 1] == 0 && e[i + 3] == 0xfc ) {
			for ( k = 0; k < 13 && e[i + 5 + k] != '\n'; k++ ) name[k] = (char)e[i + 5 + k];
			name[k] = '\0';
		}
	}
	tm_printf((UB *)"ext: port %c: EDID by %s, sum %s, \"%s\", maker %02x%02x product %02x%02x, "
		  "preferred %d x %d at %d kHz pixel clock, %d extension(s)\n", 'A' + port, how,
		  ( sum & 0xff ) == 0 ? "good" : "BAD", name, e[8], e[9], e[11], e[10],
		  e[56] | ( ( e[58] & 0xf0 ) << 4 ), e[59] | ( ( e[61] & 0xf0 ) << 4 ),
		  ( e[54] | ( e[55] << 8 ) ) * 10, e[126]);
}

LOCAL void port_look( INT port )
{
	UB	e[EDID_LEN];

	if ( dp_found(port) ) {
		if ( edid_aux(port, e) ) {
			edid_say(port, e, "AUX");
		} else {
			tm_printf((UB *)"ext: port %c: no EDID through AUX\n", 'A' + port);
		}
	} else if ( edid_gmbus(port, e) ) {
		edid_say(port, e, "GMBUS");
	} else {
		tm_printf((UB *)"ext: port %c: something is plugged in, neither AUX nor GMBUS answers\n", 'A' + port);
	}
}

LOCAL void ext_task( INT stacd, void *exinf )
{
	UW	was = 0, now, ps;
	INT	p;

	ps = ex_rd(PWR_WELL_CTL2);
	ex_wr(PWR_WELL_CTL2, ps | PW_REQ(PW_MISC_IO) | PW_REQ(PW_DDI_B) | PW_REQ(PW_DDI_B + 1) | PW_REQ(PW_DDI_B + 2));
	tk_dly_tsk(20);
	tm_printf((UB *)"ext: power wells %08x -> %08x; hot plug control %08x\n", ps, ex_rd(PWR_WELL_CTL2),
		  ex_rd(PCH_PORT_HOTPLUG));
	ex_wr(PCH_PORT_HOTPLUG, ex_rd(PCH_PORT_HOTPLUG) | HP_ENABLE(1) | HP_ENABLE(2) | HP_ENABLE(3));
	tk_dly_tsk(120);
	for ( ;; ) {
		now = ex_rd(SDEISR);
		for ( p = 1; p <= 3; p++ ) {
			if ( ( ( now ^ was ) & SDE_HPD(p) ) == 0 ) continue;
			tm_printf((UB *)"ext: port %c %s (SDEISR %08x)\n", 'A' + p,
				  ( now & SDE_HPD(p) ) ? "plugged in" : "taken out", now);
			if ( now & SDE_HPD(p) ) {
				tk_dly_tsk(200);
				port_look(p);
			}
		}
		was = now;
		tk_dly_tsk(1000);
	}
}

/* From the screen's start-up (disp_gop.c): with the setting ext=1 on an Intel display */
EXPORT void knl_disp_ext_start( void )
{
	T_CTSK	ct;
	ID	t;

	ex_mmio = knl_igfx_mmio();
	if ( ex_mmio == 0 ) return;
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ext_task;
	ct.itskpri = 28;
	ct.stksz = 8192;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) (void)tk_sta_tsk(t, 0);
}

#endif /* X64 */
