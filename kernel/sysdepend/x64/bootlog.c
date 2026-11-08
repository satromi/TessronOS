/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bootlog.c (x64 PC)
 *	The console kept in memory and written to files on the EFI system
 *	partition, for a machine without a serial port: everything the
 *	console says goes into a ring of 512KB (knl_bootlog_put, from
 *	fbcon.c, whatever the screen shows), and a task writes, under
 *	/boot/EFI/TESSRON/,
 *
 *	  KERNEL.TXT	the console from the start (the newest 512KB of it)
 *	  USB.TXT	each controller (its PCI identity, the Intel port
 *			routing and dual role registers, every root port's
 *			PORTSC read out: connected, enabled, powered, the
 *			link state and the speed), then the USB devices the
 *			kernel brought up with their speed and how many hubs
 *			deep they are, and every configuration of each as
 *			its descriptors say (the vendor and product, the
 *			interfaces' classes, the endpoints) and the
 *			descriptors themselves
 *	  ACPI.TXT	the devices of the ACPI namespace, as the interpreter
 *			found them (paths, IDs, status, resources)
 *	  DISPLAY.TXT	Intel's display engine as the firmware left it: clocks,
 *			PLLs, transcoders and their timings, pipes, scalers,
 *			planes, panel power, backlight, the start of the GTT,
 *			stolen memory and the OpRegion (gfxdump.c; only read)
 *
 *	The same files go to TESSRON of the records' partition TSLOG of the
 *	disk started from, mounted on /tslog (tools/mksd.py --log-mb), which
 *	Windows shows as a drive even on a removable disk.
 *
 *	20 seconds after it starts, and every minute after that, but only
 *	when the console has said something since the last writing;
 *	KERNEL.TXT and USB.TXT together, ACPI.TXT once, DISPLAY.TXT when it
 *	changed, so that the USB disk is not kept busy. Something worth
 *	keeping (the screen's size changed, a network not joined) has them
 *	written two seconds after (knl_bootlog_soon); the machine going off
 *	or starting again has them written last, before it goes
 *	(knl_bootlog_final). What stops the machine (an exception in the
 *	kernel, a task past its stack, a lockup) asks for them at once
 *	(knl_bootlog_stopped, from any context): the processor that stopped
 *	does not write, it halts, but this task on the others does within a
 *	quarter of a second, while the disk still answers; when it does not,
 *	the writing simply does not end. The disk started from taken out
 *	and put back is mounted again on /boot and /tslog before a writing
 *	(boot_again), so that what the console said while it was out, kept
 *	in the ring all along, is written after all. With the loader's
 *	LOADER.TXT and HW.TXT beside them, one start of a machine tells what
 *	it is and how far it got.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/fs.h>
#include <ts/usb.h>

#define LOG_SZ		( 512 * 1024 )
#define HEAD_SZ		( 256 * 1024 )	/* the start of the console, kept whatever follows */
#define TXT_SZ		( 256 * 1024 )
#define DIR		"/boot/EFI/TESSRON"
#define LOGDIR		"/tslog/TESSRON"

LOCAL UB	ring[LOG_SZ];
LOCAL UB	head[HEAD_SZ];
LOCAL UD	ring_head = 0;		/* bytes said since the start */

IMPORT unsigned char	knl_bootdisk_note[];
IMPORT void		knl_exc_stop_all( void );	/* exc_hdl.c */
IMPORT int		knl_acpica_report( char *buf, int max );	/* acpica.c */
IMPORT INT		knl_gfx_report( char *buf, INT max );		/* gfxdump.c */

#define ACPI_TXT_SZ	( 512 * 1024 )

/* What the console says, kept (any context: no lock, a race costs a character) */
EXPORT void knl_bootlog_put( const UB *buf, INT size )
{
	UD	h = ring_head;

	while ( size-- > 0 ) {
		if ( h < HEAD_SZ ) head[h] = *buf;
		ring[h % LOG_SZ] = *buf++;
		h++;
	}
	ring_head = h;
}

/* Where the console has got to: what knl_bootlog_tail takes */
EXPORT UD knl_bootlog_head( void )
{
	return ring_head;
}

/*
 * The last lines the console said before 'until', handed to out a line
 * at a time (the screen console shows them under the report of what
 * stopped the machine, which clears the screen)
 */
EXPORT void knl_bootlog_tail( UD until, INT lines, void (*out)( CONST UB *s, INT n ) )
{
	UD	h = ( until < ring_head ) ? until : ring_head, lo = ( ring_head > LOG_SZ ) ? ring_head - LOG_SZ : 0, p = h;
	INT	n = 0;
	UB	line[200];

	while ( p > lo && n <= lines ) {
		p--;
		if ( ring[p % LOG_SZ] == 0x0a ) n++;
	}
	while ( p < h ) {
		INT	k = 0;

		while ( p < h && ring[p % LOG_SZ] != 0x0a ) {
			if ( k < (INT)sizeof(line) ) line[k++] = ring[p % LOG_SZ];
			p++;
		}
		p++;
		if ( k > 0 ) out(line, k);
	}
}

/* ------------------------------------------------------------------ text */

typedef struct {
	UB	*p;
	INT	len, cap;
} TXT;

/* One line formatted into the text (only the writing task formats, so one buffer does) */
LOCAL UB	txb[512];

LOCAL void tx_add( TXT *t, INT n )
{
	INT	i;

	for ( i = 0; i < n && t->len < t->cap - 1; i++ ) t->p[t->len++] = txb[i];
}

#define tx(t, ...)	tx_add((t), tm_sprintf(txb, (CONST UB *)__VA_ARGS__))

IMPORT unsigned char	knl_logvol_name[];	/* bootdisk.c: "" when the disk has no TSLOG */
IMPORT unsigned char	knl_bootvol_name[];
IMPORT BOOL		knl_bootdisk_again( void );	/* bootdisk.c */

/*
 * The disk started from taken out and put back: /boot and /tslog mounted
 * again on it, wherever it is now
 */
LOCAL void boot_again( void )
{
	ER	er;

	if ( knl_bootvol_name[0] == '\0' || knl_fs_mount_of((CONST char *)knl_bootvol_name, NULL) ) {
		return;				/* never mounted, or there as it was */
	}
	if ( !knl_bootdisk_again() ) {
		return;				/* still out */
	}
	if ( knl_logvol_name[0] != '\0'
	  && fs_attach("fatfs", (CONST char *)knl_logvol_name, "/tslog", 0) < E_OK ) {
		knl_logvol_name[0] = '\0';
	}
	er = fs_attach("fatfs", (CONST char *)knl_bootvol_name, "/boot", 0);
	tm_printf((UB *)"bootlog: /boot on %s again (%d)\n", (CONST char *)knl_bootvol_name, (INT)er);
}

LOCAL ER put_one( CONST char *dir, CONST char *name, CONST UB *a, INT alen, CONST UB *b, INT blen )
{
	char	path[64];
	INT	fd;
	ER	er = E_OK;

	tm_sprintf((UB *)path, (CONST UB *)"%s/%s", dir, name);
	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		return (ER)fd;
	}
	if ( alen > 0 && fs_write(fd, a, alen) != alen ) er = E_IO;
	if ( er >= E_OK && blen > 0 && fs_write(fd, b, blen) != blen ) er = E_IO;
	fs_close(fd);
	return er;
}

/* A record written to /boot/EFI/TESSRON, and to TESSRON of the records' partition when there is one */
LOCAL ER put_file( CONST char *name, CONST UB *a, INT alen, CONST UB *b, INT blen )
{
	ER	er = put_one(DIR, name, a, alen, b, blen);

	if ( knl_logvol_name[0] != '\0' ) {
		(void)put_one(LOGDIR, name, a, alen, b, blen);
	}
	return er;
}

/* ------------------------------------------------------------------ the files */

/*
 * The console as KERNEL.TXT: its start (HEAD_SZ) and its last LOG_SZ, with a
 * line saying what was left out between them, so that the start up stays in
 * the record however much is said later
 */
LOCAL ER write_kernel( TXT *t, UW secs )
{
	UD	h = ring_head, from = ( h > LOG_SZ ) ? h - LOG_SZ : 0;
	UD	hn = ( h < HEAD_SZ ) ? h : HEAD_SZ;
	UB	*lin;
	INT	n, k, i;
	ER	er;

	if ( from < hn ) from = hn;
	t->len = 0;
	tx(t, "TessronOS x64: the console, %d s after the start; %d bytes said, the first %d and the last %d here\n",
	   secs, (INT)h, (INT)hn, (INT)( h - from ));
	tx(t, "boot disk: %s\n\n", knl_bootdisk_note);
	lin = (UB *)Kmalloc((SZ)( hn + ( h - from ) + 128 ));
	if ( lin == NULL ) {
		return E_NOMEM;
	}
	for ( n = 0; n < (INT)hn; n++ ) lin[n] = head[n];
	if ( from > hn ) {
		k = (INT)tm_sprintf(txb, (CONST UB *)"\n[... %d bytes left out ...]\n\n", (INT)( from - hn ));
		for ( i = 0; i < k; i++ ) lin[n++] = txb[i];
	}
	for ( ; from < h; from++ ) lin[n++] = ring[from % LOG_SZ];
	er = put_file("KERNEL.TXT", t->p, t->len, lin, n);
	Kfree(lin);
	return er;
}

LOCAL CONST char *speed_name( UINT s )
{
	switch ( s ) {
	case USB_SPEED_LOW:		return "low";
	case USB_SPEED_FULL:		return "full";
	case USB_SPEED_HIGH:		return "high";
	case USB_SPEED_SUPER:		return "super";
	case USB_SPEED_SUPER_PLUS:	return "super+";
	default:			return "?";
	}
}

IMPORT INT knl_xhci_pc_ports( INT hcidx, UW *portsc, UB *major, INT max, UH *version );
IMPORT INT knl_xhci_pc_regs( INT hcidx, UW *w, INT max );

/* the speed field of PORTSC, as the default speed IDs of xHCI have it */
LOCAL CONST char *port_speed( UW v )
{
	switch ( (v >> 10) & 0x0f ) {
	case 1:		return "full";
	case 2:		return "low";
	case 3:		return "high";
	case 4:		return "super";
	case 5:		return "super+";
	default:	return "-";
	}
}

/* the link state field of PORTSC */
LOCAL CONST char *port_link( UW v )
{
	static CONST char * CONST n[16] = {
		"U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive", "Polling",
		"Recovery", "HotReset", "Compliance", "TestMode", "12", "13", "14", "Resume"
	};

	return n[(v >> 5) & 0x0f];
}

LOCAL void controllers_text( TXT *t )
{
	UW	w[11], ps[32];
	UB	mj[32];
	UH	ver;
	INT	k, n, i;

	for ( k = 0; k < ts_usb_hc_count(); k++ ) {
		n = knl_xhci_pc_ports(k, ps, mj, 32, &ver);
		if ( n < 0 ) {
			tx(t, "controller %d: not an xHCI\n", k);
			continue;
		}
		tx(t, "controller %d: xHCI %x.%02x, %d root ports", k, ver >> 8, ver & 0xff, n);
		if ( knl_xhci_pc_regs(k, w, 11) == 11 ) {
			tx(t, ", PCI %04x:%04x at %02x:%02x.%d", w[0] >> 16, w[0] & 0xffff,
			   w[1] >> 16, (w[1] >> 8) & 0xff, w[1] & 0xff);
			if ( (w[2] & 16) != 0 ) tx(t, ", an interrupt for every isochronous TD");
			tx(t, "\n");
			if ( (w[2] & 1) != 0 ) {
				tx(t, "  Intel port routing: XUSB2PR %08x XUSB2PRM %08x USB3_PSSEN %08x USB3PRM %08x%s\n",
				   w[3], w[4], w[5], w[6], ( (w[2] & 2) != 0 ) ? ", taken from the EHCI" : "");
			}
			if ( (w[2] & 4) != 0 ) {
				tx(t, "  Intel dual role: CFG0 %08x CFG1 %08x as found, %08x %08x now%s\n",
				   w[7], w[8], w[9], w[10], ( (w[2] & 8) != 0 ) ? ", switched to the host role" : "");
			}
		} else {
			tx(t, "\n");
		}
		for ( i = 0; i < n && i < 32; i++ ) {
			UW	v = ps[i];

			tx(t, "  port %d (USB %d): PORTSC %08x%s%s%s link %s speed %s%s\n", i, mj[i], v,
			   ( (v & 0x001) != 0 ) ? " connected" : "", ( (v & 0x002) != 0 ) ? " enabled" : "",
			   ( (v & 0x200) != 0 ) ? " powered" : " unpowered", port_link(v),
			   ( (v & 0x001) != 0 ) ? port_speed(v) : "-",
			   ( (v & 0x00fe0000) != 0 ) ? " (changes pending)" : "");
		}
	}
}

LOCAL void config_text( TXT *t, CONST UB *c, INT total )
{
	INT	off = 0, i;

	while ( off + 2 <= total && c[off] >= 2 && off + c[off] <= total ) {
		CONST UB	*d = c + off;

		switch ( d[1] ) {
		case 2:
			tx(t, "    configuration %d: %d interfaces, attributes %02x, %d mA\n", d[5], d[4], d[7], d[8] * 2);
			break;
		case 4:
			tx(t, "      interface %d alt %d: class %02x.%02x.%02x, %d endpoints\n", d[2], d[3], d[5], d[6], d[7], d[4]);
			break;
		case 5:
			tx(t, "        endpoint %02x: %s, %d bytes, interval %d\n", d[2],
			   ( ( d[3] & 3 ) == 0 ) ? "control" : ( ( d[3] & 3 ) == 1 ) ? "isochronous"
			   : ( ( d[3] & 3 ) == 2 ) ? "bulk" : "interrupt", ( d[4] | ( d[5] << 8 ) ) & 0x7ff, d[6]);
			break;
		case 0x0b:
			tx(t, "      association: interfaces %d-%d class %02x.%02x\n", d[2], d[2] + d[3] - 1, d[4], d[5]);
			break;
		case 0x24:
			tx(t, "        class-specific %02x, %d bytes\n", d[2], d[0]);
			break;
		default:
			break;
		}
		off += d[0];
	}
	for ( i = 0; i < total; i++ ) {
		if ( ( i & 15 ) == 0 ) tx(t, "    %04x:", i);
		tx(t, " %02x", c[i]);
		if ( ( i & 15 ) == 15 || i + 1 == total ) tx(t, "\n");
	}
}

IMPORT INT	knl_msc_report( char *buf, INT max ) __attribute__((weak));	/* device/usb/msc.c */

LOCAL ER write_usb( TXT *t, UW secs )
{
	T_USBDEV	*l;
	T_USBSTAT	st;
	UB		*buf, dd[18];
	INT		n, i, k;

	t->len = 0;
	l = (T_USBDEV *)Kmalloc(sizeof(T_USBDEV) * 64);
	buf = (UB *)Kmalloc(4096);
	if ( l == NULL || buf == NULL ) {
		if ( l != NULL ) Kfree(l);
		if ( buf != NULL ) Kfree(buf);
		return E_NOMEM;
	}
	n = ts_usb_lst_dev(l, 64);
	tx(t, "TessronOS x64: the USB devices %d s after the start: %d controllers, %d devices\n",
	   secs, ts_usb_hc_count(), n);
	if ( ts_usb_stat(&st) >= E_OK ) {
		tx(t, "  attached %d, detached %d, failed to come up %d, command timeouts %d\n",
		   st.attached, st.detached, st.enum_failed, st.cmd_timeouts);
	}
	if ( knl_msc_report != NULL ) {
		INT	m = knl_msc_report((char *)buf, 4096);

		for ( k = 0; k < m && t->len < t->cap - 1; k++ ) t->p[t->len++] = buf[k];
	}
	controllers_text(t);
	for ( i = 0; i < n && i < 64; i++ ) {
		T_USBDEV	*d = &l[i];

		tx(t, "\ndevice %d: %04x:%04x release %04x class %02x.%02x.%02x \"%s\", %s speed, controller %d"
		   " root port %d port %d depth %d (hubs above it) route %x, role %d%s%s\n", d->dev, d->vendor, d->product,
		   d->release, d->dev_class, d->subclass, d->protocol, d->name, speed_name(d->speed), d->hc,
		   d->root_port, d->port, d->depth, d->route, d->role, ( d->devnm[0] != 0 ) ? ", disk " : "",
		   ( d->devnm[0] != 0 ) ? (char *)d->devnm : "");
		if ( ts_usb_get_desc(d->dev, 1, 0, dd, 18) >= 18 ) {
			for ( k = 0; k < dd[17] && k < 8; k++ ) {
				INT	got = ts_usb_get_desc(d->dev, 2, k, buf, 9), total;

				if ( got < 9 ) {
					tx(t, "  configuration index %d: not read (%d)\n", k, got);
					continue;
				}
				total = buf[2] | ( buf[3] << 8 );
				if ( total > 4096 ) total = 4096;
				got = ts_usb_get_desc(d->dev, 2, k, buf, total);
				if ( got < 9 ) {
					tx(t, "  configuration index %d: %d bytes not read (%d)\n", k, total, got);
					continue;
				}
				tx(t, "  configuration index %d (%d bytes):\n", k, got);
				config_text(t, buf, got);
			}
		}
	}
	Kfree(l);
	Kfree(buf);
	return put_file("USB.TXT", t->p, t->len, NULL, 0);
}

/*
 * One writing of the records, by one task at a time (the bootlog task,
 * or the one turning the machine off): KERNEL.TXT and USB.TXT together;
 * ACPI.TXT the first time only (the namespace does not change after
 * the start); DISPLAY.TXT when what it says has changed. Nothing is
 * written when the console has said nothing since the last time.
 */
LOCAL ID	w_sem = 0;
LOCAL UD	w_head = 0;		/* the console as far as the last writing had it */
LOCAL BOOL	w_acpi = FALSE;		/* ACPI.TXT written */
LOCAL UW	w_disp_sum = 0;		/* what DISPLAY.TXT last held, as a sum of its bytes */
LOCAL INT	w_disp_len = -1;
LOCAL BOOL	w_kept = FALSE;		/* the last start's KERNEL.TXT kept as KERNEL1.TXT */

/*
 * The KERNEL.TXT of the start before, kept once as KERNEL1.TXT before this
 * start writes its own, so that a start that went wrong can still be read
 * after the next one
 */
LOCAL void keep_last( CONST char *dir )
{
	char	from[64], to[64];

	tm_sprintf((UB *)from, (CONST UB *)"%s/KERNEL.TXT", dir);
	tm_sprintf((UB *)to, (CONST UB *)"%s/KERNEL1.TXT", dir);
	(void)fs_unlink(to);
	(void)fs_rename(from, to);
}

LOCAL void write_all( UW secs, BOOL always, TMO tmo )
{
	TXT	t;
	ER	a, b;

	if ( w_sem > 0 && tk_wai_sem(w_sem, 1, tmo) < E_OK ) {
		tm_printf((UB *)"bootlog: the records are being written by another: not again now\n");
		return;
	}
	if ( !always && ring_head == w_head ) {
		if ( w_sem > 0 ) tk_sig_sem(w_sem, 1);
		return;				/* nothing new */
	}
	t.cap = TXT_SZ;
	t.len = 0;
	t.p = (UB *)Kmalloc(TXT_SZ);
	if ( t.p == NULL ) {
		if ( w_sem > 0 ) tk_sig_sem(w_sem, 1);
		return;
	}
	boot_again();
	(void)fs_mkdir("/boot/EFI");
	if ( knl_logvol_name[0] != '\0' ) (void)fs_mkdir(LOGDIR);
	(void)fs_mkdir(DIR);
	if ( !w_kept ) {
		keep_last(DIR);
		if ( knl_logvol_name[0] != '\0' ) keep_last(LOGDIR);
		w_kept = TRUE;
	}
	b = write_usb(&t, secs);
	{
		char	*x = (char *)Kmalloc(ACPI_TXT_SZ);

		if ( x != NULL ) {
			INT	n, i;
			UW	sum = 0;

			if ( !w_acpi ) {
				n = knl_acpica_report(x, ACPI_TXT_SZ);
				if ( put_file("ACPI.TXT", (CONST UB *)x, n, NULL, 0) >= E_OK ) w_acpi = TRUE;
			}
			n = knl_gfx_report(x, 64 * 1024);
			for ( i = 0; i < n; i++ ) sum = sum * 31 + (UB)x[i];
			if ( n > 0 && ( n != w_disp_len || sum != w_disp_sum ) ) {
				if ( put_file("DISPLAY.TXT", (CONST UB *)x, n, NULL, 0) >= E_OK ) {
					w_disp_len = n;
					w_disp_sum = sum;
				}
			}
			Kfree(x);
		}
	}
	a = write_kernel(&t, secs);
	fs_sync();
	Kfree(t.p);
	tm_printf((UB *)"bootlog: %s/KERNEL.TXT %s, USB.TXT %s (%d s)\n", DIR,
		  ( a >= E_OK ) ? "written" : "NOT written", ( b >= E_OK ) ? "written" : "NOT written", secs);
	w_head = ring_head;			/* its own line is not something new */
	if ( w_sem > 0 ) tk_sig_sem(w_sem, 1);
}

/*
 * Something worth keeping happened (the screen's size changed, a
 * network could not be joined): the files are written again two seconds
 * from now, so that what comes with it is in, besides when they are due.
 * Asked again before then, it is still one writing.
 */
LOCAL volatile UW	soon_at = 0;
LOCAL volatile UW	secs_now = 0;
LOCAL BOOL		started = FALSE;

EXPORT void knl_bootlog_soon( void )
{
	if ( soon_at == 0 ) soon_at = secs_now + 2;
}

/* The machine stopped (an exception, a lockup): written now by the task, once (any context, no lock) */
LOCAL volatile BOOL		stop_req = FALSE;
LOCAL CONST char *volatile	stop_why = NULL;

EXPORT void knl_bootlog_stopped( CONST char *why )
{
	if ( stop_req ) return;
	stop_why = why;
	stop_req = TRUE;
}

LOCAL void bootlog_task( INT stacd, void *exinf )
{
	UW	secs = 0, next = 20, quarter = 0;
	BOOL	stop_done = FALSE;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		tk_dly_tsk(250);
		if ( stop_req && !stop_done ) {
			/* the machine stopped: written now, whatever was written before */
			stop_done = TRUE;
			tm_printf((UB *)"bootlog: the machine %s: the records written now\n", stop_why);
			write_all(secs, TRUE, 3000);
			knl_exc_stop_all();	/* an exception that waited for them: the machine stops now */
			continue;
		}
		if ( ++quarter < 4 ) continue;
		quarter = 0;
		secs++;
		secs_now = secs;
		if ( secs >= next || ( soon_at != 0 && secs >= soon_at ) ) {
			soon_at = 0;
			write_all(secs, FALSE, TMO_FEVR);
			next = secs + 60;	/* every minute from then on */
		}
	}
}

/*
 * The machine going off or starting again (knl_finish_device): the
 * records written now, whatever was last written, by the task turning it
 * off, while the disks and the interrupts still work
 */
EXPORT void knl_bootlog_final( CONST char *why )
{
	if ( !started ) return;
	/*
	 * A disk that went and came back has to be looked over before it is
	 * mounted again, for as long as tens of seconds on a card, and every
	 * file call waits for that: it is left for the next writing of the
	 * bootlog task, not done on the way to the machine going off or asleep.
	 */
	if ( knl_bootvol_name[0] != '\0' && !knl_fs_mount_of((CONST char *)knl_bootvol_name, NULL) ) {
		tm_printf((UB *)"bootlog: the machine %s: /boot is not mounted, these records not written\n", why);
		return;
	}
	tm_printf((UB *)"bootlog: the machine %s: the records written last\n", why);
	write_all(secs_now, TRUE, TMO_FEVR);
}

/*
 * After the last writing, the machine going off or starting again: /boot
 * and /tslog unmounted, which takes the in-use mark off them, so that the
 * next start does not look them over. The bootlog task stays out: w_sem is
 * not given back, and a writing that does not end in time leaves them
 * marked, as a machine that lost its power does.
 */
EXPORT void knl_bootlog_unmount( void )
{
	ER	a, b = E_OK;

	if ( !started ) return;
	if ( w_sem > 0 && tk_wai_sem(w_sem, 1, 2000) < E_OK ) return;
	if ( knl_logvol_name[0] != '\0' ) b = fs_detach("/tslog");
	a = fs_detach("/boot");
	tm_printf((UB *)"bootlog: /boot unmounted (%d), /tslog (%d)\n", (INT)a, (INT)b);
}

/* Started once /boot is mounted */
EXPORT void knl_bootlog_start( void )
{
	T_CTSK	c;
	T_CSEM	cs;
	ID	t;

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 1;
	cs.maxsem = 1;
	w_sem = tk_cre_sem(&cs);
	started = TRUE;
	knl_memset(&c, 0, sizeof(c));
	c.tskatr = TA_HLNG | TA_RNG0;
	c.task = (FP)bootlog_task;
	c.itskpri = 30;		/* low: the files wait for what else there is to do */
	c.stksz = 16 * 1024;
	t = tk_cre_tsk(&c);
	if ( t > 0 ) {
		tk_sta_tsk(t, 0);
	}
}

#endif /* X64 */
