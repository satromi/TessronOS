/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	edid.c
 *	What a display says of itself in its EDID (VESA E-EDID 1.3 and 1.4,
 *	CTA-861 extensions): who made it, its size, whether it is HDMI and
 *	plays sound, and the modes it offers (docs/tessronos 10.4.9).
 *
 *	The modes are listed in the order Linux's DRM adds them
 *	(drm_add_edid_modes): the detailed timings first, of the base block
 *	and then of the extensions, the first of them the display's
 *	preferred; then the standard timings, of the base block and of its
 *	descriptors; then the established timings; then the CTA extension's
 *	video identification codes. A mode already listed (the same size,
 *	refresh and interlacing) is not listed again. A standard timing not in
 *	the table of display monitor timings below is timed by the CVT
 *	formula with reduced blanking.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/disp.h>
#include <ts/dispout.h>
#include "edid.h"

/* A mode from the tables: size, refresh, clock and the totals */
typedef struct {
	UH	w, h, hz;
	UW	khz;
	UH	hss, hse, ht, vss, vse, vt;
	UB	pol;			/* bit 0 hsync positive, bit 1 vsync positive */
} MTAB;

/* Display monitor timings (VESA DMT) of the established timings and the common standard ones */
LOCAL CONST MTAB	dmt[] = {
	{  640,  480, 60,  25175,  656,  752,  800,  490,  492,  525, 0 },
	{  640,  480, 72,  31500,  664,  704,  832,  489,  492,  520, 0 },
	{  640,  480, 75,  31500,  656,  720,  840,  481,  484,  500, 0 },
	{  720,  400, 70,  28320,  738,  846,  900,  412,  414,  449, 2 },
	{  800,  600, 56,  36000,  824,  896, 1024,  601,  603,  625, 3 },
	{  800,  600, 60,  40000,  840,  968, 1056,  601,  605,  628, 3 },
	{  800,  600, 72,  50000,  856,  976, 1040,  637,  643,  666, 3 },
	{  800,  600, 75,  49500,  816,  896, 1056,  601,  604,  625, 3 },
	{ 1024,  768, 60,  65000, 1048, 1184, 1344,  771,  777,  806, 0 },
	{ 1024,  768, 70,  75000, 1048, 1184, 1328,  771,  777,  806, 0 },
	{ 1024,  768, 75,  78750, 1040, 1136, 1312,  769,  772,  800, 3 },
	{ 1152,  864, 75, 108000, 1216, 1344, 1600,  865,  868,  900, 3 },
	{ 1280,  960, 60, 108000, 1376, 1488, 1800,  961,  964, 1000, 3 },
	{ 1280, 1024, 60, 108000, 1328, 1440, 1688, 1025, 1028, 1066, 3 },
	{ 1280, 1024, 75, 135000, 1296, 1440, 1688, 1025, 1028, 1066, 3 },
	{ 1440,  900, 60, 106500, 1520, 1672, 1904,  903,  909,  934, 2 },
	{ 1600, 1200, 60, 162000, 1664, 1856, 2160, 1201, 1204, 1250, 3 },
	{ 1680, 1050, 60, 146250, 1784, 1960, 2240, 1053, 1059, 1089, 2 },
	{ 1920, 1200, 60, 193250, 2056, 2256, 2592, 1203, 1209, 1245, 2 },
};

/* The established timings, bit by bit from byte 35 bit 7: those of the table above, 0 for the others */
LOCAL CONST UH	est_bits[17][3] = {
	{ 720, 400, 70 }, { 0, 0, 0 }, { 640, 480, 60 }, { 0, 0, 0 },
	{ 640, 480, 72 }, { 640, 480, 75 }, { 800, 600, 56 }, { 800, 600, 60 },
	{ 800, 600, 72 }, { 800, 600, 75 }, { 0, 0, 0 }, { 0, 0, 0 },
	{ 1024, 768, 60 }, { 1024, 768, 70 }, { 1024, 768, 75 }, { 1280, 1024, 75 },
	{ 0, 0, 0 },
};

/* CTA-861 video identification codes: the ones of progressive modes in common use */
typedef struct {
	UB	vic;
	MTAB	m;
} VTAB;

LOCAL CONST VTAB	vics[] = {
	{  1, {  640,  480, 60,  25175,  656,  752,  800,  490,  492,  525, 0 } },
	{  2, {  720,  480, 60,  27000,  736,  798,  858,  489,  495,  525, 0 } },
	{  3, {  720,  480, 60,  27000,  736,  798,  858,  489,  495,  525, 0 } },
	{  4, { 1280,  720, 60,  74250, 1390, 1430, 1650,  725,  730,  750, 3 } },
	{ 16, { 1920, 1080, 60, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 3 } },
	{ 17, {  720,  576, 50,  27000,  732,  796,  864,  581,  586,  625, 0 } },
	{ 18, {  720,  576, 50,  27000,  732,  796,  864,  581,  586,  625, 0 } },
	{ 19, { 1280,  720, 50,  74250, 1720, 1760, 1980,  725,  730,  750, 3 } },
	{ 31, { 1920, 1080, 50, 148500, 2448, 2492, 2640, 1084, 1089, 1125, 3 } },
	{ 32, { 1920, 1080, 24,  74250, 2558, 2602, 2750, 1084, 1089, 1125, 3 } },
	{ 33, { 1920, 1080, 25,  74250, 2448, 2492, 2640, 1084, 1089, 1125, 3 } },
	{ 34, { 1920, 1080, 30,  74250, 2008, 2052, 2200, 1084, 1089, 1125, 3 } },
	{ 93, { 3840, 2160, 24, 297000, 5116, 5204, 5500, 2168, 2178, 2250, 3 } },
	{ 94, { 3840, 2160, 25, 297000, 4896, 4984, 5280, 2168, 2178, 2250, 3 } },
	{ 95, { 3840, 2160, 30, 297000, 4016, 4104, 4400, 2168, 2178, 2250, 3 } },
	{ 96, { 3840, 2160, 50, 594000, 4896, 4984, 5280, 2168, 2178, 2250, 3 } },
	{ 97, { 3840, 2160, 60, 594000, 4016, 4104, 4400, 2168, 2178, 2250, 3 } },
};

#define MLIST_FIRST	32		/* modes the list is first taken for; it doubles when full */

typedef struct {
	T_DISPMODE	*m;
	INT		n, max;
	BOOL		nomem;		/* a mode found no room: the parse ends with E_NOMEM */
} MLIST;

LOCAL UH refresh_of( CONST T_DISPMODE *m )
{
	UD	f = (UD)m->htotal * m->vtotal;

	if ( f == 0 ) return 0;
	return (UH)( ( (UD)m->clock_khz * 1000 + f / 2 ) / f * ( ( m->flags & DISP_MODE_INTERLACE ) ? 2 : 1 ) );
}

/* A mode put in the list, unless one of its size, refresh and interlacing is there already */
LOCAL void add( MLIST *l, CONST T_DISPMODE *m )
{
	INT	i;

	if ( m->hdisp == 0 || m->vdisp == 0 || m->clock_khz == 0 ) {
		return;
	}
	for ( i = 0; i < l->n; i++ ) {
		CONST T_DISPMODE *o = &l->m[i];

		if ( o->hdisp == m->hdisp && o->vdisp == m->vdisp
		  && ( o->vrefresh + 1 >= m->vrefresh && o->vrefresh <= m->vrefresh + 1 )
		  && ( o->flags & DISP_MODE_INTERLACE ) == ( m->flags & DISP_MODE_INTERLACE ) ) {
			return;
		}
	}
	if ( l->n >= l->max ) {
		INT		max = ( l->max > 0 ) ? l->max * 2 : MLIST_FIRST;
		T_DISPMODE	*b = (T_DISPMODE *)Kmalloc(sizeof(T_DISPMODE) * (SZ)max);

		if ( b == NULL ) {
			l->nomem = TRUE;
			return;
		}
		if ( l->m != NULL ) {
			knl_memcpy(b, l->m, (SZ)l->n * sizeof(T_DISPMODE));
			Kfree(l->m);
		}
		l->m = b;
		l->max = max;
	}
	l->m[l->n++] = *m;
}

LOCAL void from_tab( T_DISPMODE *m, CONST MTAB *t, UH flags )
{
	m->clock_khz = t->khz;
	m->hdisp = t->w;  m->hss = t->hss;  m->hse = t->hse;  m->htotal = t->ht;
	m->vdisp = t->h;  m->vss = t->vss;  m->vse = t->vse;  m->vtotal = t->vt;
	m->flags = flags | ( ( t->pol & 1 ) ? DISP_MODE_PHSYNC : 0 ) | ( ( t->pol & 2 ) ? DISP_MODE_PVSYNC : 0 );
	m->vrefresh = refresh_of(m);
}

/*
 * A size and refresh timed by CVT with reduced blanking (VESA CVT 1.1):
 * a blank of 160 pixels across, 32 of them the sync after 48; at least
 * 460 us of lines down, 3 lines before the sync, which is as long as the
 * shape says
 */
LOCAL void cvt_rb( T_DISPMODE *m, UINT w, UINT h, UINT hz, UINT aspect )
{
	UINT	vsync = ( aspect == 1 ) ? 4 : ( aspect == 3 ) ? 5 : ( aspect == 0 ) ? 6 : 7;
	UD	line_ns, vblank;
	UINT	ht = w + 160, vt;

	if ( hz == 0 ) return;
	/* the time of a line, from the frame less 460 us of blank, over the lines shown */
	line_ns = ( 1000000000ULL / hz - 460000ULL ) / h;
	vblank = ( 460000ULL + line_ns - 1 ) / line_ns + 1;
	if ( vblank < 3 + vsync + 6 ) vblank = 3 + vsync + 6;
	vt = h + (UINT)vblank;
	m->hdisp = (UH)w;  m->hss = (UH)( w + 48 );  m->hse = (UH)( w + 80 );  m->htotal = (UH)ht;
	m->vdisp = (UH)h;  m->vss = (UH)( h + 3 );  m->vse = (UH)( h + 3 + vsync );  m->vtotal = (UH)vt;
	/* the clock in steps of 250 kHz, as CVT rounds it */
	m->clock_khz = (UW)( ( (UD)ht * vt * hz / 1000 ) / 250 * 250 );
	m->flags = DISP_MODE_PHSYNC;
	m->vrefresh = refresh_of(m);
}

/* A detailed timing descriptor, 18 bytes: FALSE when it is a display descriptor instead */
LOCAL BOOL dtd( CONST UB *d, T_DISPMODE *m, UH *p_mmw, UH *p_mmh )
{
	UINT	clk = d[0] | ( d[1] << 8 );
	UINT	ha, hb, va, vb, hso, hsw, vso, vsw;

	if ( clk == 0 ) {
		return FALSE;
	}
	ha = d[2] | ( ( d[4] >> 4 ) << 8 );
	hb = d[3] | ( ( d[4] & 0x0F ) << 8 );
	va = d[5] | ( ( d[7] >> 4 ) << 8 );
	vb = d[6] | ( ( d[7] & 0x0F ) << 8 );
	hso = d[8] | ( ( ( d[11] >> 6 ) & 3 ) << 8 );
	hsw = d[9] | ( ( ( d[11] >> 4 ) & 3 ) << 8 );
	vso = ( d[10] >> 4 ) | ( ( ( d[11] >> 2 ) & 3 ) << 4 );
	vsw = ( d[10] & 0x0F ) | ( ( d[11] & 3 ) << 4 );
	knl_memset(m, 0, sizeof(*m));
	m->clock_khz = clk * 10;
	m->hdisp = (UH)ha;  m->hss = (UH)( ha + hso );  m->hse = (UH)( ha + hso + hsw );  m->htotal = (UH)( ha + hb );
	m->vdisp = (UH)va;  m->vss = (UH)( va + vso );  m->vse = (UH)( va + vso + vsw );  m->vtotal = (UH)( va + vb );
	m->flags = DISP_MODE_DTD;
	if ( d[17] & 0x80 ) m->flags |= DISP_MODE_INTERLACE;
	if ( ( d[17] & 0x18 ) == 0x18 ) {
		if ( d[17] & 0x04 ) m->flags |= DISP_MODE_PVSYNC;
		if ( d[17] & 0x02 ) m->flags |= DISP_MODE_PHSYNC;
	}
	m->vrefresh = refresh_of(m);
	if ( p_mmw != NULL && *p_mmw == 0 ) {
		*p_mmw = (UH)( d[12] | ( ( d[14] >> 4 ) << 8 ) );
		*p_mmh = (UH)( d[13] | ( ( d[14] & 0x0F ) << 8 ) );
	}
	return TRUE;
}

/* A standard timing, two bytes: its mode from the table, else by CVT */
LOCAL void std_timing( MLIST *l, UB a, UB b, UINT ver_rev )
{
	T_DISPMODE	m;
	UINT		w, h, hz, aspect, i;

	if ( ( a == 0x01 && b == 0x01 ) || a == 0x00 ) {
		return;				/* not used */
	}
	w = ( (UINT)a + 31 ) * 8;
	aspect = b >> 6;
	hz = ( b & 0x3F ) + 60;
	switch ( aspect ) {
	case 0:	 h = ( ver_rev < 3 ) ? w : w * 10 / 16; break;	/* 16:10 from EDID 1.3, 1:1 before */
	case 1:	 h = w * 3 / 4; break;
	case 2:	 h = w * 4 / 5; break;
	default: h = w * 9 / 16; break;
	}
	knl_memset(&m, 0, sizeof(m));
	for ( i = 0; i < sizeof(dmt) / sizeof(dmt[0]); i++ ) {
		if ( dmt[i].w == w && dmt[i].h == h && dmt[i].hz == hz ) {
			from_tab(&m, &dmt[i], DISP_MODE_STD);
			add(l, &m);
			return;
		}
	}
	cvt_rb(&m, w, h, hz, aspect);
	m.flags |= DISP_MODE_STD;
	add(l, &m);
}

/* Whether a block's 128 bytes sum to 0 */
LOCAL BOOL block_ok( CONST UB *b )
{
	UB	s = 0;
	INT	i;

	for ( i = 0; i < 128; i++ ) {
		s = (UB)( s + b[i] );
	}
	return ( s == 0 );
}

EXPORT INT knl_edid_parse( CONST UB *e, INT len, T_EDIDINFO *info, T_DISPMODE **p_modes )
{
	CONST UB	hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
	MLIST		l;
	T_DISPMODE	m;
	INT		i, k, nblk, b;
	UINT		ver_rev;
	UH		mmw = 0, mmh = 0;

	knl_memset(info, 0, sizeof(*info));
	*p_modes = NULL;
	l.m = NULL;
	l.n = 0;
	l.max = 0;
	l.nomem = FALSE;
	if ( e == NULL || len < 128 ) {
		return E_PAR;
	}
	for ( i = 0; i < 8; i++ ) {
		if ( e[i] != hdr[i] ) return E_PAR;
	}
	if ( !block_ok(e) ) {
		return E_PAR;
	}
	ver_rev = e[19];
	info->maker[0] = (char)( '@' + ( ( e[8] >> 2 ) & 0x1F ) );
	info->maker[1] = (char)( '@' + ( ( ( e[8] & 3 ) << 3 ) | ( e[9] >> 5 ) ) );
	info->maker[2] = (char)( '@' + ( e[9] & 0x1F ) );
	info->maker[3] = 0;
	info->product = (UH)( e[10] | ( e[11] << 8 ) );
	info->serial = (UW)e[12] | ( (UW)e[13] << 8 ) | ( (UW)e[14] << 16 ) | ( (UW)e[15] << 24 );
	info->ident = ( (UW)e[8] << 24 | (UW)e[9] << 16 | info->product ) ^ info->serial;
	nblk = 1 + e[126];
	if ( nblk * 128 > len ) nblk = len / 128;
	info->blocks = nblk;

	/* the detailed timings of the base block; the first one the preferred */
	for ( k = 0; k < 4; k++ ) {
		CONST UB	*d = e + 54 + 18 * k;

		if ( dtd(d, &m, &mmw, &mmh) ) {
			if ( l.n == 0 ) m.flags |= DISP_MODE_PREFERRED;
			add(&l, &m);
		} else if ( d[3] == 0xFC ) {
			for ( i = 0; i < 13 && d[5 + i] != 0x0A && i < (INT)sizeof(info->model) - 1; i++ ) {
				info->model[i] = (char)d[5 + i];
			}
			while ( i > 0 && info->model[i - 1] == ' ' ) i--;
			info->model[i] = 0;
		}
	}
	/* and of the CTA extensions */
	for ( b = 1; b < nblk; b++ ) {
		CONST UB	*x = e + 128 * b;
		INT		off;

		if ( x[0] != 0x02 || !block_ok(x) ) continue;
		off = x[2];
		if ( off < 4 || off > 127 ) continue;
		for ( k = off; k + 18 <= 127; k += 18 ) {
			if ( dtd(x + k, &m, NULL, NULL) ) add(&l, &m);
		}
	}
	/* the standard timings, of the block and of its descriptors */
	for ( k = 0; k < 8; k++ ) {
		std_timing(&l, e[38 + 2 * k], e[39 + 2 * k], ver_rev);
	}
	for ( k = 0; k < 4; k++ ) {
		CONST UB	*d = e + 54 + 18 * k;

		if ( d[0] == 0 && d[1] == 0 && d[3] == 0xFA ) {
			for ( i = 0; i < 6; i++ ) {
				std_timing(&l, d[5 + 2 * i], d[6 + 2 * i], ver_rev);
			}
		}
	}
	/* the established timings */
	for ( k = 0; k < 17; k++ ) {
		UINT	byte = 35 + k / 8, bit = 7 - ( k % 8 );
		UINT	j;

		if ( ( e[byte] & ( 1U << bit ) ) == 0 || est_bits[k][0] == 0 ) continue;
		for ( j = 0; j < sizeof(dmt) / sizeof(dmt[0]); j++ ) {
			if ( dmt[j].w == est_bits[k][0] && dmt[j].h == est_bits[k][1] && dmt[j].hz == est_bits[k][2] ) {
				from_tab(&m, &dmt[j], DISP_MODE_EST);
				add(&l, &m);
				break;
			}
		}
	}
	/* the CTA extensions' data blocks: their video codes, sound and HDMI's identifier */
	for ( b = 1; b < nblk; b++ ) {
		CONST UB	*x = e + 128 * b;
		INT		off, p;

		if ( x[0] != 0x02 || !block_ok(x) ) continue;
		off = x[2];
		if ( x[1] >= 2 && ( x[3] & 0x40 ) ) info->audio = TRUE;	/* basic audio */
		for ( p = 4; p < off && p < 127; p += 1 + ( x[p] & 0x1F ) ) {
			UINT	tag = x[p] >> 5, n = x[p] & 0x1F;

			if ( tag == 1 && n > 0 ) {
				info->audio = TRUE;
			} else if ( tag == 3 && n >= 3 && x[p + 1] == 0x03 && x[p + 2] == 0x0C && x[p + 3] == 0x00 ) {
				info->hdmi = TRUE;
			} else if ( tag == 2 ) {
				for ( i = 1; i <= (INT)n && p + i < 127; i++ ) {
					UINT	v = x[p + i], vic, j;

					vic = ( v >= 129 && v <= 192 ) ? v - 128 : v;
					for ( j = 0; j < sizeof(vics) / sizeof(vics[0]); j++ ) {
						if ( vics[j].vic == vic ) {
							from_tab(&m, &vics[j].m, DISP_MODE_VIC);
							add(&l, &m);
							break;
						}
					}
				}
			}
		}
	}
	if ( mmw == 0 && e[21] != 0 && e[22] != 0 ) {
		mmw = (UH)( e[21] * 10 );
		mmh = (UH)( e[22] * 10 );
	}
	info->mm_w = mmw;
	info->mm_h = mmh;
	if ( l.nomem ) {
		if ( l.m != NULL ) Kfree(l.m);
		return E_NOMEM;
	}
	*p_modes = l.m;
	return l.n;
}
