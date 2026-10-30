/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpica_off.c
 *	The machine turned off through the ACPI interpreter, in the order
 *	ACPI lays down for a sleep state (\_TTS, \_PTS, then SLP_TYP and
 *	SLP_EN):
 *
 *	- \_TTS(5) while the system still runs, before the devices stop
 *	  (knl_finish_device), for a reset as well as for off;
 *	- what the last step will do, said while the records can still be
 *	  written (the sleep registers, \_S5, the GPEs enabled and those
 *	  kept for waking);
 *	- at the end, every other processor stopped: \_PTS(5) and \_SST
 *	  (AcpiEnterSleepStatePrep), every GPE disabled, then SLP_TYP and
 *	  SLP_EN (AcpiEnterSleepState: the PM1 status cleared, the wake
 *	  GPEs only enabled, PM1a and PM1b written each with its own
 *	  SLP_TYP, first without SLP_EN and then with it; the sleep control
 *	  register of the FADT instead on a hardware-reduced machine);
 *	- as the machine starts, the wake status the firmware left, before
 *	  the interpreter clears it: what turned the machine on again when
 *	  it was asked to go off.
 *
 *	Built with the interpreter by tools/x64/acpica/build.sh.
 */

#include "acpi.h"
#include "accommon.h"
#include "tsacpi_os.h"

#define _COMPONENT	ACPI_OS_SERVICES
ACPI_MODULE_NAME("tsoff")

/* The PM1 status bits */
#define PM1_TMR		0x0001
#define PM1_BM		0x0010
#define PM1_GBL		0x0020
#define PM1_PWRBTN	0x0100
#define PM1_SLPBTN	0x0200
#define PM1_RTC		0x0400
#define PM1_PCIEXP_WAKE	0x4000
#define PM1_WAK		0x8000

#define GPE_MAX		256

static void say( const char *f, ... )
{
	char	b[200];
	va_list	ap;

	va_start(ap, f);
	(void)vsnprintf(b, sizeof(b), f, ap);
	va_end(ap);
	knl_acpi_os_puts(b);
}

/* One byte of a GPE block's register: the status half first, the enable half after it */
static UINT8 gpe_byte( ACPI_GENERIC_ADDRESS *blk, UINT32 off )
{
	UINT32	v = 0;
	UINT64	m = 0;

	if ( blk->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO ) {
		(void)AcpiOsReadPort((ACPI_IO_ADDRESS)( blk->Address + off ), &v, 8);
		return (UINT8)v;
	}
	(void)AcpiOsReadMemory((ACPI_PHYSICAL_ADDRESS)( blk->Address + off ), &m, 8);
	return (UINT8)m;
}

/* The numbers of the GPEs whose bit is set in one half (status or enable) of a block, appended to b */
static int gpe_bits( char *b, int len, int max, ACPI_GENERIC_ADDRESS *blk, UINT32 bytes, UINT32 half,
		     UINT32 base )
{
	UINT32	i, k;
	UINT8	v;

	if ( blk->Address == 0 || bytes == 0 ) return len;
	for ( i = 0; i < bytes / 2; i++ ) {
		v = gpe_byte(blk, half * ( bytes / 2 ) + i);
		for ( k = 0; k < 8; k++ ) {
			if ( ( v & ( 1U << k ) ) != 0 && len < max - 8 ) {
				len += snprintf(b + len, (ACPI_SIZE)( max - len ), " %02x", base + i * 8 + k);
			}
		}
	}
	return len;
}

static int pm1_names( char *b, int len, int max, UINT32 sts )
{
	static const struct { UINT32 bit; const char *name; } n[] = {
		{ PM1_WAK, "WAK" }, { PM1_PWRBTN, "PWRBTN" }, { PM1_SLPBTN, "SLPBTN" }, { PM1_RTC, "RTC" },
		{ PM1_PCIEXP_WAKE, "PCIEXP_WAKE" }, { PM1_GBL, "GBL" }, { PM1_BM, "BM" }, { PM1_TMR, "TMR" }
	};
	UINT32	i;

	for ( i = 0; i < sizeof(n) / sizeof(n[0]); i++ ) {
		if ( ( sts & n[i].bit ) != 0 && len < max - 16 ) {
			len += snprintf(b + len, (ACPI_SIZE)( max - len ), " %s", n[i].name);
		}
	}
	return len;
}

/*
 * As the machine starts, after the tables and before the hardware is put
 * in ACPI mode (which clears the GPE status): the PM1 status and the GPE
 * status the firmware left. WAK with a source beside it (PWRBTN,
 * PCIEXP_WAKE, RTC, a GPE) says what woke a machine that was asked to go
 * off; nothing set is a start from off as the firmware makes it.
 */
void knl_acpica_wake_found( void )
{
	char	b[200];
	int	len;
	UINT32	sts = 0, en = 0, cnt = 0;

	if ( ACPI_FAILURE(AcpiHwRegisterRead(ACPI_REGISTER_PM1_STATUS, &sts)) ) return;
	(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_ENABLE, &en);
	(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_CONTROL, &cnt);
	len = snprintf(b, sizeof(b), "acpi: as the firmware left it: PM1 status %04x (", sts);
	len = pm1_names(b, len, (int)sizeof(b), sts);
	len += snprintf(b + len, sizeof(b) - (ACPI_SIZE)len, " ), enable %04x, control %04x; GPE status set:", en, cnt);
	len = gpe_bits(b, len, (int)sizeof(b), &AcpiGbl_FADT.XGpe0Block, AcpiGbl_FADT.Gpe0BlockLength, 0, 0);
	len = gpe_bits(b, len, (int)sizeof(b), &AcpiGbl_FADT.XGpe1Block, AcpiGbl_FADT.Gpe1BlockLength, 0,
		       AcpiGbl_FADT.Gpe1Base);
	(void)snprintf(b + len, sizeof(b) - (ACPI_SIZE)len, "\n");
	knl_acpi_os_puts(b);
}

/* \_TTS(state): 1 when run, 0 when the firmware has none, -1 when it failed */
int knl_acpica_tts( int state )
{
	ACPI_HANDLE		h = NULL;
	ACPI_OBJECT		arg;
	ACPI_OBJECT_LIST	args = { 1, &arg };
	ACPI_STATUS		s;

	if ( !knl_acpica_ready() ) return 0;
	if ( ACPI_FAILURE(AcpiGetHandle(NULL, "\\_TTS", &h)) ) {
		say("acpi: off: the firmware has no \\_TTS\n");
		return 0;
	}
	arg.Type = ACPI_TYPE_INTEGER;
	arg.Integer.Value = (UINT64)state;
	s = AcpiEvaluateObject(h, NULL, &args, NULL);
	say("acpi: off: \\_TTS(%d): %s\n", state, AcpiFormatException(s));
	return ACPI_SUCCESS(s) ? 1 : -1;
}

/* The GPEs whose flag (ACPI_EVENT_FLAG_*) is set, appended to b */
static int gpe_flags( char *b, int len, int max, ACPI_EVENT_STATUS flag )
{
	ACPI_EVENT_STATUS	st;
	UINT32			n;

	for ( n = 0; n < GPE_MAX; n++ ) {
		if ( ACPI_SUCCESS(AcpiGetGpeStatus(NULL, n, &st)) && ( st & flag ) != 0 && len < max - 8 ) {
			len += snprintf(b + len, (ACPI_SIZE)( max - len ), " %02x", n);
		}
	}
	return len;
}

/*
 * What the last step will do, said before the records are written for
 * the last time: the way into S5 (the PM1 control blocks, or the sleep
 * control register of a hardware-reduced machine), \_S5's types, whether
 * \_PTS is there, the GPEs enabled now and those that would be left
 * enabled for waking from S5 (none unless one was asked for), and the
 * fixed events enabled (the power button wakes a machine in S5 whatever
 * PM1 enable says).
 */
void knl_acpica_off_plan( void )
{
	char		b[200];
	int		len;
	UINT8		a = 0, bt = 0;
	ACPI_STATUS	s;
	ACPI_HANDLE	h = NULL;
	UINT32		en = 0, cnt = 0;

	if ( !knl_acpica_ready() ) {
		say("acpi: off: the interpreter is not running: the tables alone turn the machine off\n");
		return;
	}
	s = AcpiGetSleepTypeData(ACPI_STATE_S5, &a, &bt);
	if ( AcpiGbl_ReducedHardware ) {
		say("acpi: off: hardware-reduced: SLP_TYP %u and SLP_EN to the sleep control register %lx (%s)\n",
		    a, (unsigned long)AcpiGbl_FADT.SleepControl.Address, AcpiFormatException(s));
	} else {
		(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_CONTROL, &cnt);
		(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_ENABLE, &en);
		say("acpi: off: \\_S5 %u/%u (%s): PM1a control %lx, PM1b control %lx%s; now control %04x, enable %04x\n",
		    a, bt, AcpiFormatException(s), (unsigned long)AcpiGbl_FADT.XPm1aControlBlock.Address,
		    (unsigned long)AcpiGbl_FADT.XPm1bControlBlock.Address,
		    AcpiGbl_FADT.XPm1bControlBlock.Address == 0 ? " (none)" : "", cnt, en);
	}
	say("acpi: off: \\_PTS %s\n", ACPI_SUCCESS(AcpiGetHandle(NULL, "\\_PTS", &h)) ? "there" : "not there");
	len = snprintf(b, sizeof(b), "acpi: off: GPEs enabled now:");
	len = gpe_flags(b, len, (int)sizeof(b), ACPI_EVENT_FLAG_ENABLED);
	(void)snprintf(b + len, sizeof(b) - (ACPI_SIZE)len, "\n");
	knl_acpi_os_puts(b);
	len = snprintf(b, sizeof(b), "acpi: off: GPEs left enabled for waking from S5:");
	len = gpe_flags(b, len, (int)sizeof(b), ACPI_EVENT_FLAG_WAKE_ENABLED);
	(void)snprintf(b + len, sizeof(b) - (ACPI_SIZE)len, "\n");
	knl_acpi_os_puts(b);
}

/*
 * Off. Called with every other processor stopped and the interrupts
 * off (hw_setting.c): \_PTS(5) and \_SST, every GPE disabled (the
 * runtime ones must not raise an SCI or a wake between here and SLP_EN;
 * ACPICA enables the wake ones again as it enters S5), then the sleep
 * registers. Each step is said on the screen, the last records having
 * been written already. AcpiEnterSleepState writes SLP_EN a second time
 * after 10 s and then waits for WAK_STS: it returns only on an error.
 */
int knl_acpica_poweroff( void )
{
	ACPI_STATUS	s;
	UINT32		sts = 0, cnt = 0;

	if ( !knl_acpica_ready() ) {
		return -1;
	}
	s = AcpiEnterSleepStatePrep(ACPI_STATE_S5);
	say("acpi: off: \\_PTS(5) and \\_SST: %s\n", AcpiFormatException(s));
	s = AcpiDisableAllGpes();
	say("acpi: off: every GPE disabled: %s\n", AcpiFormatException(s));
	(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_STATUS, &sts);
	say("acpi: off: SLP_TYP %u/%u, then SLP_EN (PM1 status %04x); still on after 10 s: SLP_EN not taken\n",
	    AcpiGbl_SleepTypeA, AcpiGbl_SleepTypeB, sts);
	__asm__ volatile ( "cli" );
	s = AcpiEnterSleepState(ACPI_STATE_S5);
	(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_STATUS, &sts);
	(void)AcpiHwRegisterRead(ACPI_REGISTER_PM1_CONTROL, &cnt);
	say("acpi: off: S5 not entered: %s (PM1 status %04x, control %04x)\n", AcpiFormatException(s), sts, cnt);
	return -1;
}
