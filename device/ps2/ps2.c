/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ps2.c (x64 PC)
 *	The keyboard and the pointer behind the i8042 controller: the
 *	built-in keyboard of a notebook PC and its pointing stick or touch
 *	pad in PS/2 mode (the ThinkPad's TrackPoint), and QEMU's.
 *
 *	The controller is taken when the ACPI namespace has its keyboard
 *	(PNP0303 and its kin) or, without the interpreter, when the FADT
 *	says there is one and its status port answers. The keyboard is set
 *	to scan code set 2 with the controller's translation off; the mouse
 *	is asked for the IntelliMouse's wheel (four byte packets). The
 *	interrupts (ISA 1 and 12, as the MADT moves them) put the bytes in a
 *	ring; a task of the driver turns them into reports of the USB HID
 *	boot protocol and hands them to the HID layer (knl_hid_ext_report),
 *	so that a key or a move from here goes the same way as one from USB:
 *	the key layout, the repeat, the pointer's speed. They are device
 *	objects as the USB ones are (obinput.c): "ps2kbd" and "ps2aux".
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <sys/sysdef.h>
#include "../../kernel/sysdepend/acpi/acpi.h"

IMPORT INT	knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name );
IMPORT void	knl_hid_ext_report( INT hd, CONST UB *r, INT len );
IMPORT void	knl_usb_lock( void );
IMPORT void	knl_usb_unlock( void );

#define DATA		0x60
#define STATUS		0x64
#define CMD		0x64

#define ST_OBF		0x01
#define ST_IBF		0x02
#define ST_AUX		0x20

#define CFG_IRQ1	0x01
#define CFG_IRQ12	0x02
#define CFG_NOCLK1	0x10
#define CFG_NOCLK2	0x20
#define CFG_XLATE	0x40

#define RING		512

IMPORT UD	knl_get_mono_ns( void );

LOCAL UB	ring[RING];
LOCAL UH	ring_src[RING / 8 * 8];		/* a bit per byte: from the mouse */
LOCAL UW	ring_us[RING];			/* when each came, in microseconds */
LOCAL volatile UW	rhead = 0, rtail = 0;
LOCAL T_SPLOCK	take_lock;			/* the status and the byte read as one, on any processor */
LOCAL ID	ps2_sem = 0;
LOCAL INT	kbd_hd = 0, mou_hd = 0;
LOCAL BOOL	mou_wheel = FALSE;
LOCAL UINT	kbd_int = 0, mou_int = 0;

/* ---------------------------------------------------------------- the controller */

LOCAL BOOL wait_in( void )
{
	INT	i;

	for ( i = 0; i < 10000; i++ ) {
		if ( ( in_port_b(STATUS) & ST_IBF ) == 0 ) return TRUE;
		WaitUsec(10);
	}
	return FALSE;
}

LOCAL INT read_out( INT ms )
{
	INT	i;

	for ( i = 0; i < ms * 100; i++ ) {
		if ( in_port_b(STATUS) & ST_OBF ) return in_port_b(DATA);
		WaitUsec(10);
	}
	return -1;
}

LOCAL void ctl( UB c )
{
	(void)wait_in();
	out_port_b(CMD, c);
}

LOCAL void flush( void )
{
	INT	i;

	for ( i = 0; i < 64 && ( in_port_b(STATUS) & ST_OBF ); i++ ) {
		(void)in_port_b(DATA);
		WaitUsec(50);
	}
}

/* A byte to the keyboard (aux FALSE) or the mouse, and its acknowledgement */
LOCAL INT dev_cmd( BOOL aux, UB b )
{
	INT	r, t;

	for ( t = 0; t < 3; t++ ) {
		if ( aux ) ctl(0xd4);
		(void)wait_in();
		out_port_b(DATA, b);
		r = read_out(100);
		if ( r != 0xfe ) return r;		/* 0xfe: send it again */
	}
	return r;
}

/* ---------------------------------------------------------------- the interrupts */

/*
 * The bytes waiting in the controller, into the ring with where each
 * came from and when. The keyboard's and the mouse's interrupts may be
 * taken on two processors at once: the status and the byte it speaks of
 * are read under one lock, or one could read the byte the other's
 * status was about, and the mouse's packets would lose their step.
 */
LOCAL void take( UINT intno )
{
	UB	st;
	INT	n = 0;
	UINT	imask;
	UW	us = (UW)( knl_get_mono_ns() / 1000 );

	ISpinLock(&take_lock, &imask);
	while ( ( ( st = in_port_b(STATUS) ) & ST_OBF ) && n++ < 32 ) {
		UB	b = in_port_b(DATA);
		UW	h = rhead;

		if ( h - rtail < RING ) {
			ring[h % RING] = b;
			ring_us[h % RING] = us;
			if ( st & ST_AUX ) {
				ring_src[( h % RING ) / 16] |= (UH)( 1U << ( h % 16 ) );
			} else {
				ring_src[( h % RING ) / 16] &= (UH)~( 1U << ( h % 16 ) );
			}
			rhead = h + 1;
		}
	}
	ISpinUnlock(&take_lock, &imask);
	EndOfInt(intno);
	tk_sig_sem(ps2_sem, 1);
}

LOCAL void ps2_inthdr( UINT intno )
{
	take(intno);
}

/* ---------------------------------------------------------------- the keyboard */

/* Scan code set 2 to the HID usages: the codes without a prefix */
LOCAL CONST UB set2[0x84] = {
	[0x76] = 0x29, [0x05] = 0x3a, [0x06] = 0x3b, [0x04] = 0x3c, [0x0c] = 0x3d, [0x03] = 0x3e,
	[0x0b] = 0x3f, [0x83] = 0x40, [0x0a] = 0x41, [0x01] = 0x42, [0x09] = 0x43, [0x78] = 0x44,
	[0x07] = 0x45, [0x0e] = 0x35, [0x16] = 0x1e, [0x1e] = 0x1f, [0x26] = 0x20, [0x25] = 0x21,
	[0x2e] = 0x22, [0x36] = 0x23, [0x3d] = 0x24, [0x3e] = 0x25, [0x46] = 0x26, [0x45] = 0x27,
	[0x4e] = 0x2d, [0x55] = 0x2e, [0x66] = 0x2a, [0x0d] = 0x2b, [0x15] = 0x14, [0x1d] = 0x1a,
	[0x24] = 0x08, [0x2d] = 0x15, [0x2c] = 0x17, [0x35] = 0x1c, [0x3c] = 0x18, [0x43] = 0x0c,
	[0x44] = 0x12, [0x4d] = 0x13, [0x54] = 0x2f, [0x5b] = 0x30, [0x5d] = 0x31, [0x58] = 0x39,
	[0x1c] = 0x04, [0x1b] = 0x16, [0x23] = 0x07, [0x2b] = 0x09, [0x34] = 0x0a, [0x33] = 0x0b,
	[0x3b] = 0x0d, [0x42] = 0x0e, [0x4b] = 0x0f, [0x4c] = 0x33, [0x52] = 0x34, [0x5a] = 0x28,
	[0x12] = 0xe1, [0x1a] = 0x1d, [0x22] = 0x1b, [0x21] = 0x06, [0x2a] = 0x19, [0x32] = 0x05,
	[0x31] = 0x11, [0x3a] = 0x10, [0x41] = 0x36, [0x49] = 0x37, [0x4a] = 0x38, [0x59] = 0xe5,
	[0x14] = 0xe0, [0x11] = 0xe2, [0x29] = 0x2c, [0x77] = 0x53, [0x7e] = 0x47, [0x7c] = 0x55,
	[0x7b] = 0x56, [0x79] = 0x57, [0x71] = 0x63, [0x70] = 0x62, [0x69] = 0x59, [0x72] = 0x5a,
	[0x7a] = 0x5b, [0x6b] = 0x5c, [0x73] = 0x5d, [0x74] = 0x5e, [0x6c] = 0x5f, [0x75] = 0x60,
	[0x7d] = 0x61, [0x13] = 0x88, [0x6a] = 0x89, [0x51] = 0x87, [0x64] = 0x8a, [0x67] = 0x8b,
	[0x61] = 0x64,
};

/* The codes after 0xE0 */
LOCAL UB set2_e0( UB c )
{
	switch ( c ) {
	case 0x14: return 0xe4;		/* right Ctrl */
	case 0x11: return 0xe6;		/* right Alt */
	case 0x1f: return 0xe3;		/* left GUI */
	case 0x27: return 0xe7;		/* right GUI */
	case 0x2f: return 0x65;		/* menu */
	case 0x4a: return 0x54;		/* keypad / */
	case 0x5a: return 0x58;		/* keypad Enter */
	case 0x69: return 0x4d;		/* End */
	case 0x6b: return 0x50;		/* left */
	case 0x6c: return 0x4a;		/* Home */
	case 0x70: return 0x49;		/* Insert */
	case 0x71: return 0x4c;		/* Delete */
	case 0x72: return 0x51;		/* down */
	case 0x74: return 0x4f;		/* right */
	case 0x75: return 0x52;		/* up */
	case 0x7a: return 0x4e;		/* Page Down */
	case 0x7d: return 0x4b;		/* Page Up */
	case 0x7c: return 0x46;		/* Print Screen */
	case 0x7e: return 0x48;		/* Ctrl+Pause (Break) */
	default:   return 0;		/* 0x12 and 0x59: the shift Print Screen pretends */
	}
}

LOCAL UB	kmods = 0;
LOCAL UB	kkeys[6];
LOCAL INT	kstate = 0;		/* 1 after E0, 2 after F0, 3 after E0 F0, >= 10 in the Pause sequence */

LOCAL void kbd_send( void )
{
	UB	r[8];

	r[0] = kmods;
	r[1] = 0;
	knl_memcpy(r + 2, kkeys, 6);
	knl_usb_lock();
	knl_hid_ext_report(kbd_hd, r, 8);
	knl_usb_unlock();
}

LOCAL void key( UB usage, BOOL down )
{
	INT	i, free = -1;

	if ( usage == 0 ) return;
	if ( usage >= 0xe0 && usage <= 0xe7 ) {
		UB	bit = (UB)( 1U << ( usage - 0xe0 ) );

		if ( down == ( ( kmods & bit ) != 0 ) ) return;
		kmods = down ? ( kmods | bit ) : ( kmods & ~bit );
		kbd_send();
		return;
	}
	for ( i = 0; i < 6; i++ ) {
		if ( kkeys[i] == usage ) {
			if ( down ) return;		/* the keyboard's own repeat */
			kkeys[i] = 0;
			kbd_send();
			return;
		}
		if ( kkeys[i] == 0 && free < 0 ) free = i;
	}
	if ( down && free >= 0 ) {
		kkeys[free] = usage;
		kbd_send();
	}
}

LOCAL void kbd_byte( UB b )
{
	if ( kstate >= 10 ) {			/* E1 14 77 E1 F0 14 F0 77: Pause */
		if ( ++kstate >= 17 ) {
			kstate = 0;
			key(0x48, TRUE);
			key(0x48, FALSE);
		}
		return;
	}
	switch ( b ) {
	case 0xe0:	kstate = 1; return;
	case 0xe1:	kstate = 10; return;
	case 0xf0:	kstate = ( kstate == 1 ) ? 3 : 2; return;
	case 0xfa: case 0xfe: case 0xaa: case 0xee: case 0x00: case 0xff:
		return;				/* acknowledgements, self test, overruns */
	default:
		break;
	}
	if ( kstate == 1 || kstate == 3 ) {
		key(set2_e0(b), (BOOL)( kstate == 1 ));
	} else {
		key(( b < sizeof(set2) ) ? set2[b] : 0, (BOOL)( kstate == 0 ));
	}
	kstate = 0;
}

/* ---------------------------------------------------------------- the mouse */

/*
 * The mouse's packets kept in step. The first byte of a packet has bit 3
 * set and its two overflow bits (6, 7) clear; a byte that is not so
 * where a packet should begin is dropped, which also passes over an
 * acknowledgement (FA) or a self-test's AA come in the stream. The bytes
 * of one packet come within a few milliseconds of each other, and the
 * packets themselves at most 100 a second: a packet begun and left
 * longer than PKT_GAP_US is thrown away, so that one byte lost puts the
 * stream out of step only until the pointer next stops. Out of step, a
 * movement's byte read as a first byte (FF for -1 has all three button
 * bits) would press buttons nobody pressed.
 */
#define PKT_GAP_US	20000

LOCAL UB	mpkt[4];
LOCAL INT	mn = 0;
LOCAL UW	m_last_us = 0;
LOCAL UB	m_buttons = 0;
LOCAL UD	m_packets = 0, m_dropped = 0, m_cut = 0;

LOCAL void mou_byte( UB b, UW us )
{
	INT	need = mou_wheel ? 4 : 3;

	if ( mn > 0 && us - m_last_us > PKT_GAP_US ) {
		mn = 0;				/* left unfinished: thrown away */
		m_cut++;
	}
	m_last_us = us;
	if ( mn == 0 && ( b & 0xc8 ) != 0x08 ) {
		m_dropped++;			/* out of step: not a packet's first byte */
		return;
	}
	mpkt[mn++] = b;
	if ( mn < need ) return;
	mn = 0;
	m_packets++;
	if ( ( mpkt[0] & 0x07 ) != m_buttons ) {
		tm_printf((UB *)"ps2: buttons %c%c%c (packets %d, bytes out of step %d, packets cut %d)\n",
			  ( mpkt[0] & 1 ) ? 'L' : '-', ( mpkt[0] & 4 ) ? 'M' : '-', ( mpkt[0] & 2 ) ? 'R' : '-',
			  (INT)m_packets, (INT)m_dropped, (INT)m_cut);
		m_buttons = mpkt[0] & 0x07;
	}
	{
		INT	dx = mpkt[1] - ( ( mpkt[0] & 0x10 ) ? 256 : 0 );
		INT	dy = mpkt[2] - ( ( mpkt[0] & 0x20 ) ? 256 : 0 );
		INT	dz = mou_wheel ? (INT)(B)( ( mpkt[3] & 0x08 ) ? ( mpkt[3] | 0xf0 ) : ( mpkt[3] & 0x0f ) ) : 0;
		UB	r[4];

		if ( mpkt[0] & 0xc0 ) dx = dy = 0;	/* overflow */
		dy = -dy;				/* PS/2 counts up, the screen down */
		if ( dx > 127 ) dx = 127;
		if ( dx < -127 ) dx = -127;
		if ( dy > 127 ) dy = 127;
		if ( dy < -127 ) dy = -127;
		r[0] = mpkt[0] & 0x07;
		r[1] = (UB)dx;
		r[2] = (UB)dy;
		r[3] = (UB)( -dz );		/* the wheel away from the user is up */
		knl_usb_lock();
		knl_hid_ext_report(mou_hd, r, 4);
		knl_usb_unlock();
	}
}

LOCAL void ps2_task( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		tk_wai_sem(ps2_sem, 1, TMO_FEVR);
		while ( rtail != rhead ) {
			UW	t = rtail;
			UB	b = ring[t % RING];
			UW	us = ring_us[t % RING];
			BOOL	aux = (BOOL)( ( ring_src[( t % RING ) / 16] >> ( t % 16 ) ) & 1 );

			rtail = t + 1;
			if ( aux ) {
				if ( mou_hd > 0 ) mou_byte(b, us);
			} else {
				if ( kbd_hd > 0 ) kbd_byte(b);
			}
		}
	}
}

/* ---------------------------------------------------------------- start-up */

LOCAL UINT irq_of( UB isa )
{
	UW	gsi = knl_acpi_isa_gsi(isa, NULL);

	return ( gsi < INTVEC_GSI_NUM ) ? INTVEC_GSI(gsi) : 0;
}

EXPORT ER knl_ps2_init( void )
{
	T_CSEM	cs;
	T_CTSK	ct;
	INT	cfg, r, id = -1;
	BOOL	acpi_kbd, acpi_mou, mouse = FALSE;
	ID	t;

	acpi_kbd = knl_acpica_ready() && ( knl_acpica_has_hid("PNP0303") || knl_acpica_has_hid("PNP030B")
					   || knl_acpica_has_hid("LEN0071") );
	acpi_mou = knl_acpica_ready() && ( knl_acpica_has_hid("PNP0F13") || knl_acpica_has_hid("PNP0F03") );
	if ( !acpi_kbd && ( knl_acpica_ready() || ( knl_acpi.iapc_boot & 0x0002 ) == 0 || in_port_b(STATUS) == 0xff ) ) {
		return E_NOEXS;
	}

	InitSpinLock(&take_lock);
	/* both ports off, what is waiting thrown away, the configuration read */
	ctl(0xad);
	ctl(0xa7);
	flush();
	ctl(0x20);
	cfg = read_out(100);
	if ( cfg < 0 ) {
		tm_printf((UB *)"TessronOS: ps2: the controller does not answer\n");
		return E_IO;
	}
	cfg &= ~( CFG_IRQ1 | CFG_IRQ12 | CFG_XLATE );
	ctl(0x60);
	(void)wait_in();
	out_port_b(DATA, (UB)cfg);

	/* the keyboard: on, set 2, scanning */
	ctl(0xae);
	(void)dev_cmd(FALSE, 0xf5);
	if ( dev_cmd(FALSE, 0xf0) == 0xfa ) (void)dev_cmd(FALSE, 0x02);
	r = dev_cmd(FALSE, 0xf4);
	flush();

	/* the mouse, when there is one: defaults, the wheel asked for, reporting */
	if ( acpi_mou || !knl_acpica_ready() ) {
		ctl(0xa8);
		if ( dev_cmd(TRUE, 0xf6) == 0xfa ) {
			mouse = TRUE;
			(void)dev_cmd(TRUE, 0xf3); (void)dev_cmd(TRUE, 200);
			(void)dev_cmd(TRUE, 0xf3); (void)dev_cmd(TRUE, 100);
			(void)dev_cmd(TRUE, 0xf3); (void)dev_cmd(TRUE, 80);
			if ( dev_cmd(TRUE, 0xf2) == 0xfa ) id = read_out(100);
			mou_wheel = (BOOL)( id == 3 || id == 4 );
			(void)dev_cmd(TRUE, 0xf4);
		}
		flush();
	}

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 0x7fffffff;
	ps2_sem = tk_cre_sem(&cs);
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ps2_task;
	ct.itskpri = 10;
	ct.stksz = 8 * 1024;
	t = tk_cre_tsk(&ct);
	if ( ps2_sem <= 0 || t <= 0 ) {
		return E_NOMEM;
	}
	kbd_hd = knl_hid_ext_attach(TRUE, NULL, 0, (CONST UB *)"ps2kbd");
	if ( mouse ) mou_hd = knl_hid_ext_attach(FALSE, NULL, 0, (CONST UB *)"ps2aux");
	tk_sta_tsk(t, 0);

	/* the interrupts on: in the IOAPIC, then in the controller */
	kbd_int = irq_of(1);
	if ( kbd_int != 0 ) {
		knl_define_inthdr((INT)kbd_int, TA_HLNG, (FP)ps2_inthdr);
		EnableInt(kbd_int, INTPRI_DEVICE);
	}
	if ( mouse ) {
		mou_int = irq_of(12);
		if ( mou_int != 0 ) {
			knl_define_inthdr((INT)mou_int, TA_HLNG, (FP)ps2_inthdr);
			EnableInt(mou_int, INTPRI_DEVICE);
		}
	}
	/*
	 * The configuration read again with both ports off: a byte the mouse
	 * or the keyboard sends meanwhile (the TrackPoint touched as the
	 * machine starts) would be read as the configuration, and the mouse's
	 * packets would start out of step
	 */
	ctl(0xad);
	if ( mouse ) ctl(0xa7);
	flush();
	ctl(0x20);
	cfg = read_out(100);
	if ( cfg >= 0 ) {
		cfg = ( cfg | CFG_IRQ1 | ( mouse ? CFG_IRQ12 : 0 ) ) & ~( CFG_XLATE | CFG_NOCLK1 | ( mouse ? CFG_NOCLK2 : 0 ) );
		ctl(0x60);
		(void)wait_in();
		out_port_b(DATA, (UB)cfg);
	}
	ctl(0xae);
	if ( mouse ) ctl(0xa8);
	tm_printf((UB *)"TessronOS: ps2: keyboard (%s, %02x) on interrupt %d, %s\n",
		  acpi_kbd ? "ACPI" : "FADT", r & 0xff, kbd_int,
		  mouse ? ( mou_wheel ? "a mouse with a wheel" : "a mouse" ) : "no mouse");
	return E_OK;
}

#endif /* X64 */
