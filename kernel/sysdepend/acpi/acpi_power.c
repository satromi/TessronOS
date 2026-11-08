/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpi_power.c
 *	What the firmware tells of power, through the ACPI interpreter, as
 *	device objects (OB_S_POWER, design 18.7): each battery (PNP0C0A)
 *	"battery0", "battery1", the AC adapter (ACPI0003) "acpower", the lid
 *	(PNP0C0D) "lid", the power button (PNP0C0C, the fixed one of the
 *	FADT, the Surface's MSHW0028/MSHW0040) "powerbutton". The volume and
 *	power buttons of a tablet's HID event filter (INT33D5) are taken
 *	here too, as keys and as the power button (the 5-button array).
 *
 *	Record 1 is the state as "key value" lines:
 *	  a battery	PRESENT, STATE (discharging, charging, full,
 *			notcharging), CRITICAL 1 when the firmware says the
 *			charge is critically low, PERCENT, REMAINING, FULL,
 *			DESIGN (mWh, or mAh with UNIT mAh), RATE (mW or mA),
 *			VOLTAGE (mV), TECHNOLOGY, MODEL, SERIAL, TYPE, OEM
 *	  the adapter	ONLINE 1 or 0
 *	  the lid	OPEN 1 or 0
 *	  the button	PRESSES, how many times since the start
 *	read again from the firmware's methods (_BIX or _BIF, _BST, _PSR,
 *	_LID) when the firmware notifies the device, and a battery every 30
 *	seconds besides, for the machines whose firmware tells nothing; a
 *	change is told as OB_E_CHANGE on record 1. A notification of the
 *	adapter has the batteries read again too (they start or stop
 *	charging with it), and a battery's status notification (0x80) the
 *	adapter (firmware that tells a plug pulled only through the battery).
 *	The notifications come in the interpreter's task for them (osl.c),
 *	not in the SCI's handler, so the methods are run and the change
 *	posted there. システム環境設定's 〈電源管理〉 reads these records.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/hid.h>
#include "obj/obj.h"
#include "acpi.h"

#define PW_MAX		6
#define PW_TEXT		512
#define BAT_POLL_MS	30000

#define K_BATTERY	1
#define K_AC		2
#define K_LID		3
#define K_BUTTON	4

typedef struct {
	BOOL	used;
	INT	kind;
	void	*dev;			/* the interpreter's handle of the device */
	UB	name[OB_DV_NAME];
	UB	path[64];
	TS_UUID	uuid;
	UB	text[PW_TEXT];
	INT	tlen;
	UW	presses;
} PWDEV;

LOCAL PWDEV	pw[PW_MAX];
LOCAL ID	pw_mtx = 0, pw_flg = 0;

#define LOCK()		tk_loc_mtx(pw_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(pw_mtx)

/* ---------------------------------------------------------------- the state */

LOCAL INT line( UB *t, INT n, CONST char *k, CONST char *v )
{
	n = knl_oj_put(t, n, PW_TEXT, k);
	n = knl_oj_put(t, n, PW_TEXT, " ");
	n = knl_oj_put(t, n, PW_TEXT, v);
	return knl_oj_put(t, n, PW_TEXT, "\n");
}

LOCAL INT line_num( UB *t, INT n, CONST char *k, D v )
{
	n = knl_oj_put(t, n, PW_TEXT, k);
	n = knl_oj_put(t, n, PW_TEXT, " ");
	n = knl_oj_put_num(t, n, PW_TEXT, v);
	return knl_oj_put(t, n, PW_TEXT, "\n");
}

/* An unknown value of ACPI's (0xFFFFFFFF) as -1 */
LOCAL D val( unsigned long long v )
{
	return ( v == 0xffffffffULL ) ? -1 : (D)v;
}

/* A battery's state from _BIX (or _BIF) and _BST */
LOCAL INT battery_text( PWDEV *p, UB *t )
{
	unsigned long long	v[24], sta = 0xf;
	char			s[24][32];
	INT			n = 0, k, bix, design, full, rem, state;

	if ( knl_acpica_int(p->dev, "_STA", &sta) == 0 && ( sta & 0x10 ) == 0 ) {
		return line(t, 0, "PRESENT", "0");
	}
	n = line(t, n, "PRESENT", "1");
	bix = 1;
	k = knl_acpica_pkg(p->dev, "_BIX", v, s, 24);
	if ( k < 20 ) {
		bix = 0;
		k = knl_acpica_pkg(p->dev, "_BIF", v, s, 24);
	}
	if ( k < 13 ) {
		return line(t, n, "INFO", "none");
	}
	/* _BIX has its revision first; the rest is _BIF's, with more between */
	{
		INT	o = bix ? 1 : 0;

		design = (INT)val(v[o + 1]);
		full = (INT)val(v[o + 2]);
		n = line(t, n, "UNIT", ( v[o] == 0 ) ? "mWh" : "mAh");
		n = line_num(t, n, "DESIGN", design);
		n = line_num(t, n, "FULL", full);
		n = line(t, n, "TECHNOLOGY", ( v[o + 3] == 1 ) ? "rechargeable" : "primary");
		n = line_num(t, n, "DESIGN_VOLTAGE", val(v[o + 4]));
		k = bix ? 16 : 9;
		n = line(t, n, "MODEL", s[k]);
		n = line(t, n, "SERIAL", s[k + 1]);
		n = line(t, n, "TYPE", s[k + 2]);
		n = line(t, n, "OEM", s[k + 3]);
	}
	if ( knl_acpica_pkg(p->dev, "_BST", v, s, 4) >= 4 ) {
		state = (INT)v[0];
		rem = (INT)val(v[2]);
		/*
		 * Neither charging nor discharging is full only when the
		 * remaining capacity has reached the last full charge (or the
		 * design capacity); short of it the charger is holding back
		 * (a charge threshold, a battery too warm): notcharging
		 */
		n = line(t, n, "STATE", ( state & 1 ) ? "discharging" : ( state & 2 ) ? "charging"
			 : ( rem > 0 && ( rem == full || ( design > 0 && rem >= design ) ) ) ? "full"
			 : "notcharging");
		if ( state & 4 ) {
			n = line(t, n, "CRITICAL", "1");
		}
		n = line_num(t, n, "RATE", val(v[1]));
		n = line_num(t, n, "REMAINING", rem);
		n = line_num(t, n, "VOLTAGE", val(v[3]));
		if ( full <= 0 ) full = design;		/* the last full charge unknown: as designed */
		if ( full > 0 && rem >= 0 ) {
			D	pc = (D)rem * 100 / full;

			n = line_num(t, n, "PERCENT", ( pc > 100 ) ? 100 : pc);
		}
	}
	return n;
}

/* Record 1 made again; TRUE when it changed */
LOCAL BOOL refresh( PWDEV *p )
{
	UB			t[PW_TEXT];
	unsigned long long	v = 0;
	INT			n = 0, k;
	BOOL			changed;

	switch ( p->kind ) {
	case K_BATTERY:
		n = battery_text(p, t);
		break;
	case K_AC:
		n = line(t, 0, "ONLINE", ( knl_acpica_int(p->dev, "_PSR", &v) == 0 && v != 0 ) ? "1" : "0");
		break;
	case K_LID:
		n = line(t, 0, "OPEN", ( knl_acpica_int(p->dev, "_LID", &v) != 0 || v != 0 ) ? "1" : "0");
		break;
	case K_BUTTON:
		n = line_num(t, 0, "PRESSES", p->presses);
		break;
	}
	if ( n < 0 ) n = 0;
	LOCK();
	changed = (BOOL)( n != p->tlen );
	for ( k = 0; !changed && k < n; k++ ) changed = (BOOL)( t[k] != p->text[k] );
	knl_memcpy(p->text, t, n);
	p->tlen = n;
	UNLOCK();
	return changed;
}

LOCAL void tell( PWDEV *p )
{
	if ( refresh(p) ) {
		knl_obdev_post(&p->uuid, OB_PW_STATE, OB_E_CHANGE, NULL);
	}
}

/* ---------------------------------------------------------------- the objects */

LOCAL INT pw_text( void *ctx, UB *t, INT n, INT max )
{
	PWDEV	*p = (PWDEV *)ctx;

	n = knl_oj_put(t, n, max, "<p>");
	n = knl_oj_put(t, n, max, ( p->kind == K_BATTERY ) ? "A battery"
			: ( p->kind == K_AC ) ? "The AC adapter"
			: ( p->kind == K_LID ) ? "The lid" : "The power button");
	n = knl_oj_put(t, n, max, ", as the firmware's ACPI tells of it (");
	n = knl_oj_put(t, n, max, (CONST char *)p->path);
	n = knl_oj_put(t, n, max, "). Record 1: its state as key value lines; a change is told on it.</p>");
	return n;
}

LOCAL INT pw_attr( void *ctx, UB *j, INT n, INT max )
{
	PWDEV	*p = (PWDEV *)ctx;

	n = knl_oj_put(j, n, max, ",\"acpi\":");
	return knl_oj_put_str(j, n, max, p->path);
}

LOCAL ER pw_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	PWDEV	*p = (PWDEV *)ctx;
	SZ	k;

	(void)pos;
	if ( recno != OB_PW_STATE ) {
		return E_NOEXS;
	}
	LOCK();
	k = ( off < p->tlen ) ? p->tlen - off : 0;
	if ( size >= 0 && k > size ) k = size;
	if ( size > 0 && k > 0 ) knl_memcpy(buf, p->text + off, (INT)k);
	UNLOCK();
	if ( p_asize != NULL ) *p_asize = ( size == 0 ) ? p->tlen : k;
	return E_OK;
}

LOCAL UD pw_size( void *ctx, INT recno )
{
	return ( recno == OB_PW_STATE ) ? (UD)( (PWDEV *)ctx )->tlen : 0;
}

LOCAL CONST T_OBDVOPS bat_ops = { OB_S_POWER, "battery", OB_PW_NREC, 0644, 0, 0, pw_text, pw_attr, pw_rea, NULL, pw_size, NULL };
LOCAL CONST T_OBDVOPS ac_ops  = { OB_S_POWER, "ac", OB_PW_NREC, 0644, 0, 0, pw_text, pw_attr, pw_rea, NULL, pw_size, NULL };
LOCAL CONST T_OBDVOPS lid_ops = { OB_S_POWER, "lid", OB_PW_NREC, 0644, 0, 0, pw_text, pw_attr, pw_rea, NULL, pw_size, NULL };
LOCAL CONST T_OBDVOPS btn_ops = { OB_S_POWER, "button", OB_PW_NREC, 0644, 0, 0, pw_text, pw_attr, pw_rea, NULL, pw_size, NULL };

LOCAL void add( INT kind, void *dev, CONST char *name )
{
	PWDEV	*p = NULL;
	INT	i;
	ER	er;

	for ( i = 0; i < PW_MAX; i++ ) {
		if ( !pw[i].used ) {
			p = &pw[i];
			break;
		}
	}
	if ( p == NULL ) return;
	knl_memset(p, 0, sizeof(*p));
	p->kind = kind;
	p->dev = dev;
	(void)knl_oj_put(p->name, 0, OB_DV_NAME, name);
	if ( dev == NULL || knl_acpica_path(dev, (char *)p->path, sizeof(p->path)) != 0 ) {
		(void)knl_oj_put(p->path, 0, sizeof(p->path), "the FADT's fixed power button");
	}
	p->used = TRUE;
	(void)refresh(p);
	er = knl_obdev_add(p->name, NULL, ( kind == K_BATTERY ) ? &bat_ops : ( kind == K_AC ) ? &ac_ops
			   : ( kind == K_LID ) ? &lid_ops : &btn_ops, p, &p->uuid);
	if ( er < E_OK ) {
		p->used = FALSE;
		tm_printf((UB *)"acpi: %s not made a device object (%d)\n", name, (INT)er);
		return;
	}
	{
		UB	one[PW_TEXT];

		for ( i = 0; i < p->tlen && i < PW_TEXT - 1; i++ ) {
			one[i] = ( p->text[i] == '\n' ) ? ',' : p->text[i];
		}
		one[i] = 0;
		tm_printf((UB *)"acpi: %s (%s): %s\n", name, p->path, one);
	}
}

/* ---------------------------------------------------------------- the 5-button array */

/*
 * A tablet's volume and power buttons, as the firmware's HID event
 * filter (INT33D5) tells of them: the Surface Go 2 wires them to its
 * embedded controller, whose queries notify that device. It tells of
 * none of them until the 5-button array is loaded, function 1 of the
 * device's _DSM; loaded, the firmware takes its PNP0C0C away (_STA 0)
 * and tells of the power button through the array too. Each button is
 * its press and its let go:
 *
 *	0xC4 0xC5	Volume Up
 *	0xC6 0xC7	Volume Down
 *	0xCE 0xCF	the power button
 *
 * The volume buttons are a keyboard of their own to the input layer
 * ("acpibtn", knl_hid_ext_attach), whose keys are the keyboard page's
 * Volume Up and Volume Down: they are taken, and held repeat, as the
 * same keys of any keyboard are, and the window layer changes the
 * whole's volume with them (beep.c). The power button is the power
 * button's press, as the fixed event's is (acpi_os.c). The others the
 * array may send (the Windows button, the rotation lock, the tablet
 * and laptop modes) are said on the console and left.
 */
IMPORT INT	knl_hid_ext_attach( BOOL kbd, CONST UB *rd, INT rdlen, CONST UB *name );
IMPORT void	knl_hid_ext_report( INT hd, CONST UB *r, INT len );
IMPORT void	knl_usb_lock( void );
IMPORT void	knl_usb_unlock( void );
IMPORT void	knl_acpi_power_button_array( void );	/* acpi_os.c */

#define BA_DSM_REV	1
#define BA_DSM_FUNCS	0		/* the functions there are, a bit each */
#define BA_DSM_BTNL	1		/* the array loaded */
#define BA_SAID		16		/* presses and let goes said on the console */

LOCAL CONST UB	ba_dsm[16] = {		/* eeec56b3-4442-408f-a792-4edd4d758054 */
	0xb3, 0x56, 0xec, 0xee, 0x42, 0x44, 0x8f, 0x40, 0xa7, 0x92, 0x4e, 0xdd, 0x4d, 0x75, 0x80, 0x54
};

LOCAL void	*ba_dev = NULL;		/* the HID event filter, once its array is loaded */
LOCAL INT	ba_hd = 0;		/* its keyboard in the input layer */
LOCAL UB	ba_down = 0;		/* bit 0 Volume Up, bit 1 Volume Down held */
LOCAL INT	ba_said = 0;

/* The keys held, to the input layer as a keyboard's report */
LOCAL void ba_report( void )
{
	UB	r[8];
	INT	k = 2;

	knl_memset(r, 0, sizeof(r));
	if ( ba_down & 1 ) r[k++] = HID_KEY_VOLUP;
	if ( ba_down & 2 ) r[k++] = HID_KEY_VOLDOWN;
	knl_usb_lock();
	knl_hid_ext_report(ba_hd, r, 8);
	knl_usb_unlock();
}

LOCAL void ba_key( UB bit, BOOL down )
{
	/* a press with no let go since: the let go was lost, and it is pressed anew */
	if ( down && ( ba_down & bit ) != 0 ) {
		ba_down &= (UB)~bit;
		ba_report();
	}
	if ( down ) ba_down |= bit;
	else ba_down &= (UB)~bit;
	ba_report();
}

/* A notification of the array: TRUE when it was the array's */
LOCAL BOOL ba_notified( void *dev, unsigned int value )
{
	CONST char	*what;

	if ( ba_dev == NULL || dev != ba_dev ) {
		return FALSE;
	}
	switch ( value ) {
	case 0xc4: what = "Volume Up pressed";		break;
	case 0xc5: what = "Volume Up let go";		break;
	case 0xc6: what = "Volume Down pressed";	break;
	case 0xc7: what = "Volume Down let go";		break;
	case 0xce: what = "the power button pressed";	break;
	case 0xcf: what = "the power button let go";	break;
	default:   what = NULL;				break;
	}
	if ( what != NULL && ba_said < BA_SAID ) {
		ba_said++;
		tm_printf((UB *)"acpi: buttons: %s (%x)\n", what, value);
	}
	switch ( value ) {
	case 0xc4: case 0xc5:
		if ( ba_hd > 0 ) ba_key(1, value == 0xc4);
		break;
	case 0xc6: case 0xc7:
		if ( ba_hd > 0 ) ba_key(2, value == 0xc6);
		break;
	case 0xce:
		knl_acpi_power_button_array();
		break;
	default:
		break;
	}
	return TRUE;
}

/* The array of the HID event filter loaded, when the machine has one */
LOCAL void ba_start( void )
{
	void			*d = knl_acpica_device("INT33D5", 0);
	unsigned long long	funcs = 0, v = 0;
	char			path[64];

	if ( d == NULL ) {
		return;
	}
	if ( knl_acpica_path(d, path, sizeof(path)) != 0 ) path[0] = '\0';
	if ( knl_acpica_dsm_int(d, ba_dsm, BA_DSM_REV, BA_DSM_FUNCS, &funcs) != 0
	  || ( funcs & ( 1ULL << BA_DSM_BTNL ) ) == 0 ) {
		tm_printf((UB *)"acpi: buttons (%s, INT33D5): no 5-button array (_DSM functions %x)\n",
			  path, (UINT)funcs);
		return;
	}
	knl_usb_lock();				/* the input layer's devices, as the USB manager's are */
	ba_hd = knl_hid_ext_attach(TRUE, NULL, 0, (CONST UB *)"acpibtn");
	knl_usb_unlock();
	if ( ba_hd < 0 ) ba_hd = 0;
	ba_dev = d;				/* before the array tells of anything */
	if ( knl_acpica_dsm_int(d, ba_dsm, BA_DSM_REV, BA_DSM_BTNL, &v) != 0 ) {
		ba_dev = NULL;
		tm_printf((UB *)"acpi: buttons (%s, INT33D5): the 5-button array did not load\n", path);
		return;
	}
	tm_printf((UB *)"acpi: buttons (%s, INT33D5): the 5-button array loaded (_DSM functions %x); "
		  "Volume Up c4/c5, Volume Down c6/c7, power ce/cf; keyboard %d\n", path, (UINT)funcs, ba_hd);
}

/* ---------------------------------------------------------------- the firmware's word */

/* From the interpreter's task for notifications */
IMPORT void	knl_pm_lid( BOOL open );	/* kernel/sysdepend/x64/pm.c */

EXPORT void knl_acpi_os_notify( void *dev, unsigned int value )
{
	INT	i;

	BOOL	battery = FALSE;

	if ( ba_notified(dev, value) ) {
		return;
	}
	for ( i = 0; i < PW_MAX; i++ ) {
		if ( pw[i].used && pw[i].dev == dev && pw[i].kind == K_LID ) {
			unsigned long long	v = 1;

			(void)knl_acpica_int(dev, "_LID", &v);
			knl_pm_lid(v != 0);
		}
		if ( pw[i].used && pw[i].dev == dev && pw[i].kind != K_BUTTON ) {
			tell(&pw[i]);
			if ( pw[i].kind == K_BATTERY ) battery = TRUE;
		}
	}
	/* a battery that starts or stops discharging: the adapter may have been plugged without a word of its own */
	if ( battery && value == 0x80 ) {
		for ( i = 0; i < PW_MAX; i++ ) {
			if ( pw[i].used && pw[i].kind == K_AC ) tell(&pw[i]);
		}
	}
	/* the batteries read again by their task: the adapter's change starts or stops their charging */
	if ( !battery && pw_flg > 0 ) tk_set_flg(pw_flg, 1);
}

/*
 * The power button was pressed (acpi_os.c): array, told by the 5-button
 * array, else by the fixed event or a PNP0C0C's notification. A press
 * told by the other way less than PW_SAME_MS after one is the same
 * press, and answers FALSE: a firmware may tell of one press both ways.
 */
#define PW_SAME_MS	1000

LOCAL UD	pw_last_at = 0;		/* ms of the last press taken, 0 none */
LOCAL BOOL	pw_last_array = FALSE;

LOCAL UD now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return ( (UD)(UW)t.hi << 32 ) | (UW)t.lo;
}

EXPORT BOOL knl_acpi_power_pressed( BOOL array )
{
	INT	i;
	UD	now = now_ms();

	if ( pw_last_at != 0 && pw_last_array != array && now - pw_last_at < PW_SAME_MS ) {
		return FALSE;
	}
	pw_last_at = now | 1;
	pw_last_array = array;
	for ( i = 0; i < PW_MAX; i++ ) {
		if ( pw[i].used && pw[i].kind == K_BUTTON ) {
			pw[i].presses++;
			tell(&pw[i]);
		}
	}
	return TRUE;
}

LOCAL void pw_task( INT stacd, void *exinf )
{
	UINT	ptn;
	INT	i;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		(void)tk_wai_flg(pw_flg, 1, TWF_ORW | TWF_BITCLR, &ptn, BAT_POLL_MS);
		for ( i = 0; i < PW_MAX; i++ ) {
			if ( pw[i].used && pw[i].kind == K_BATTERY ) tell(&pw[i]);
		}
	}
}

/*
 * Once the object layer is up (devinit.c, after knl_ob_init): the
 * devices the namespace has, made objects
 */
EXPORT void knl_acpi_power_start( void )
{
	T_CMTX	cm;
	T_CFLG	cf;
	T_CTSK	ct;
	ID	t;
	INT	i;
	void	*d;
	BOOL	button = FALSE;

	if ( !knl_acpica_ready() ) {
		return;
	}
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	pw_mtx = tk_cre_mtx(&cm);
	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	pw_flg = tk_cre_flg(&cf);
	if ( pw_mtx <= 0 || pw_flg <= 0 ) return;
	for ( i = 0; i < 2 && ( d = knl_acpica_device("PNP0C0A", i) ) != NULL; i++ ) {
		add(K_BATTERY, d, ( i == 0 ) ? "battery0" : "battery1");
	}
	if ( ( d = knl_acpica_device("ACPI0003", 0) ) != NULL ) add(K_AC, d, "acpower");
	if ( ( d = knl_acpica_device("PNP0C0D", 0) ) != NULL ) add(K_LID, d, "lid");
	if ( ( d = knl_acpica_device("PNP0C0C", 0) ) != NULL
	  || ( d = knl_acpica_device("MSHW0040", 0) ) != NULL || ( d = knl_acpica_device("MSHW0028", 0) ) != NULL ) {
		add(K_BUTTON, d, "powerbutton");
		button = TRUE;
	}
	if ( !button && ( knl_acpi.flags & ( 1U << 4 ) ) == 0 ) {
		add(K_BUTTON, NULL, "powerbutton");	/* the FADT's fixed one (PWR_BUTTON clear: there is one) */
	}
	ba_start();				/* after the power button's object: loading takes its PNP0C0C away */
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)pw_task;
	ct.itskpri = 28;
	ct.stksz = 16 * 1024;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) tk_sta_tsk(t, 0);
}

#endif /* X64 */
