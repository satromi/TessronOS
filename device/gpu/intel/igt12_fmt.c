/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt12_fmt.c (x64 PC, IGT12=1 only)
 *	The formats of the 12th generation's execlists, which differ from the
 *	9th's (docs/tessronos/13-gpu.md 13.11.16):
 *
 *	  - a context's descriptor carries its engine's class and instance
 *	    beside the context number
 *	  - a queue of 8 elements is written to its registers and started by
 *	    a separate load, where the 9th generation has a port of two
 *	  - a status entry is 64 bits of two context numbers, the one left and
 *	    the one gone to, read from 12 entries of the status page
 *
 *	Nothing calls these on a device yet: only the tests do, on memory of
 *	their own. The engines' start is not written.
 */

#include <sys/machine.h>

#ifdef X64
#ifdef IGT12

#include "kernel.h"
#include "igt12.h"

#define DESC_VALID		0x001
#define DESC_FORCE_RESTORE	0x004
#define DESC_LEGACY_64BIT	0x018	/* legacy context, 48 bit canonical addresses, PML4 in PDP0 */
#define DESC_PRIVILEGE		0x100	/* has to be set */
#define DESC_PRIO_SHIFT		9

#define R_SQ_CONTENTS		0x510
#define R_EXECLIST_CONTROL	0x550
#define  CONTROL_LOAD		0x1
#define SQ_ELEMENTS		8

/*
 * 'prio' is < 0 on the engines where the field is not to be programmed; the
 * fault mode stays 0, fault and hang, the only one defined.
 */
EXPORT UD igt12_desc( UW lrca, UINT eng_class, UINT instance, UINT sw_ctx, UINT sw_count, INT prio,
		      BOOL force_restore )
{
	UD	lo, hi;

	if ( lrca == 0 || ( lrca & 0xfff ) != 0 || eng_class > 7 || instance > 63 || sw_ctx >= IGT12_IDLE_CTX
	  || sw_count > 63 || prio > 2 ) {
		return 0;
	}
	lo = (UD)lrca | DESC_PRIVILEGE | DESC_LEGACY_64BIT | DESC_VALID;
	if ( prio >= 0 ) lo |= (UD)prio << DESC_PRIO_SHIFT;
	if ( force_restore ) lo |= DESC_FORCE_RESTORE;
	hi = ( (UD)eng_class << 29 ) | ( (UD)sw_count << 23 ) | ( (UD)instance << 16 ) | ( (UD)sw_ctx << 5 );
	return ( hi << 32 ) | lo;
}

EXPORT void igt12_cs_decode( UD qw, T_IGT12CS *cs )
{
	UW	dw0 = (UW)qw, dw1 = (UW)( qw >> 32 );
	BOOL	to_idle, away_idle;

	cs->to = (UH)( ( dw0 >> 15 ) & 0x7ff );
	cs->away = (UH)( ( dw1 >> 15 ) & 0x7ff );
	cs->instance = (UB)( ( dw0 >> 6 ) & 0x3f );
	cs->eng_class = (UB)( ( dw0 >> 3 ) & 7 );
	cs->new_queue = ( dw0 & 1 ) != 0;
	cs->detail = (UB)( dw1 & 0xf );
	to_idle = ( cs->to == IGT12_IDLE_CTX );
	away_idle = ( cs->away == IGT12_IDLE_CTX );
	if ( cs->new_queue ) {
		cs->kind = ( away_idle && to_idle ) ? IGT12_CS_IDLE_REPORT
			 : away_idle ? IGT12_CS_IDLE_TO_ACTIVE
			 : to_idle ? IGT12_CS_PREEMPT_TO_IDLE : IGT12_CS_NEW_QUEUE;
	} else {
		cs->kind = to_idle ? IGT12_CS_ACTIVE_TO_IDLE : IGT12_CS_ELEMENT;
	}
}

EXPORT void igt12_sq_load( UBINT mmio, UW eng_base, UD desc )
{
	volatile UW	*sq = (volatile UW *)( mmio + eng_base + R_SQ_CONTENTS );
	INT		i;

	sq[0] = (UW)desc;
	sq[1] = (UW)( desc >> 32 );
	for ( i = 1; i < SQ_ELEMENTS; i++ ) {
		sq[i * 2] = 0;
		sq[i * 2 + 1] = 0;
	}
	*(volatile UW *)( mmio + eng_base + R_EXECLIST_CONTROL ) = CONTROL_LOAD;
}

#endif /* IGT12 */
#endif /* X64 */
