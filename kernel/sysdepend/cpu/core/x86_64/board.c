/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	board.c (x86-64)
 *	Which machine this is (include/ts/board.h): one image serves every
 *	PC with UEFI firmware, and the machine is told by SMBIOS, the
 *	System Information structure's manufacturer and product name.
 *
 *	There is no device tree on a PC. The names of it the common parts
 *	read are here, and say nothing is there; what a device tree would
 *	say comes from ACPI (kernel/sysdepend/acpi) and from the loader's
 *	information (bootinfo.h).
 */

#include <sys/machine.h>

#ifdef CPU_CORE_X86_64

#include "kernel.h"
#include "../../../sysdepend.h"
#include <tm/tmonitor.h>
#include <ts/board.h>

EXPORT T_BOARD	knl_board;
EXPORT UBINT	knl_dtb_addr = 0;
EXPORT UD	knl_dtb_size = 0;
EXPORT T_DTBFB	knl_dtb_fb;
EXPORT UB	knl_dtb_compat[1];
EXPORT INT	knl_dtb_compat_len = 0;
EXPORT UB	knl_dtb_model[64];		/* the SMBIOS product name */

EXPORT CONST UB *knl_dtb_prop( CONST char *path, CONST char *prop, INT *p_len )
{
	(void)path;
	(void)prop;
	if ( p_len != NULL ) *p_len = 0;
	return NULL;
}

EXPORT UW knl_dtb_u32( CONST char *path, CONST char *prop, UW dflt )
{
	(void)path;
	(void)prop;
	return dflt;
}

EXPORT BOOL knl_dtb_okay( CONST char *path )
{
	(void)path;
	return FALSE;
}

typedef struct {
	UW		kind;
	CONST char	*vendor;	/* the start of the SMBIOS manufacturer, NULL any */
	CONST char	*product;	/* the start of the product name */
	CONST char	*name;
} BOARD_DEF;

LOCAL CONST BOARD_DEF	board_def[] = {
	{ BOARD_QEMU_Q35, "QEMU", "Standard PC (Q35", "QEMU q35" },
	{ BOARD_SURFACE_GO2, "Microsoft", "Surface Go 2", "Microsoft Surface Go 2" },
	/* a ThinkPad's product name is its machine type; the model name is in the version */
	{ BOARD_X1YOGA3, "LENOVO", "20LD", "Lenovo ThinkPad X1 Yoga (3rd)" },
	{ BOARD_X1YOGA3, "LENOVO", "20LE", "Lenovo ThinkPad X1 Yoga (3rd)" },
	{ BOARD_X1YOGA3, "LENOVO", "20LF", "Lenovo ThinkPad X1 Yoga (3rd)" },
	{ BOARD_X1YOGA3, "LENOVO", "20LG", "Lenovo ThinkPad X1 Yoga (3rd)" },
};
#define N_BOARD_DEF	( sizeof(board_def) / sizeof(board_def[0]) )

LOCAL BOOL starts( CONST UB *s, CONST char *head )
{
	while ( *head != '\0' ) {
		if ( *s++ != (UB)*head++ ) return FALSE;
	}
	return TRUE;
}

/* String n (from 1) of an SMBIOS structure, "" when there is none */
LOCAL CONST UB *smb_str( CONST UB *st, INT n )
{
	CONST UB	*p = st + st[1];

	if ( n <= 0 ) {
		return (CONST UB *)"";
	}
	while ( --n > 0 ) {
		while ( *p != '\0' ) p++;
		p++;
		if ( *p == '\0' ) return (CONST UB *)"";
	}
	return p;
}

/*
 * The System Information structure (type 1) of the SMBIOS table the
 * loader found: its manufacturer and product name copied out
 */
LOCAL void smbios_system( UB *vendor, INT vmax, UB *product, INT pmax )
{
	CONST T_BOOTINFO *bi = knl_bootinfo;
	CONST UB	*ep, *t, *end;
	UD		tbl;
	UW		len;
	INT		i;

	vendor[0] = product[0] = '\0';
	if ( bi->smbios3_pa != 0 ) {
		ep = (CONST UB *)PA2VA(bi->smbios3_pa);
		if ( !starts(ep, "_SM3_") ) return;
		len = *(CONST UW *)( ep + 12 );
		tbl = *(CONST UD *)( ep + 16 );
	} else if ( bi->smbios_pa != 0 ) {
		ep = (CONST UB *)PA2VA(bi->smbios_pa);
		if ( !starts(ep, "_SM_") ) return;
		len = *(CONST UH *)( ep + 22 );
		tbl = *(CONST UW *)( ep + 24 );
	} else {
		return;
	}
	t = (CONST UB *)PA2VA(tbl);
	end = t + len;
	while ( t + 4 <= end && t[0] != 127 ) {
		CONST UB	*next = t + t[1];

		while ( next + 1 < end && ( next[0] != 0 || next[1] != 0 ) ) next++;
		next += 2;
		if ( t[0] == 1 && t[1] >= 8 ) {
			CONST UB	*v = smb_str(t, t[4]), *p = smb_str(t, t[5]);

			for ( i = 0; i < vmax - 1 && v[i] != '\0'; i++ ) vendor[i] = v[i];
			vendor[i] = '\0';
			for ( i = 0; i < pmax - 1 && p[i] != '\0'; i++ ) product[i] = p[i];
			product[i] = '\0';
			return;
		}
		t = next;
	}
}

EXPORT void knl_board_set( UW kind, UW rev )
{
	INT	i;

	knl_board.kind = kind;
	knl_board.rev = rev;
	knl_board.name = ( knl_dtb_model[0] != '\0' ) ? (CONST char *)knl_dtb_model : "PC";
	knl_board.sd = BOARD_SD_PCI;
	knl_board.sd_width = 8;
	for ( i = 0; i < (INT)N_BOARD_DEF; i++ ) {
		if ( board_def[i].kind == kind ) {
			knl_board.name = board_def[i].name;
		}
	}
}

EXPORT BOOL knl_bootarg( CONST UB *args, CONST char *key, UB *val, INT max )
{
	CONST UB	*p = args;
	INT		i, n;

	while ( *p != '\0' ) {
		while ( *p == ' ' || *p == '\t' || *p == '\n' ) p++;
		for ( i = 0; key[i] != '\0' && p[i] == (UB)key[i]; i++ ) ;
		if ( key[i] == '\0' && p[i] == '=' ) {
			p += i + 1;
			for ( n = 0; p[n] != '\0' && p[n] != ' ' && p[n] != '\t' && p[n] != '\n'; n++ ) {
				if ( n < max - 1 ) val[n] = p[n];
			}
			if ( max > 0 ) val[( n < max - 1 ) ? n : max - 1] = '\0';
			return TRUE;
		}
		while ( *p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' ) p++;
	}
	return FALSE;
}

EXPORT BOOL knl_board_has_compat( CONST UB *compat, INT len, CONST char *want )
{
	(void)compat;
	(void)len;
	(void)want;
	return FALSE;
}

EXPORT UW knl_board_by_dt( CONST UB *compat, INT len, CONST UB *model )
{
	(void)compat;
	(void)len;
	(void)model;
	return BOARD_UNKNOWN;
}

EXPORT UW knl_board_by_rev( UW rev )
{
	(void)rev;
	return BOARD_UNKNOWN;
}

EXPORT void knl_board_set_rev( UW rev )
{
	knl_board_set(knl_board.kind, rev);
}

/* The machine as SMBIOS names it */
EXPORT void knl_board_init( void )
{
	UB	vendor[32];
	INT	i;
	UW	kind = BOARD_PC;

	knl_memset(&knl_board, 0, sizeof(knl_board));
	smbios_system(vendor, sizeof(vendor), knl_dtb_model, sizeof(knl_dtb_model));
	for ( i = 0; i < (INT)N_BOARD_DEF; i++ ) {
		if ( ( board_def[i].vendor == NULL || starts(vendor, board_def[i].vendor) )
		  && starts(knl_dtb_model, board_def[i].product) ) {
			kind = board_def[i].kind;
		}
	}
	knl_board_set(kind, 0);
	tm_printf((UB *)"TessronOS x64: machine \"%s\" \"%s\": %s\n", vendor, knl_dtb_model, knl_board.name);
}

#endif /* CPU_CORE_X86_64 */
