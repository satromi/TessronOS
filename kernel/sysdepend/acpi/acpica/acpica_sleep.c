/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpica_sleep.c
 *	The machine asleep through the ACPI interpreter (kernel/sysdepend/x64/pm.c
 *	decides and runs the steps):
 *
 *	- which ways the firmware offers: \_S3, the FADT's LOW_POWER_S0 flag
 *	  and the low power S0 idle device (PNP0D80);
 *	- S3: \_PTS(3) and \_SST, the waking vector in the FACS, SLP_TYP and
 *	  SLP_EN; on waking SCI_EN, \_BFS-less prepare, then \_WAK;
 *	- low power S0 idle: the device's _DSM (screen off, entry, exit,
 *	  screen on) and its list of constraints, said for the records;
 *	- the lid's and the power button's wake (_PRW, _DSW or _PSW);
 *	- the RTC's alarm as a fixed event, for sleeping a set time.
 *
 *	Built with the interpreter by tools/x64/acpica/build.sh.
 */

#include "acpi.h"
#include "accommon.h"
#include "tsacpi_os.h"

#define _COMPONENT	ACPI_OS_SERVICES
ACPI_MODULE_NAME("tssleep")

#define LPS0_FN_LIST		1

/* The _DSM's UUID c4eb40a0-6cd2-11e2-bcfd-0800200c9a66, as the firmware compares it */
static UINT8 lps0_uuid[16] = {
	0xa0, 0x40, 0xeb, 0xc4, 0xd2, 0x6c, 0xe2, 0x11, 0xbc, 0xfd, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};

/*
 * AMD's firmware answers another UUID, e3f32452-febc-43ce-9039-932122d37721,
 * with the same requests under other function numbers. Nothing makes
 * that way the default (pm.c): its deep state also asks of the system
 * management unit, which is not told here. Only sleep=s0 takes it.
 */
static UINT8 lps0_uuid_amd[16] = {
	0x52, 0x24, 0xf3, 0xe3, 0xbc, 0xfe, 0xce, 0x43, 0x90, 0x39, 0x93, 0x21, 0x22, 0xd3, 0x77, 0x21
};
static int		lps0_amd = 0;

static ACPI_HANDLE	lps0_dev = NULL;
static UINT64		lps0_fns = 0;
static int		lps0_looked = 0;
static void		(*rtc_fn)( void ) = NULL;

static void say( const char *f, ... )
{
	char	b[200];
	va_list	ap;

	va_start(ap, f);
	(void)vsnprintf(b, sizeof(b), f, ap);
	va_end(ap);
	knl_acpi_os_puts(b);
}

static ACPI_STATUS found_one( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	*(ACPI_HANDLE *)Context = h;
	return AE_CTRL_TERMINATE;
}

/* _DSM(the UUID, 0, fn, empty): its integer or buffer of up to 8 bytes in *p_val, or the package in *ret */
static ACPI_STATUS lps0_call( UINT32 fn, ACPI_BUFFER *ret )
{
	ACPI_OBJECT		a[4];
	ACPI_OBJECT_LIST	l = { 4, a };

	a[0].Type = ACPI_TYPE_BUFFER;
	a[0].Buffer.Length = sizeof(lps0_uuid);
	a[0].Buffer.Pointer = lps0_amd ? lps0_uuid_amd : lps0_uuid;
	a[1].Type = ACPI_TYPE_INTEGER;
	a[1].Integer.Value = 0;
	a[2].Type = ACPI_TYPE_INTEGER;
	a[2].Integer.Value = fn;
	a[3].Type = ACPI_TYPE_PACKAGE;
	a[3].Package.Count = 0;
	a[3].Package.Elements = NULL;
	return AcpiEvaluateObject(lps0_dev, "_DSM", &l, ret);
}

/* The functions the _DSM of the UUID in use lists */
static UINT64 lps0_list( void )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o;
	UINT32		k;
	UINT64		fns = 0;

	if ( ACPI_FAILURE(lps0_call(0, &b)) ) {
		return 0;
	}
	o = (ACPI_OBJECT *)b.Pointer;
	if ( o != NULL && o->Type == ACPI_TYPE_BUFFER ) {
		for ( k = 0; k < o->Buffer.Length && k < 8; k++ ) {
			fns |= (UINT64)o->Buffer.Pointer[k] << ( 8 * k );
		}
	} else if ( o != NULL && o->Type == ACPI_TYPE_INTEGER ) {
		fns = o->Integer.Value;
	}
	if ( b.Pointer != NULL ) ACPI_FREE(b.Pointer);
	return fns;
}

static void lps0_find( void )
{
	if ( lps0_looked ) return;
	lps0_looked = 1;
	(void)AcpiGetDevices("PNP0D80", found_one, &lps0_dev, NULL);
	if ( lps0_dev == NULL ) return;
	lps0_fns = lps0_list();
	if ( ( lps0_fns & 1 ) == 0 ) {
		lps0_amd = 1;
		lps0_fns = lps0_list();
		if ( ( lps0_fns & 1 ) != 0 ) {
			say("acpi: sleep: the low power S0 device answers AMD's UUID, functions %lx\n", (unsigned long)lps0_fns);
		}
	}
	if ( ( lps0_fns & 1 ) == 0 ) lps0_dev = NULL;
}

/* The ways the firmware offers (PMC_* of tsacpi_os.h) */
int knl_acpica_sleep_caps( void )
{
	UINT8	a = 0, bt = 0;
	int	c = 0;

	if ( !knl_acpica_ready() ) return 0;
	if ( ACPI_SUCCESS(AcpiGetSleepTypeData(ACPI_STATE_S3, &a, &bt)) ) c |= PMC_S3;
	if ( ( AcpiGbl_FADT.Flags & ACPI_FADT_LOW_POWER_S0 ) != 0 ) c |= PMC_LOW_S0;
	lps0_find();
	if ( lps0_dev != NULL && !lps0_amd ) c |= PMC_LPS0;
	say("acpi: sleep: \\_S3 %s (SLP_TYP %u/%u), FADT LOW_POWER_S0 %s, low power S0 device %s, _DSM functions %lx\n",
	    ( c & PMC_S3 ) ? "there" : "not there", a, bt, ( c & PMC_LOW_S0 ) ? "set" : "clear",
	    ( c & PMC_LPS0 ) ? "there" : "not there", (unsigned long)lps0_fns);
	return c;
}

/* The device's own list of what must be in which state for it to reach the low power state */
void knl_acpica_lps0_constraints( void )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o;
	UINT32		i;

	if ( lps0_dev == NULL || ( lps0_fns & ( 1U << LPS0_FN_LIST ) ) == 0 ) return;
	if ( ACPI_FAILURE(lps0_call(LPS0_FN_LIST, &b)) ) return;
	o = (ACPI_OBJECT *)b.Pointer;
	if ( o != NULL && o->Type == ACPI_TYPE_PACKAGE ) {
		for ( i = 0; i < o->Package.Count; i++ ) {
			ACPI_OBJECT	*e = &o->Package.Elements[i], *p;

			if ( e->Type != ACPI_TYPE_PACKAGE || e->Package.Count < 3
			  || e->Package.Elements[0].Type != ACPI_TYPE_STRING
			  || e->Package.Elements[1].Type != ACPI_TYPE_INTEGER
			  || e->Package.Elements[1].Integer.Value == 0 ) continue;
			p = &e->Package.Elements[2];
			if ( p->Type == ACPI_TYPE_PACKAGE && p->Package.Count >= 2
			  && p->Package.Elements[1].Type == ACPI_TYPE_PACKAGE
			  && p->Package.Elements[1].Package.Count >= 2
			  && p->Package.Elements[1].Package.Elements[1].Type == ACPI_TYPE_INTEGER ) {
				say("acpi: sleep: constraint %s: D%u\n", e->Package.Elements[0].String.Pointer,
				    (unsigned)p->Package.Elements[1].Package.Elements[1].Integer.Value);
			}
		}
	}
	if ( b.Pointer != NULL ) ACPI_FREE(b.Pointer);
}

/* A _DSM function of the low power S0 device: PMC_LPS0_* */
void knl_acpica_lps0( int fn )
{
	ACPI_STATUS	s;

	if ( lps0_amd ) {
		/* screen off 3 to 4, on 4 to 5, entry 5 to 2, exit 6 to 3 */
		fn = ( fn == 3 ) ? 4 : ( fn == 4 ) ? 5 : ( fn == 5 ) ? 2 : ( fn == 6 ) ? 3 : fn;
	}
	if ( lps0_dev == NULL || fn >= 64 || ( lps0_fns & ( 1ULL << fn ) ) == 0 ) {
		say("acpi: sleep: _DSM %d not offered\n", fn);
		return;
	}
	s = lps0_call((UINT32)fn, NULL);
	say("acpi: sleep: _DSM %d: %s\n", fn, AcpiFormatException(s));
}

/* A device's _PRW: its GPE; the block's handle when not the FADT's */
static int wake_gpe_of( ACPI_HANDLE h, ACPI_HANDLE *p_blk, UINT32 *p_gpe )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o, *e;
	int		ok = 0;

	if ( ACPI_FAILURE(AcpiEvaluateObjectTyped(h, "_PRW", NULL, &b, ACPI_TYPE_PACKAGE)) ) return 0;
	o = (ACPI_OBJECT *)b.Pointer;
	if ( o->Package.Count >= 1 ) {
		e = &o->Package.Elements[0];
		if ( e->Type == ACPI_TYPE_INTEGER ) {
			*p_blk = NULL;
			*p_gpe = (UINT32)e->Integer.Value;
			ok = 1;
		} else if ( e->Type == ACPI_TYPE_PACKAGE && e->Package.Count == 2
			 && e->Package.Elements[0].Type == ACPI_TYPE_LOCAL_REFERENCE
			 && e->Package.Elements[1].Type == ACPI_TYPE_INTEGER ) {
			*p_blk = e->Package.Elements[0].Reference.Handle;
			*p_gpe = (UINT32)e->Package.Elements[1].Integer.Value;
			ok = 1;
		}
	}
	ACPI_FREE(b.Pointer);
	return ok;
}

/* _DSW(on, the state, 0), or the older _PSW(on) */
static void device_wake_method( ACPI_HANDLE h, int on, int state )
{
	ACPI_OBJECT		a[3];
	ACPI_OBJECT_LIST	l = { 3, a };
	ACPI_HANDLE		m;

	a[0].Type = a[1].Type = a[2].Type = ACPI_TYPE_INTEGER;
	a[0].Integer.Value = (UINT64)on;
	a[1].Integer.Value = (UINT64)state;
	a[2].Integer.Value = 0;
	if ( ACPI_SUCCESS(AcpiGetHandle(h, "_DSW", &m)) ) {
		(void)AcpiEvaluateObject(h, "_DSW", &l, NULL);
		return;
	}
	if ( ACPI_SUCCESS(AcpiGetHandle(h, "_PSW", &m)) ) {
		l.Count = 1;
		(void)AcpiEvaluateObject(h, "_PSW", &l, NULL);
	}
}

typedef struct {
	int	on;
	int	state;		/* the sleep state, 0 for low power S0 idle */
} WAKE_ARG;

static ACPI_STATUS wake_one( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	WAKE_ARG	*w = (WAKE_ARG *)Context;
	ACPI_HANDLE	blk = NULL;
	UINT32		gpe = 0;
	ACPI_STATUS	s;
	char		path[64];
	ACPI_BUFFER	n = { sizeof(path), path };

	if ( !wake_gpe_of(h, &blk, &gpe) ) return AE_OK;
	device_wake_method(h, w->on, w->state);
	s = AcpiSetGpeWakeMask(blk, gpe, w->on ? ACPI_GPE_ENABLE : ACPI_GPE_DISABLE);
	if ( w->state == 0 ) {
		/* no sleep registers written: the GPE works as any running one does */
		if ( w->on ) {
			(void)AcpiEnableGpe(blk, gpe);
		} else {
			(void)AcpiDisableGpe(blk, gpe);
		}
	}
	path[0] = '\0';
	(void)AcpiGetName(h, ACPI_FULL_PATHNAME, &n);
	say("acpi: sleep: wake by %s, GPE %x: %s %s\n", path, gpe, w->on ? "on" : "off", AcpiFormatException(s));
	return AE_OK;
}

/* The lid and the buttons may (on) or may no more wake the machine from the state */
void knl_acpica_wake_devices( int on, int state )
{
	WAKE_ARG	w;

	w.on = on;
	w.state = state;
	(void)AcpiGetDevices("PNP0C0D", wake_one, &w, NULL);
	(void)AcpiGetDevices("PNP0C0C", wake_one, &w, NULL);
	(void)AcpiGetDevices("PNP0C0E", wake_one, &w, NULL);
}

/*
 * \_PTS(3) and \_SST, with the system still whole but this processor
 * alone, the waking vector in the FACS
 */
int knl_acpica_s3_prep( unsigned long long vector )
{
	ACPI_STATUS	s;

	s = AcpiEnterSleepStatePrep(ACPI_STATE_S3);
	say("acpi: sleep: \\_PTS(3) and \\_SST: %s\n", AcpiFormatException(s));
	if ( ACPI_FAILURE(s) ) return -1;
	s = AcpiSetFirmwareWakingVector((ACPI_PHYSICAL_ADDRESS)vector, 0);
	say("acpi: sleep: waking vector %lx in the FACS: %s\n", (unsigned long)vector, AcpiFormatException(s));
	return ACPI_SUCCESS(s) ? 0 : -1;
}

/*
 * S3: the last step, the processors' caches written back by ACPICA. It
 * returns only when SLP_EN was not taken; a waking comes in at the
 * vector instead.
 */
int knl_acpica_s3_enter( void )
{
	ACPI_STATUS	s;

	s = AcpiEnterSleepState(ACPI_STATE_S3);
	say("acpi: sleep: S3 not entered: %s\n", AcpiFormatException(s));
	return -1;
}

/*
 * On waking, the interrupts still off. Firmware that left SCI_EN clear
 * would stop every ACPI event from here on, so it is set whatever the
 * firmware did, then the SLP_TYP put back for S0. What woke the machine
 * is said first, before the status is cleared.
 */
void knl_acpica_s3_leave_prep( void )
{
	knl_acpica_wake_found();
	(void)AcpiWriteBitRegister(ACPI_BITREG_SCI_ENABLE, 1);
	(void)AcpiLeaveSleepStatePrep(ACPI_STATE_S3);
}

/*
 * \_WAK and the running GPEs enabled again, with the interrupts on and
 * the system whole. The power button's press that woke the machine is
 * cleared: it was not a request to sleep again.
 */
void knl_acpica_s3_leave( void )
{
	ACPI_STATUS	s;

	(void)AcpiClearEvent(ACPI_EVENT_POWER_BUTTON);
	s = AcpiLeaveSleepState(ACPI_STATE_S3);
	(void)AcpiSetFirmwareWakingVector(0, 0);
	say("acpi: sleep: \\_WAK: %s\n", AcpiFormatException(s));
}

static UINT32 rtc_event( void *Context )
{
	(void)AcpiClearEvent(ACPI_EVENT_RTC);
	(void)AcpiDisableEvent(ACPI_EVENT_RTC, 0);
	if ( rtc_fn != NULL ) (*rtc_fn)();
	return ACPI_INTERRUPT_HANDLED;
}

/* The RTC's alarm as a wake (fn from the interrupt) or, with fn NULL, no more */
void knl_acpica_rtc_event( void (*fn)( void ) )
{
	static int	installed = 0;

	if ( !knl_acpica_ready() ) return;
	if ( fn == NULL ) {
		(void)AcpiDisableEvent(ACPI_EVENT_RTC, 0);
		rtc_fn = NULL;
		return;
	}
	rtc_fn = fn;
	if ( !installed ) {
		installed = ACPI_SUCCESS(AcpiInstallFixedEventHandler(ACPI_EVENT_RTC, rtc_event, NULL));
	}
	(void)AcpiClearEvent(ACPI_EVENT_RTC);
	(void)AcpiEnableEvent(ACPI_EVENT_RTC, 0);
}
