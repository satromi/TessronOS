/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	emu_pad.h
 *	USB game controllers for BeSFC (design 11.32), as the program USB devices
 *	the class drivers leave (design 6.3.1). The first controller found is pad
 *	1 and the second pad 2. HID controllers are read by their report
 *	descriptors; the Xbox 360 kind has no descriptor and a fixed report.
 *	Nothing waits: each frame takes the reports that have come, and a
 *	controller that goes away is looked for again every few seconds.
 *	Included by one source file; the parser knows nothing of the system.
 */

#ifndef EMU_PAD_H
#define EMU_PAD_H

#define PAD_UNITS	2
#define PAD_LOOK_MS	3000		/* how often the devices are looked over */
#define PAD_REPORT_MAX	64
#define PAD_DESC_MAX	1024
#define PAD_DRAIN_MAX	16		/* reports taken in a frame */
#define PAD_F_UP	0x1
#define PAD_F_DOWN	0x2
#define PAD_F_LEFT	0x4
#define PAD_F_RIGHT	0x8

/* Where the state of a controller is in its report, in bits; a size of 0 is absent */
typedef struct {
	INT	report_id;		/* -1: reports have no number */
	INT	btn_off, btn_cnt;
	INT	x_off, x_size, x_min, x_max;
	INT	y_off, y_size, y_min, y_max;
	INT	hat_off, hat_size, hat_min, hat_max;
	INT	dpad_off[4];		/* up, down, right, left; -1 absent */
} PADMAP;

/* The bits of a report from off, as unsigned */
static UINT pad_bits( CONST UB *b, INT n, INT off, INT size )
{
	UINT	v = 0;
	INT	i;

	for ( i = 0; i < size && i < 32; i++ ) {
		INT	at = off + i;

		if ( at / 8 < n && ( ( b[at / 8] >> ( at % 8 ) ) & 1 ) != 0 ) v |= 1U << i;
	}
	return v;
}

/* As the logical range says: a range with a negative start is signed */
static INT pad_axis( CONST UB *b, INT n, INT off, INT size, INT min )
{
	UINT	v = pad_bits(b, n, off, size);

	if ( min < 0 && size < 32 && ( v >> ( size - 1 ) ) != 0 ) v |= ~0U << size;
	return (INT)v;
}

/*
 * The layout of the first report with a controller's fields. Answers whether
 * the descriptor's top collection is a joystick or a game pad with any button.
 */
static BOOL pad_parse( CONST UB *d, INT n, PADMAP *m )
{
	UINT	page = 0, usage[32], umin = 0, umax = 0;
	INT	nusage = 0, lmin = 0, lmax = 0, rsize = 0, rcount = 0, rid = -1;
	INT	depth = 0, i = 0, pos = 0, last_rid = -1;
	BOOL	ispad = FALSE, have = FALSE;

	memset(m, 0, sizeof(*m));
	m->report_id = -1;
	for ( i = 0; i < 4; i++ ) m->dpad_off[i] = -1;
	i = 0;
	while ( i < n ) {
		UB	pre = d[i++];
		INT	size = pre & 3, type = ( pre >> 2 ) & 3, tag = pre >> 4, k;
		UINT	u = 0;
		INT	s;

		if ( size == 3 ) size = 4;
		if ( pre == 0xFE ) {		/* a long item */
			if ( i < n ) i += d[i] + 2;
			continue;
		}
		if ( i + size > n ) break;
		for ( k = 0; k < size; k++ ) u |= (UINT)d[i + k] << ( 8 * k );
		i += size;
		s = (INT)u;
		if ( size == 1 ) s = (signed char)u;
		else if ( size == 2 ) s = (short)u;
		if ( type == 1 ) {		/* global */
			if ( tag == 0 ) page = u;
			else if ( tag == 1 ) lmin = s;
			else if ( tag == 2 ) lmax = s;
			else if ( tag == 7 ) rsize = (INT)u;
			else if ( tag == 8 ) rid = (INT)u;
			else if ( tag == 9 ) rcount = (INT)u;
			if ( tag == 2 && lmin >= 0 && lmax < 0 ) lmax = (INT)u;
		} else if ( type == 2 ) {	/* local */
			UINT	full = ( size == 4 ) ? u : ( page << 16 ) | u;

			if ( tag == 0 && nusage < 32 ) usage[nusage++] = full;
			else if ( tag == 1 ) umin = full;
			else if ( tag == 2 ) umax = full;
		} else if ( type == 0 ) {	/* main */
			if ( tag == 10 ) {	/* a collection */
				if ( depth == 0 && nusage > 0 && ( u & 0xff ) == 1
				  && ( usage[0] == 0x00010004U || usage[0] == 0x00010005U ) ) ispad = TRUE;
				depth++;
			} else if ( tag == 12 ) {
				depth--;
			} else if ( tag == 8 ) {	/* an input */
				if ( rid != last_rid ) {
					if ( have ) break;	/* the first report that has fields is the one read */
					last_rid = rid;
					pos = 0;
				}
				for ( k = 0; k < rcount; k++, pos += rsize ) {
					UINT	us;

					if ( ( u & 1 ) != 0 ) continue;	/* a constant: padding */
					if ( umax > umin ) us = umin + (UINT)k;
					else if ( nusage > 0 ) us = usage[k < nusage ? k : nusage - 1];
					else continue;
					if ( ( us >> 16 ) == 9 ) {
						if ( m->btn_cnt == 0 ) m->btn_off = pos;
						if ( pos == m->btn_off + m->btn_cnt ) m->btn_cnt++;
						have = TRUE;
					} else if ( us == 0x00010030U && m->x_size == 0 ) {
						m->x_off = pos; m->x_size = rsize; m->x_min = lmin; m->x_max = lmax;
						have = TRUE;
					} else if ( us == 0x00010031U && m->y_size == 0 ) {
						m->y_off = pos; m->y_size = rsize; m->y_min = lmin; m->y_max = lmax;
						have = TRUE;
					} else if ( us == 0x00010039U && m->hat_size == 0 ) {
						m->hat_off = pos; m->hat_size = rsize; m->hat_min = lmin; m->hat_max = lmax;
						have = TRUE;
					} else if ( us >= 0x00010090U && us <= 0x00010093U ) {
						m->dpad_off[us - 0x00010090U] = pos;
						have = TRUE;
					}
				}
				if ( have ) m->report_id = rid;
			}
			nusage = 0;
			umin = umax = 0;
		}
	}
	return (BOOL)( ispad && m->btn_cnt > 0 );
}

/* The directions a stick's value is past half way to */
static UINT pad_stick( INT v, INT min, INT max, UINT less, UINT more )
{
	INT	mid = min + ( max - min ) / 2, q = ( max - min ) / 4;

	if ( q <= 0 ) return 0;
	return ( v < mid - q ) ? less : ( v > mid + q ) ? more : 0;
}

/* The buttons of an HID report: 1 Y, 2 B, 3 A, 4 X, 5 L, 6 R, 9 select, 10 start */
static uint16_t pad_hid( CONST UB *b, INT n, CONST PADMAP *m )
{
	static CONST uint16_t	btn[10] = {
		SFC_BTN_Y, SFC_BTN_B, SFC_BTN_A, SFC_BTN_X, SFC_BTN_L, SFC_BTN_R, 0, 0, SFC_BTN_SELECT, SFC_BTN_START
	};
	static CONST UB		hat[8] = {
		PAD_F_UP, PAD_F_UP | PAD_F_RIGHT, PAD_F_RIGHT, PAD_F_RIGHT | PAD_F_DOWN,
		PAD_F_DOWN, PAD_F_DOWN | PAD_F_LEFT, PAD_F_LEFT, PAD_F_LEFT | PAD_F_UP
	};
	uint16_t	r = 0;
	UINT		f = 0;
	INT		i;

	for ( i = 0; i < m->btn_cnt && i < 10; i++ ) {
		if ( pad_bits(b, n, m->btn_off + i, 1) != 0 ) r |= btn[i];
	}
	if ( m->x_size > 0 ) f |= pad_stick(pad_axis(b, n, m->x_off, m->x_size, m->x_min), m->x_min, m->x_max, PAD_F_LEFT, PAD_F_RIGHT);
	if ( m->y_size > 0 ) f |= pad_stick(pad_axis(b, n, m->y_off, m->y_size, m->y_min), m->y_min, m->y_max, PAD_F_UP, PAD_F_DOWN);
	if ( m->hat_size > 0 ) {
		INT	h = pad_axis(b, n, m->hat_off, m->hat_size, m->hat_min) - m->hat_min;

		if ( h >= 0 && h < 8 && m->hat_max - m->hat_min == 7 ) f |= hat[h];
	}
	if ( m->dpad_off[0] >= 0 && pad_bits(b, n, m->dpad_off[0], 1) != 0 ) f |= PAD_F_UP;
	if ( m->dpad_off[1] >= 0 && pad_bits(b, n, m->dpad_off[1], 1) != 0 ) f |= PAD_F_DOWN;
	if ( m->dpad_off[2] >= 0 && pad_bits(b, n, m->dpad_off[2], 1) != 0 ) f |= PAD_F_RIGHT;
	if ( m->dpad_off[3] >= 0 && pad_bits(b, n, m->dpad_off[3], 1) != 0 ) f |= PAD_F_LEFT;
	if ( ( f & PAD_F_UP ) != 0 ) r |= SFC_BTN_UP;
	if ( ( f & PAD_F_DOWN ) != 0 ) r |= SFC_BTN_DOWN;
	if ( ( f & PAD_F_LEFT ) != 0 ) r |= SFC_BTN_LEFT;
	if ( ( f & PAD_F_RIGHT ) != 0 ) r |= SFC_BTN_RIGHT;
	return r;
}

/* The Xbox 360 report: its buttons in bytes 2 and 3, the left stick from byte 6 */
static uint16_t pad_x360( CONST UB *b, INT n )
{
	uint16_t	r = 0;
	UINT		lx, ly;

	if ( n < 10 || b[0] != 0 ) return 0;
	if ( ( b[2] & 0x01 ) != 0 ) r |= SFC_BTN_UP;
	if ( ( b[2] & 0x02 ) != 0 ) r |= SFC_BTN_DOWN;
	if ( ( b[2] & 0x04 ) != 0 ) r |= SFC_BTN_LEFT;
	if ( ( b[2] & 0x08 ) != 0 ) r |= SFC_BTN_RIGHT;
	if ( ( b[2] & 0x10 ) != 0 ) r |= SFC_BTN_START;
	if ( ( b[2] & 0x20 ) != 0 ) r |= SFC_BTN_SELECT;
	if ( ( b[3] & 0x01 ) != 0 ) r |= SFC_BTN_L;
	if ( ( b[3] & 0x02 ) != 0 ) r |= SFC_BTN_R;
	if ( ( b[3] & 0x10 ) != 0 ) r |= SFC_BTN_A;
	if ( ( b[3] & 0x20 ) != 0 ) r |= SFC_BTN_B;
	if ( ( b[3] & 0x40 ) != 0 ) r |= SFC_BTN_X;
	if ( ( b[3] & 0x80 ) != 0 ) r |= SFC_BTN_Y;
	lx = pad_stick((short)( b[6] | ( b[7] << 8 ) ), -32768, 32767, 1, 2);
	ly = pad_stick((short)( b[8] | ( b[9] << 8 ) ), -32768, 32767, 1, 2);
	if ( lx == 1 ) r |= SFC_BTN_LEFT;
	if ( lx == 2 ) r |= SFC_BTN_RIGHT;
	if ( ly == 1 ) r |= SFC_BTN_DOWN;
	if ( ly == 2 ) r |= SFC_BTN_UP;
	return r;
}

#ifndef EMU_PAD_PARSER_ONLY

typedef struct {
	INT		ud;		/* open, 0 not */
	INT		dev;
	UINT		ep;
	BOOL		x360;
	PADMAP		map;
	uint16_t	buttons;
} PADUNIT;

static PADUNIT	g_units[PAD_UNITS];
static UD	g_pad_look = 0;

/* A device opened if it is a controller; answers whether it became unit u */
static BOOL pad_try( PADUNIT *u, CONST T_USBDEV *d )
{
	static UB	cfg[512], rd[PAD_DESC_MAX];
	INT		n = us_get_dsc(d->dev, 2, 0, cfg, sizeof(cfg)), i, ifno = -1, ud, rlen = 0;
	UINT		ep = 0;
	BOOL		x360 = FALSE, want = FALSE;

	if ( n < 9 ) return FALSE;
	for ( i = 0; i + 2 <= n && cfg[i] >= 2 && i + cfg[i] <= n && ep == 0; i += cfg[i] ) {
		if ( cfg[i + 1] == 4 && cfg[i] >= 9 ) {		/* an interface */
			BOOL	hid = (BOOL)( cfg[i + 5] == 3 );
			BOOL	xb = (BOOL)( cfg[i + 5] == 0xFF && cfg[i + 6] == 0x5D && cfg[i + 7] == 1 );

			want = (BOOL)( hid || xb );
			if ( want ) {
				ifno = cfg[i + 2];
				x360 = xb;
				rlen = 0;
			}
		} else if ( want && cfg[i + 1] == 0x21 && cfg[i] >= 9 ) {	/* its HID descriptor */
			rlen = cfg[i + 7] | ( cfg[i + 8] << 8 );
		} else if ( want && cfg[i + 1] == 5 && cfg[i] >= 7
			 && ( cfg[i + 2] & 0x80 ) != 0 && ( cfg[i + 3] & 3 ) == 3 ) {	/* interrupt IN */
			ep = cfg[i + 2];
		}
	}
	if ( ep == 0 ) return FALSE;
	if ( rlen > PAD_DESC_MAX ) rlen = PAD_DESC_MAX;
	ud = us_opn_dev(d->dev);
	if ( ud <= 0 ) return FALSE;
	if ( !x360 ) {
		UB	setup[8] = { 0x81, 6, 0, 0x22, (UB)ifno, 0, (UB)rlen, (UB)( rlen >> 8 ) };

		rlen = us_ctl(ud, setup, rd, rlen, 1000);
		if ( rlen <= 0 || !pad_parse(rd, rlen, &u->map) ) {
			(void)us_cls_dev(ud);
			return FALSE;
		}
	}
	u->ud = ud;
	u->dev = d->dev;
	u->ep = ep;
	u->x360 = x360;
	u->buttons = 0;
	return TRUE;
}

static void pad_look( void )
{
	T_USBDEV	list[16];
	INT		n = us_lst_dev(list, 16), i, j;

	for ( i = 0; i < n; i++ ) {
		BOOL	used = (BOOL)( list[i].role != USB_ROLE_NONE );

		for ( j = 0; j < PAD_UNITS; j++ ) {
			if ( g_units[j].ud > 0 && g_units[j].dev == list[i].dev ) used = TRUE;
		}
		for ( j = 0; !used && j < PAD_UNITS; j++ ) {
			if ( g_units[j].ud <= 0 ) {
				if ( pad_try(&g_units[j], &list[i]) ) say("game controller %d: device %d\n", j + 1, list[i].dev);
				break;
			}
		}
	}
}

/* The buttons of a unit now, from the reports that have come */
static uint16_t pad_read( INT unit )
{
	PADUNIT	*u = &g_units[unit];
	UB	b[PAD_REPORT_MAX];
	INT	i;

	if ( unit == 0 ) {
		UD	now = ui_now();

		if ( now - g_pad_look >= PAD_LOOK_MS ) {
			g_pad_look = now;
			pad_look();
		}
	}
	if ( u->ud <= 0 ) return 0;
	for ( i = 0; i < PAD_DRAIN_MAX; i++ ) {
		INT	n = us_xfer(u->ud, u->ep, b, sizeof(b), TMO_POL);

		if ( n == E_TMOUT ) break;
		if ( n < 0 ) {		/* it is gone */
			(void)us_cls_dev(u->ud);
			u->ud = 0;
			u->buttons = 0;
			say("game controller %d: gone\n", unit + 1, 0);
			break;
		}
		if ( u->x360 ) {
			if ( n >= 10 && b[1] >= 14 ) u->buttons = pad_x360(b, n);
		} else if ( u->map.report_id < 0 ) {
			u->buttons = pad_hid(b, n, &u->map);
		} else if ( n >= 1 && b[0] == u->map.report_id ) {
			u->buttons = pad_hid(b + 1, n - 1, &u->map);
		}
	}
	return u->buttons;
}

static void pad_close( void )
{
	INT	i;

	for ( i = 0; i < PAD_UNITS; i++ ) {
		if ( g_units[i].ud > 0 ) (void)us_cls_dev(g_units[i].ud);
		g_units[i].ud = 0;
	}
}

#endif /* EMU_PAD_PARSER_ONLY */

#endif /* EMU_PAD_H */
