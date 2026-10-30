/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hid_i2c.c (x64 PC)
 *	HID over I2C: the touch screens, touch pads and pens of notebook PCs
 *	(the Surface Go 2's ELAN digitizer, a ThinkPad's), which speak the
 *	USB HID's descriptors and reports over an I2C bus.
 *
 *	A device is found in the ACPI namespace (PNP0C50 or ACPI0C50, as
 *	its _HID or its _CID): its I2C address, the bus's speed and its
 *	controller from its _CRS, the register of its HID descriptor from
 *	its _DSM (function 1 of the HID over I2C UUID); its _PS0 is run. The
 *	HID descriptor (30 bytes) names the registers of the report
 *	descriptor, of the input reports, of the commands and of their data.
 *	The device is powered (SET_POWER ON, then 60 ms: some devices are
 *	not ready sooner though the protocol says they stretch the clock)
 *	and reset (RESET, after which it raises its interrupt and gives an
 *	input of length 0), powered again after the reset, its report
 *	descriptor read and given to the HID layer, which parses it as a USB
 *	device's (knl_hid_ext_attach_dg). A pen or a touch screen is put
 *	into the mode it is read in with the feature report the HID layer
 *	made (SET_REPORT: the command register, the opcode with the report's
 *	type and ID, the data register, then the report with its length
 *	first), as a USB interface's is, so that the digitizer's Tip Switch,
 *	Confidence and Contact Identifier, and the hold that is the second
 *	button, are read the same way. Each input report (two bytes of
 *	length, then the report) is handed on as a USB report is. The device
 *	is an object of its name ("hidi2c0", ...), in the input that every
 *	keyboard and pointer feeds.
 *
 *	Its interrupt, when its _CRS gives an Interrupt (an input of the
 *	IOAPICs; a GPIO pad the firmware routes there is one), says when a
 *	report waits: the line is level triggered and stays asserted until
 *	the report is read, so it is masked as it comes and unmasked after
 *	the read. Without one (a GpioInt, whose GPIO controller is not
 *	driven yet), or when reports are found waiting while no interrupt
 *	came, the device is read every few milliseconds.
 *
 *	Some devices need otherwise (the quirks): an ELAN controller breaks
 *	when told to power on after its reset, and may raise its interrupt
 *	with nothing to read (an input whose length is 0xFFFF, dropped
 *	anyway); a few raise no interrupt after the reset, and are waited
 *	for 100 ms instead.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "../../../kernel/sysdepend/acpi/acpi.h"
#include "i2c.h"

IMPORT INT	knl_hid_ext_attach_dg( CONST UB *rd, INT rdlen, CONST UB *name, UB *mode, INT *p_mlen, INT *p_mrid );
IMPORT void	knl_hid_ext_report( INT hd, CONST UB *r, INT len );
IMPORT void	knl_usb_lock( void );
IMPORT void	knl_usb_unlock( void );
IMPORT void	knl_obinput_sync( void );	/* obinput.c: the keyboards and pointers as objects */

#define HI_MAX		4
#define HI_RD_MAX	4096
#define HI_IN_MAX	256
#define HI_FT_MAX	64		/* the longest feature report sent */
#define HI_IDLE_MS	1000		/* with the interrupt: a read this long after the last one came, all the same */
#define HI_MISSED	3		/* reports found by those reads in a row: the interrupt is not arriving */
#define HI_RESET_MS	5000		/* the longest a reset is waited for */

#define OP_RESET	0x1
#define OP_GET_REPORT	0x2
#define OP_SET_REPORT	0x3
#define OP_SET_POWER	0x8
#define RT_FEATURE	3		/* a report's type in a command */
#define PWR_ON		0

#define Q_NO_WAKEUP_AFTER_RESET	0x01	/* no SET_POWER ON after the reset */
#define Q_NO_IRQ_AFTER_RESET	0x02	/* no interrupt after the reset: 100 ms waited */
#define Q_BOGUS_IRQ		0x04	/* interrupts with nothing to read (length 0xFFFF) */

#define VID_ANY		0xffff

LOCAL CONST struct {
	UH	vid, pid;
	UW	q;
} quirks[] = {
	{ 0x04f3, VID_ANY, Q_NO_WAKEUP_AFTER_RESET | Q_BOGUS_IRQ },	/* ELAN */
	{ 0x0911, 0x5288, Q_NO_IRQ_AFTER_RESET },			/* Hantick 5288 */
	{ 0x2386, 0x3118, Q_NO_IRQ_AFTER_RESET },			/* Raydium 3118 */
};

typedef struct {
	BOOL		used;
	T_I2CBUS	*bus;
	UH		addr;
	UH		rdesc_reg, rdesc_len, in_reg, in_max, cmd_reg, data_reg;
	UH		vid, pid;
	UW		quirks;
	INT		hd;
	INT		period;
	UINT		intno;		/* its interrupt, 0 none */
	BOOL		edge;		/* edge triggered: not masked, and read until nothing waits */
	ID		sem;		/* signalled by it */
	BOOL		polled;		/* read every period ms */
	INT		missed;
	UD		reports, ints;
	UB		name[16];
} HIDI2C;

LOCAL HIDI2C	hi[HI_MAX];

/* Bytes of the device from a register of 16 bits */
LOCAL ER reg_read( HIDI2C *h, UH reg, UB *buf, INT n )
{
	UB	w[2];

	w[0] = (UB)reg;
	w[1] = (UB)( reg >> 8 );
	return h->bus->xfer(h->bus->ctx, h->addr, w, 2, buf, n);
}

/* A command: the command register, the opcode and its argument */
LOCAL ER command( HIDI2C *h, UB op, UB arg )
{
	UB	w[4];

	w[0] = (UB)h->cmd_reg;
	w[1] = (UB)( h->cmd_reg >> 8 );
	w[2] = arg;
	w[3] = op;
	return h->bus->xfer(h->bus->ctx, h->addr, w, 4, NULL, 0);
}

/*
 * The command register and a command of a report (the low byte its type
 * and ID, the high its opcode; an ID of 15 and more in a third byte),
 * then the data register, into w: its length
 */
LOCAL INT report_cmd( HIDI2C *h, UB *w, UB op, UB type, INT rid )
{
	INT	k = 0;

	w[k++] = (UB)h->cmd_reg;
	w[k++] = (UB)( h->cmd_reg >> 8 );
	if ( rid < 0x0f ) {
		w[k++] = (UB)( ( type << 4 ) | rid );
		w[k++] = op;
	} else {
		w[k++] = (UB)( ( type << 4 ) | 0x0f );
		w[k++] = op;
		w[k++] = (UB)rid;
	}
	w[k++] = (UB)h->data_reg;
	w[k++] = (UB)( h->data_reg >> 8 );
	return k;
}

/* A report of the type and ID set (SET_REPORT): its data without the ID, n bytes */
LOCAL ER set_report( HIDI2C *h, UB type, INT rid, CONST UB *data, INT n )
{
	UB	w[8 + 3 + HI_FT_MAX];
	INT	k, len;

	if ( n < 0 || n > HI_FT_MAX ) return E_PAR;
	k = report_cmd(h, w, OP_SET_REPORT, type, rid);
	len = 2 + ( rid != 0 ? 1 : 0 ) + n;		/* the length counts itself */
	w[k++] = (UB)len;
	w[k++] = (UB)( len >> 8 );
	if ( rid != 0 ) w[k++] = (UB)rid;
	knl_memcpy(w + k, data, n);
	return h->bus->xfer(h->bus->ctx, h->addr, w, k + n, NULL, 0);
}

/* A report of the type and ID read (GET_REPORT) into r (max bytes, its length first): the length, or an error */
LOCAL INT get_report( HIDI2C *h, UB type, INT rid, UB *r, INT max )
{
	UB	w[8];
	INT	k, len;
	ER	er;

	k = report_cmd(h, w, OP_GET_REPORT, type, rid);
	er = h->bus->xfer(h->bus->ctx, h->addr, w, k, r, max);
	if ( er < E_OK ) return er;
	len = r[0] | ( r[1] << 8 );
	return ( len < 2 || len > max ) ? E_OBJ : len;
}

/* One input report, when the device has one: its length (0 none, -1 the bus failed) */
LOCAL INT read_input( HIDI2C *h, UB *in )
{
	INT	len;
	INT	max = ( h->in_max > 2 && h->in_max <= HI_IN_MAX ) ? h->in_max : HI_IN_MAX;

	if ( h->bus->xfer(h->bus->ctx, h->addr, NULL, 0, in, max) < E_OK ) {
		return -1;
	}
	len = in[0] | ( in[1] << 8 );
	return ( len <= 2 || len > max ) ? 0 : len;	/* 0xFFFF too: an interrupt with nothing behind it */
}

/* The interrupt: masked until the report it tells of is read, the reader woken */
LOCAL void hi_inthdr( UINT intno )
{
	INT	i;
	BOOL	edge = FALSE;

	for ( i = 0; i < HI_MAX; i++ ) {
		if ( hi[i].intno == intno && hi[i].sem > 0 ) {
			edge = hi[i].edge;
			hi[i].ints++;
			tk_sig_sem(hi[i].sem, 1);
		}
	}
	if ( !edge ) DisableInt(intno);
	EndOfInt(intno);
}

LOCAL void unmask( HIDI2C *h )
{
	if ( h->intno != 0 ) EnableInt(h->intno, INTPRI_DEVICE);
}

LOCAL void hi_task( INT stacd, void *exinf )
{
	HIDI2C	*h = (HIDI2C *)exinf;
	UB	in[HI_IN_MAX];
	INT	len;
	ER	er;

	(void)stacd;
	for ( ;; ) {
		er = E_OK;
		if ( h->polled ) {
			tk_dly_tsk(h->period);
		} else {
			er = tk_wai_sem(h->sem, 1, HI_IDLE_MS);
		}
		do {
			len = read_input(h, in);
			if ( len > 2 ) {
				h->reports++;
				knl_usb_lock();
				knl_hid_ext_report(h->hd, in + 2, len - 2);
				knl_usb_unlock();
			}
		} while ( ( h->polled || h->edge ) && len > 2 );	/* more may wait */
		if ( !h->polled ) {
			h->missed = ( er == E_TMOUT && len > 2 ) ? h->missed + 1 : 0;
			if ( h->missed >= HI_MISSED ) {
				h->polled = TRUE;
				tm_printf((UB *)"hidi2c: %s: reports wait with no interrupt %d coming (%d so far): read every %d ms\n",
					  h->name, h->intno, (INT)h->ints, h->period);
			} else {
				unmask(h);
			}
		}
	}
}

/* The interrupt of the device's _CRS taken (an input of the IOAPICs, mode IM_EDGE, IM_LOW): whether it is */
LOCAL BOOL take_int( HIDI2C *h, INT irq, INT mode )
{
	T_CSEM	cs;
	INT	i;
	BOOL	shared = FALSE;

	if ( irq < 0 || irq >= INTVEC_GSI_NUM ) return FALSE;
	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 0x7fffffff;
	h->sem = tk_cre_sem(&cs);
	if ( h->sem <= 0 ) {
		h->sem = 0;
		return FALSE;
	}
	h->intno = INTVEC_GSI(irq);
	h->edge = (BOOL)( ( mode & IM_EDGE ) != 0 );
	for ( i = 0; i < HI_MAX; i++ ) {
		if ( &hi[i] != h && hi[i].used && hi[i].intno == h->intno ) shared = TRUE;
	}
	if ( !shared ) {
		knl_define_inthdr((INT)h->intno, TA_HLNG, (FP)hi_inthdr);
		SetIntMode(h->intno, (UINT)mode);
	}
	return TRUE;
}

/* A report the interrupt tells of, or one read every 10 ms, waited for up to ms: its length, -1 none */
LOCAL INT wait_input( HIDI2C *h, UB *in, INT ms )
{
	INT	t, len;

	if ( h->intno != 0 ) {
		unmask(h);
		if ( tk_wai_sem(h->sem, 1, ms) < E_OK ) return -1;
		return read_input(h, in);
	}
	for ( t = 0; t < ms; t += 10 ) {
		len = read_input(h, in);
		if ( len >= 0 ) return len;
		tk_dly_tsk(10);
	}
	return -1;
}

LOCAL INT attach( T_I2CBUS *bus, UH addr, UH desc, CONST UB *name, INT period, INT irq, INT irq_mode )
{
	HIDI2C	*h = NULL;
	UB	d[30], in[HI_IN_MAX], *rd, mode[64];
	INT	i, len, mlen = 0, mrid = 0;
	T_CTSK	ct;
	ID	t;
	BOOL	kbd = FALSE;
	ER	er;

	for ( i = 0; i < HI_MAX; i++ ) {
		if ( !hi[i].used && hi[i].sem == 0 ) {
			h = &hi[i];
			break;
		}
	}
	if ( h == NULL || bus == NULL ) return E_LIMIT;
	knl_memset(h, 0, sizeof(*h));
	h->bus = bus;
	h->addr = addr;
	h->period = ( period > 0 ) ? period : 8;
	for ( i = 0; i < 15 && name[i] != 0; i++ ) h->name[i] = name[i];
	if ( reg_read(h, desc, d, 30) < E_OK ) {
		tm_printf((UB *)"hidi2c: %s %02x: no answer at register %x\n", bus->name, addr, desc);
		return E_IO;
	}
	if ( ( d[0] | ( d[1] << 8 ) ) != 30 || ( d[2] | ( d[3] << 8 ) ) != 0x0100 ) {
		tm_printf((UB *)"hidi2c: %s %02x: not a HID descriptor (%02x%02x %02x%02x)\n", bus->name, addr,
			  d[1], d[0], d[3], d[2]);
		return E_OBJ;
	}
	h->rdesc_len = (UH)( d[4] | ( d[5] << 8 ) );
	h->rdesc_reg = (UH)( d[6] | ( d[7] << 8 ) );
	h->in_reg = (UH)( d[8] | ( d[9] << 8 ) );
	h->in_max = (UH)( d[10] | ( d[11] << 8 ) );
	h->cmd_reg = (UH)( d[16] | ( d[17] << 8 ) );
	h->data_reg = (UH)( d[18] | ( d[19] << 8 ) );
	h->vid = (UH)( d[20] | ( d[21] << 8 ) );
	h->pid = (UH)( d[22] | ( d[23] << 8 ) );
	if ( h->rdesc_len == 0 || h->rdesc_len > HI_RD_MAX ) return E_OBJ;
	for ( i = 0; i < (INT)( sizeof(quirks) / sizeof(quirks[0]) ); i++ ) {
		if ( quirks[i].vid == h->vid && ( quirks[i].pid == VID_ANY || quirks[i].pid == h->pid ) ) {
			h->quirks |= quirks[i].q;
		}
	}
	h->polled = !take_int(h, irq, irq_mode);

	/* on, reset: the device says it is done with an input of length 0 */
	er = command(h, OP_SET_POWER, PWR_ON);
	if ( er < E_OK ) {
		tk_dly_tsk(1);				/* a device asleep may miss the first */
		er = command(h, OP_SET_POWER, PWR_ON);
	}
	if ( er >= E_OK ) tk_dly_tsk(60);
	(void)command(h, OP_RESET, 0);
	if ( h->quirks & Q_NO_IRQ_AFTER_RESET ) {
		tk_dly_tsk(100);
		len = read_input(h, in);
	} else {
		len = wait_input(h, in, HI_RESET_MS);
	}
	if ( len != 0 ) {
		tm_printf((UB *)"hidi2c: %s %02x: the reset not told (%d)\n", bus->name, addr, len);
	}
	if ( !( h->quirks & Q_NO_WAKEUP_AFTER_RESET ) ) {
		(void)command(h, OP_SET_POWER, PWR_ON);
	}

	rd = (UB *)Kmalloc(h->rdesc_len);
	if ( rd == NULL ) return E_NOMEM;
	if ( reg_read(h, h->rdesc_reg, rd, h->rdesc_len) < E_OK ) {
		Kfree(rd);
		return E_IO;
	}
	/* a keyboard of its own (Generic Desktop, Keyboard) is not taken yet: its reports are not the boot protocol's */
	for ( i = 0; i + 3 < h->rdesc_len; i++ ) {
		if ( rd[i] == 0x05 && rd[i + 1] == 0x01 && rd[i + 2] == 0x09 && rd[i + 3] == 0x06 ) kbd = TRUE;
	}
	h->hd = kbd ? E_NOSPT : knl_hid_ext_attach_dg(rd, h->rdesc_len, name, mode, &mlen, &mrid);
	Kfree(rd);
	if ( h->hd <= 0 ) {
		tm_printf((UB *)"hidi2c: %s %02x %04x:%04x not taken (%d)\n", bus->name, addr, h->vid, h->pid, h->hd);
		return h->hd;
	}

	/* a pen or a touch screen into the mode it is read in, and what it says it is in after */
	if ( mlen > 0 ) {
		INT	skip = ( mrid != 0 ) ? 1 : 0, k, m, got;
		UB	back[2 + 1 + HI_FT_MAX];
		char	line[3 * 16 + 8];

		er = set_report(h, RT_FEATURE, mrid, mode + skip, mlen - skip);
		got = get_report(h, RT_FEATURE, mrid, back, 2 + mlen);
		line[0] = 0;
		for ( k = 2, m = 0; got > 0 && k < got && k < 2 + 16; k++ ) {
			m += tm_sprintf((UB *)line + m, (UB *)" %02x", (INT)back[k]);
		}
		tm_printf((UB *)"hidi2c: %s: input mode: feature report %d (%d bytes) set (%d); read back (%d):%s\n",
			  name, mrid, mlen, (INT)er, got, line);
	}

	h->used = TRUE;
	knl_memset(&ct, 0, sizeof(ct));
	ct.exinf = h;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)hi_task;
	ct.itskpri = 12;
	ct.stksz = 8 * 1024;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
	if ( h->intno != 0 ) unmask(h);
	knl_obinput_sync();			/* its object, when the object layer is up already */
	tm_printf((UB *)"TessronOS: %s: HID over I2C %04x:%04x on %s at %02x, report descriptor %d bytes, input %d, "
		  "quirks %x, %s %d%s\n",
		  name, h->vid, h->pid, bus->name, addr, h->rdesc_len, h->in_max, h->quirks,
		  h->polled ? "read every" : "interrupt", h->polled ? h->period : (INT)h->intno, h->polled ? " ms" : "");
	return h->hd;
}

EXPORT INT knl_hidi2c_attach( T_I2CBUS *bus, UH addr, UH desc, CONST UB *name, INT period )
{
	return attach(bus, addr, desc, name, period, -1, 0);
}

/* 3CDFF6F7-4267-4555-AD05-B30A3D8938DE, as ACPI lays out a UUID */
LOCAL CONST UB	dsm_uuid[16] = {
	0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42, 0x55, 0x45, 0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde
};

EXPORT INT knl_hidi2c_init( void )
{
	INT	i, n = 0, k;
	void	*dev;

	if ( !knl_acpica_ready() ) return 0;
	for ( k = 0; k < 2; k++ ) {
		for ( i = 0; i < 4 && ( dev = knl_acpica_device(( k == 0 ) ? "PNP0C50" : "ACPI0C50", i) ) != NULL; i++ ) {
			char			path[64], ctrl[64], gctrl[64];
			unsigned int		addr = 0, hz = 0;
			int			pin = -1, irq = -1, irq_mode = 0, ps0;
			unsigned long long	reg = 0, adr = 0;
			UB			name[16];
			T_I2CBUS		*bus;

			(void)knl_acpica_path(dev, path, sizeof(path));
			if ( knl_acpica_i2c(dev, &addr, &hz, ctrl, sizeof(ctrl), &pin, gctrl, sizeof(gctrl), &irq,
					    &irq_mode) != 0
			  || knl_acpica_dsm_int(dev, dsm_uuid, 1, 1, &reg) != 0 ) {
				tm_printf((UB *)"hidi2c: %s: no I2C connection or no descriptor register\n", path);
				continue;
			}
			if ( knl_acpica_int(knl_acpica_handle(ctrl), "_ADR", &adr) != 0 ) {
				tm_printf((UB *)"hidi2c: %s: its bus %s has no _ADR\n", path, ctrl);
				continue;
			}
			ps0 = knl_acpica_call(dev, "_PS0");
			tm_printf((UB *)"hidi2c: %s at %02x on %s (%x), %d Hz, descriptor at %x, interrupt %d (%s, active %s), "
				  "GPIO %d of %s, _PS0 %s\n",
				  path, addr, ctrl, (UW)adr, hz, (UW)reg, irq, ( irq_mode & IM_EDGE ) ? "edge" : "level",
				  ( irq_mode & IM_LOW ) ? "low" : "high", pin, gctrl,
				  ( ps0 == 0 ) ? "run" : ( ps0 == -1 ? "none" : "failed" ));
			bus = knl_dwi2c_bus((UW)adr, knl_acpica_handle(ctrl), hz);
			if ( bus == NULL ) continue;
			(void)tm_sprintf(name, (UB *)"hidi2c%d", n);
			if ( attach(bus, (UH)addr, (UH)reg, name, 8, irq, irq_mode) > 0 ) n++;
		}
	}
	return n;
}

#endif /* X64 */
