/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_cam.c
 *	The camera device "cama" (design 10.16), with the kernel's own camera
 *	(knl_cam_loopback): no camera, then one; its modes; a mode picked and
 *	the stream started; frames handed to it read back, each once, with
 *	their header; the counts; the device's object with its records.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/cam.h>
#include <ts/ob.h>

LOCAL UB	frame[64 * 48 * 2];
LOCAL UB	got[sizeof(T_CAMFRAME) + 64 * 48 * 2];

LOCAL BOOL same( CONST UB *a, CONST UB *b, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL ER cam_r(ID dd, W dn, void *buf, SZ size, SZ *asz )
{
	return tk_srea_dev(dd, dn, buf, size, asz);
}

LOCAL ER cam_w( ID dd, W dn, CONST void *buf, SZ size )
{
	SZ	asz = 0;

	return tk_swri_dev(dd, dn, buf, size, &asz);
}

/* On the board a camera may be plugged in: the tests of the kernel's own camera, which take unit 0, are then left */
LOCAL BOOL unit0_taken( void )
{
	T_CAMINFO	in;
	SZ		asz = 0;
	BOOL		taken = FALSE;
	ID		dd = tk_opn_dev((UB *)CAM_DEVNM, TD_READ);

	if ( dd > 0 ) {
		taken = ( cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz) >= E_OK && in.state != CAMST_NODEV );
		tk_cls_dev(dd, 0);
	}
	return taken;
}

#define LEAVE_IF_PLUGGED()	do { if ( KT_BOARD && unit0_taken() ) KT_SKIP("a USB camera is plugged in"); } while (0)

/* no camera on the unit: it says so */
LOCAL void test_nocam( void )
{
	T_CAMINFO	in;
	SZ		asz = 0;
	ID		dd = tk_opn_dev((UB *)CAM_DEVNM, TD_UPDATE);

	LEAVE_IF_PLUGGED();
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	KT_ASSERT_ER(cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(in));
	KT_ASSERT_EQ(in.state, CAMST_NODEV);
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_NOEXS);
	tk_cls_dev(dd, 0);
}

/* the kernel's own camera: its modes, a stream, its frames each once */
LOCAL void test_loopback( void )
{
	T_CAMINFO	in;
	T_CAMMODE	modes[4];
	T_CAMSETMODE	m;
	T_CAMFRAME	h;
	T_CAMSTAT	st;
	SZ		asz = 0;
	W		v;
	INT		i;
	ID		dd;

	LEAVE_IF_PLUGGED();
	knl_cam_loopback(TRUE);
	dd = tk_opn_dev((UB *)CAM_DEVNM, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) {
		knl_cam_loopback(FALSE);
		return;
	}
	KT_ASSERT_ER(cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz), E_OK);
	KT_ASSERT_EQ(in.state, CAMST_READY);
	KT_ASSERT_EQ(in.nmodes, 2);
	KT_ASSERT_EQ(in.curMode, -1);
	KT_ASSERT(knl_strcmp((CONST char *)in.product, "Test camera") == 0);
	KT_ASSERT_ER(cam_r(dd, DN_CAMMODES, NULL, 0, &asz), E_OK);
	KT_ASSERT_EQ(asz, 2 * sizeof(T_CAMMODE));
	KT_ASSERT_ER(cam_r(dd, DN_CAMMODES, modes, sizeof(modes), &asz), E_OK);
	KT_ASSERT_EQ(modes[0].format, CAMFMT_YUY2);
	KT_ASSERT_EQ(modes[1].format, CAMFMT_MJPEG);
	KT_ASSERT(modes[0].width == 64 && modes[0].height == 48);

	/* no stream before a mode is picked; then one */
	v = CAM_START;
	KT_ASSERT_ER(cam_w(dd, DN_CAMSTREAM, &v, sizeof(v)), E_OBJ);
	knl_memset(&m, 0, sizeof(m));
	m.mode = 0;
	KT_ASSERT_ER(cam_w(dd, DN_CAMMODE, &m, sizeof(m)), E_OK);
	KT_ASSERT_ER(cam_r(dd, DN_CAMMODE, &m, sizeof(m), &asz), E_OK);
	KT_ASSERT_EQ(m.maxFrame, 64 * 48 * 2);
	KT_ASSERT_ER(cam_w(dd, DN_CAMSTREAM, &v, sizeof(v)), E_OK);
	KT_ASSERT_ER(cam_r(dd, DN_CAMSTREAM, &v, sizeof(v), &asz), E_OK);
	KT_ASSERT_EQ(v, CAM_START);

	/* nothing yet; a frame; read once */
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);
	for ( i = 0; i < (INT)sizeof(frame); i++ ) frame[i] = (UB)( i * 7 );
	knl_cam_feed(frame, sizeof(frame));
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, NULL, 0, &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + sizeof(frame));
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + sizeof(frame));
	knl_memcpy(&h, got, sizeof(h));
	KT_ASSERT_EQ(h.seq, 1);
	KT_ASSERT_EQ(h.format, CAMFMT_YUY2);
	KT_ASSERT(h.width == 64 && h.height == 48);
	KT_ASSERT_EQ(h.bytes, sizeof(frame));
	KT_ASSERT_EQ(h.flags, 0);
	KT_ASSERT(same(got + sizeof(h), frame, sizeof(frame)));
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);				/* had already */

	/* two more: the newest is given; a short buffer gets the head of it */
	knl_cam_feed(frame, sizeof(frame));
	knl_cam_feed(frame, sizeof(frame));
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(T_CAMFRAME) + 100, &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + 100);
	knl_memcpy(&h, got, sizeof(h));
	KT_ASSERT_EQ(h.seq, 3);
	KT_ASSERT_EQ(h.flags, CAMFR_TRUNC);
	KT_ASSERT_ER(cam_r(dd, DN_CAMSTAT, &st, sizeof(st), &asz), E_OK);
	KT_ASSERT_EQ(st.frames, 3);
	KT_ASSERT_EQ(st.delivered, 2);

	/* stopped: a frame handed in is dropped */
	v = CAM_STOP;
	KT_ASSERT_ER(cam_w(dd, DN_CAMSTREAM, &v, sizeof(v)), E_OK);
	knl_cam_feed(frame, sizeof(frame));
	KT_ASSERT_ER(cam_r(dd, DN_CAMSTAT, &st, sizeof(st), &asz), E_OK);
	KT_ASSERT_EQ(st.frames, 3);
	KT_ASSERT_EQ(st.dropped, 1);
	tk_cls_dev(dd, 0);
	knl_cam_loopback(FALSE);

	dd = tk_opn_dev((UB *)CAM_DEVNM, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd > 0 ) {
		KT_ASSERT_ER(cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz), E_OK);
		KT_ASSERT_EQ(in.state, CAMST_NODEV);
		tk_cls_dev(dd, 0);
	}
}

/* the device's object: found as a camera, its records the device's data */
LOCAL void test_object( void )
{
	TS_UUID		list[8];
	T_CAMINFO	in;
	T_CAMSETMODE	m;
	T_OBREC		recs[8];
	SZ		asz = 0;
	INT		cnt = 0;
	W		v;
	ID		k;

	LEAVE_IF_PLUGGED();
	knl_cam_loopback(TRUE);
	KT_ASSERT_ER(ob_lst_obj(OB_T_DEVICE, OB_S_CAMERA, NULL, list, 8, &cnt), E_OK);
	KT_ASSERT(cnt >= 1);
	if ( cnt < 1 ) {
		knl_cam_loopback(FALSE);
		return;
	}
	k = ob_opn_obj(&list[0], OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(k > 0);
	if ( k > 0 ) {
		KT_ASSERT_ER(ob_lst_rec(k, recs, 8, &cnt), E_OK);
		KT_ASSERT_EQ(cnt, OB_CAM_NREC);
		KT_ASSERT_EQ(recs[OB_CAM_INFO].size, sizeof(T_CAMINFO));
		KT_ASSERT_ER(ob_rea_rec(k, OB_CAM_INFO, 0, &in, sizeof(in), &asz), E_OK);
		KT_ASSERT_EQ(in.nmodes, 2);
		knl_memset(&m, 0, sizeof(m));
		m.mode = 1;
		KT_ASSERT_ER(ob_wri_rec(k, OB_CAM_MODE, 0, &m, sizeof(m), &asz), E_OK);
		v = CAM_START;
		KT_ASSERT_ER(ob_wri_rec(k, OB_CAM_STREAM, 0, &v, sizeof(v), &asz), E_OK);
		knl_cam_feed(frame, 1000);
		KT_ASSERT_ER(ob_rea_rec(k, OB_CAM_FRAME, 0, got, sizeof(got), &asz), E_OK);
		KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + 1000);
		KT_ASSERT_EQ(((T_CAMFRAME *)got)->format, CAMFMT_MJPEG);
		ob_cls_obj(k);
	}
	knl_cam_loopback(FALSE);
}

/* ---------------------------------------------------------------- a USB video class camera */

IMPORT INT  knl_uvc_probe_attach( CONST UB *cfg, INT len, INT speed, UW payload,
				  INT *alts, INT maxalts, BOOL *bulk );
IMPORT void knl_uvc_probe_payload( CONST UB *data, INT len );
IMPORT void knl_uvc_probe_detach( void );

#define USB_SPEED_HIGH_	3

/*
 * A UVC 1.1 camera's configuration: the VideoControl interface, the
 * VideoStreaming one with YUY2 16 x 8 and MJPEG 32 x 24, and either two
 * isochronous settings (512 and 3 x 1024 bytes a microframe) or one bulk
 * endpoint on setting 0.
 */
LOCAL INT make_cfg( UB *c, BOOL bulk )
{
	static CONST UB	head[] = {
		0x09, 0x02, 0, 0, 0x02, 0x01, 0x00, 0x80, 0xfa,
		0x09, 0x04, 0x00, 0x00, 0x00, 0x0e, 0x01, 0x00, 0x00,
		0x0d, 0x24, 0x01, 0x10, 0x01, 0x0d, 0x00, 0x00, 0x6c, 0xdc, 0x02, 0x01, 0x01,
		0x09, 0x04, 0x01, 0x00, 0x00, 0x0e, 0x02, 0x00, 0x00,
		0x0e, 0x24, 0x01, 0x02, 0x00, 0x00, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00,
		/* YUY2 */
		0x1b, 0x24, 0x04, 0x01, 0x01, 'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00,
		0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x1e, 0x24, 0x05, 0x01, 0x00, 16, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x00, 0x01, 0x00, 0x00, 0x15, 0x16, 0x05, 0x00, 0x01, 0x15, 0x16, 0x05, 0x00,
		/* MJPEG */
		0x0b, 0x24, 0x06, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x1e, 0x24, 0x07, 0x01, 0x00, 32, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x00, 0x10, 0x00, 0x00, 0x15, 0x16, 0x05, 0x00, 0x01, 0x15, 0x16, 0x05, 0x00,
	};
	static CONST UB	iso[] = {
		0x09, 0x04, 0x01, 0x01, 0x01, 0x0e, 0x02, 0x00, 0x00,
		0x07, 0x05, 0x81, 0x05, 0x00, 0x02, 0x01,
		0x09, 0x04, 0x01, 0x02, 0x01, 0x0e, 0x02, 0x00, 0x00,
		0x07, 0x05, 0x81, 0x05, 0x00, 0x14, 0x01,
	};
	static CONST UB	blk[] = {
		0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
	};
	INT	n = 0;

	knl_memcpy(c, head, sizeof(head));
	n = sizeof(head);
	if ( bulk ) {
		knl_memcpy(c + n, blk, sizeof(blk));
		n += sizeof(blk);
	} else {
		knl_memcpy(c + n, iso, sizeof(iso));
		n += sizeof(iso);
	}
	c[2] = (UB)n;
	c[3] = (UB)( n >> 8 );

	return n;
}

/* A payload: its header (with a PTS when pts is not 0) and n bytes of v */
LOCAL void payload( UB info, UW pts, UB v, INT n )
{
	UB	p[512];
	INT	h = ( pts != 0 ) ? 6 : 2, i;

	p[0] = (UB)h;
	p[1] = (UB)( info | ( pts != 0 ? 0x04 : 0 ) );
	if ( pts != 0 ) {
		p[2] = (UB)pts;
		p[3] = (UB)( pts >> 8 );
		p[4] = (UB)( pts >> 16 );
		p[5] = (UB)( pts >> 24 );
	}
	for ( i = 0; i < n; i++ ) p[h + i] = (UB)( v + i );
	knl_uvc_probe_payload(p, h + n);
}

LOCAL ER pick( ID dd, INT mode, T_CAMSETMODE *m )
{
	W	v = CAM_START;

	knl_memset(m, 0, sizeof(*m));
	m->mode = mode;
	if ( cam_w(dd, DN_CAMMODE, m, sizeof(*m)) < E_OK ) return E_IO;
	{
		SZ	asz = 0;

		cam_r(dd, DN_CAMMODE, m, sizeof(*m), &asz);
	}
	return cam_w(dd, DN_CAMSTREAM, &v, sizeof(v));
}

/* The descriptors read; payloads put together into frames, the bad ones dropped */
LOCAL void test_uvc_iso( void )
{
	UB		cfg[256];
	INT		alts[4] = { 0, 0, 0, 0 };
	BOOL		bulk = TRUE;
	T_CAMINFO	in;
	T_CAMMODE	modes[2];
	T_CAMSETMODE	m;
	T_CAMFRAME	h;
	T_CAMSTAT	st;
	SZ		asz = 0;
	INT		n, i;
	ID		dd;

	LEAVE_IF_PLUGGED();
	n = knl_uvc_probe_attach(cfg, make_cfg(cfg, FALSE), USB_SPEED_HIGH_, 600, alts, 4, &bulk);
	KT_ASSERT_EQ(n, 2);
	if ( n < 0 ) return;
	KT_ASSERT_EQ(alts[0], 512);
	KT_ASSERT_EQ(alts[1], 3 * 1024);
	KT_ASSERT(!bulk);
	dd = tk_opn_dev((UB *)CAM_DEVNM, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) {
		knl_uvc_probe_detach();
		return;
	}
	KT_ASSERT_ER(cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz), E_OK);
	KT_ASSERT_EQ(in.state, CAMST_READY);
	KT_ASSERT_EQ(in.bcdUVC, 0x0110);
	KT_ASSERT(knl_strcmp((CONST char *)in.product, "Probe camera") == 0);
	KT_ASSERT_ER(cam_r(dd, DN_CAMMODES, modes, sizeof(modes), &asz), E_OK);
	KT_ASSERT_EQ(modes[0].format, CAMFMT_YUY2);
	KT_ASSERT(modes[0].width == 16 && modes[0].height == 8);
	KT_ASSERT_EQ(modes[0].maxBytes, 256);
	KT_ASSERT_EQ(modes[0].defInterval, 333333);
	KT_ASSERT_EQ(modes[0].nintv, 1);
	KT_ASSERT_EQ(modes[1].format, CAMFMT_MJPEG);
	KT_ASSERT(modes[1].width == 32 && modes[1].height == 24);
	KT_ASSERT_EQ(modes[1].maxBytes, 4096);

	/* the setting whose service carries the payload: 600 bytes take the second */
	KT_ASSERT_ER(pick(dd, 0, &m), E_OK);
	KT_ASSERT_EQ(m.altSetting, 2);

	/* two payloads, the second with EOF: one frame */
	payload(0x00, 0, 0, 128);
	payload(0x02, 0, 128, 128);
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + 256);
	knl_memcpy(&h, got, sizeof(h));
	KT_ASSERT_EQ(h.seq, 1);
	KT_ASSERT_EQ(h.format, CAMFMT_YUY2);
	KT_ASSERT_EQ(h.bytes, 256);
	for ( i = 0; i < 256 && got[sizeof(h) + i] == (UB)i; i++ ) ;
	KT_ASSERT_EQ(i, 256);

	/* FID changes without an EOF: the frame before it ends there */
	payload(0x01, 0, 0, 128);
	payload(0x01, 0, 128, 128);
	payload(0x00, 0, 0, 128);
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + 256);
	/* ... and one too short for 16 x 8 YUY2 is dropped */
	payload(0x02, 0, 0, 100);
	KT_ASSERT_ER(cam_r(dd, DN_CAMSTAT, &st, sizeof(st), &asz), E_OK);
	KT_ASSERT_EQ(st.frames, 2);
	KT_ASSERT_EQ(st.dropped, 1);

	/* ERR in a header: the frame is dropped; too many bytes: dropped */
	payload(0x40, 0, 0, 128);
	payload(0x02, 0, 0, 128);
	payload(0x00, 0, 0, 128);
	payload(0x00, 0, 0, 128);
	payload(0x02, 0, 0, 128);
	KT_ASSERT_ER(cam_r(dd, DN_CAMSTAT, &st, sizeof(st), &asz), E_OK);
	KT_ASSERT_EQ(st.frames, 2);
	KT_ASSERT_EQ(st.dropped, 3);

	/* a PTS is kept with the frame */
	payload(0x00, 0x12345678, 0, 128);
	payload(0x02, 0, 128, 128);
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	knl_memcpy(&h, got, sizeof(h));
	KT_ASSERT_EQ(h.seq, 3);
	KT_ASSERT_EQ(h.pts, 0x12345678);

	/* MJPEG: a frame is taken when it starts as a JPEG does */
	KT_ASSERT_ER(pick(dd, 1, &m), E_OK);
	{
		UB	p[2 + 300];

		knl_memset(p, 0x11, sizeof(p));
		p[0] = 2;
		p[1] = 0x02;
		p[2] = 0xff;
		p[3] = 0xd8;
		knl_uvc_probe_payload(p, sizeof(p));
	}
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + 300);
	knl_memcpy(&h, got, sizeof(h));
	KT_ASSERT_EQ(h.format, CAMFMT_MJPEG);
	KT_ASSERT(h.width == 32 && h.height == 24);
	payload(0x02, 0, 0, 300);			/* not a JPEG */
	KT_ASSERT_ER(cam_r(dd, DN_CAMSTAT, &st, sizeof(st), &asz), E_OK);
	KT_ASSERT_EQ(st.dropped, 4);
	tk_cls_dev(dd, 0);
	knl_uvc_probe_detach();
}

/* A bulk camera: each payload ended by a transfer shorter than a whole one */
LOCAL void test_uvc_bulk( void )
{
	UB		cfg[256];
	INT		alts[4];
	BOOL		bulk = FALSE;
	T_CAMSETMODE	m;
	SZ		asz = 0;
	INT		n;
	ID		dd;

	LEAVE_IF_PLUGGED();
	n = knl_uvc_probe_attach(cfg, make_cfg(cfg, TRUE), USB_SPEED_HIGH_, 200, alts, 4, &bulk);
	KT_ASSERT_EQ(n, 2);
	if ( n < 0 ) return;
	KT_ASSERT(bulk);
	dd = tk_opn_dev((UB *)CAM_DEVNM, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) {
		knl_uvc_probe_detach();
		return;
	}
	KT_ASSERT_ER(pick(dd, 0, &m), E_OK);
	KT_ASSERT_EQ(m.altSetting, 0);
	/* payloads of 200 bytes at most go one transfer each (512, a packet) */
	payload(0x00, 0, 0, 128);
	payload(0x02, 0, 128, 128);
	KT_ASSERT_ER(cam_r(dd, DN_CAMFRAME, got, sizeof(got), &asz), E_OK);
	KT_ASSERT_EQ(asz, sizeof(T_CAMFRAME) + 256);
	KT_ASSERT_EQ(got[sizeof(T_CAMFRAME) + 200], 200);
	tk_cls_dev(dd, 0);
	knl_uvc_probe_detach();

	dd = tk_opn_dev((UB *)CAM_DEVNM, TD_UPDATE);
	if ( dd > 0 ) {
		T_CAMINFO	in;

		KT_ASSERT_ER(cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz), E_OK);
		KT_ASSERT_EQ(in.state, CAMST_NODEV);
		tk_cls_dev(dd, 0);
	}
}

/*
 * A USB camera plugged in (the board): every unit with a camera that is
 * not the kernel's own, as the class driver read it; then its smallest
 * mode of each format streamed for three seconds, the frames counted and
 * looked at (an uncompressed one as long as its size says, an MJPEG one
 * beginning as a JPEG does), and its largest mode for two. Each step has
 * its time-out; what came is written on the console.
 */
LOCAL BOOL frame_ok( UB fmt, CONST T_CAMFRAME *h, CONST UB *data )
{
	if ( fmt == CAMFMT_MJPEG ) {
		return (BOOL)( h->bytes >= 4 && data[0] == 0xff && data[1] == 0xd8 );
	}
	if ( fmt == CAMFMT_NV12 ) {
		return (BOOL)( h->bytes == (UW)h->width * h->height * 3 / 2 );
	}
	return (BOOL)( h->bytes == (UW)h->width * h->height * 2 );
}

LOCAL INT stream_for( ID dd, INT mode, UB fmt, INT ms, INT *bad )
{
	T_CAMSETMODE	m;
	T_CAMFRAME	h;
	SZ		asz = 0;
	W		v = CAM_START;
	UB		*buf;
	INT		n = 0, t;
	SZ		cap;

	knl_memset(&m, 0, sizeof(m));
	m.mode = mode;
	if ( cam_w(dd, DN_CAMMODE, &m, sizeof(m)) < E_OK ) return -1;
	(void)cam_r(dd, DN_CAMMODE, &m, sizeof(m), &asz);
	cap = sizeof(T_CAMFRAME) + (SZ)m.maxFrame + 65536;
	buf = (UB *)Kmalloc(cap);
	if ( buf == NULL ) return -2;
	if ( cam_w(dd, DN_CAMSTREAM, &v, sizeof(v)) < E_OK ) {
		Kfree(buf);
		return -3;
	}
	*bad = 0;
	for ( t = 0; t < ms; t += 10 ) {
		if ( cam_r(dd, DN_CAMFRAME, buf, cap, &asz) >= E_OK && asz > (SZ)sizeof(h) ) {
			knl_memcpy(&h, buf, sizeof(h));
			if ( !frame_ok(fmt, &h, buf + sizeof(h)) ) ( *bad )++;
			n++;
		}
		tk_dly_tsk(10);
	}
	v = CAM_STOP;
	(void)cam_w(dd, DN_CAMSTREAM, &v, sizeof(v));
	Kfree(buf);
	return n;
}

/* The units the camera device has now */
LOCAL INT units( void )
{
	T_RDEV	rd;

	return ( tk_ref_dev((UB *)CAM_DEVNM, &rd) >= E_OK ) ? rd.nsub : 0;
}

LOCAL void test_uvc_board( void )
{
	static T_CAMMODE	modes[CAM_MAXMODES];
	T_CAMINFO		in;
	SZ			asz = 0;
	UB			name[8];
	INT			u, k, f, n, bad, cams = 0;
	ID			dd;

	if ( !KT_BOARD ) KT_SKIP("a USB camera is on the board");
	tk_dly_tsk(2000);					/* a camera plugged in at start-up comes up */
	for ( u = 0; u < units(); u++ ) {
		tm_sprintf(name, (CONST UB *)"cama%d", u);
		dd = tk_opn_dev(name, TD_UPDATE);
		if ( dd <= 0 ) continue;
		if ( cam_r(dd, DN_CAMINFO, &in, sizeof(in), &asz) < E_OK || in.state == CAMST_NODEV ) {
			tk_cls_dev(dd, 0);
			continue;
		}
		cams++;
		tm_printf((UB *)"  cama%d: %s %s, %04x:%04x, UVC %x.%02x, %d modes\n", u, in.manufacturer, in.product,
			  in.vid, in.pid, in.bcdUVC >> 8, in.bcdUVC & 0xff, in.nmodes);
		KT_ASSERT(in.nmodes > 0);
		(void)cam_r(dd, DN_CAMMODES, modes, sizeof(modes), &asz);
		for ( f = CAMFMT_YUY2; f <= CAMFMT_UYVY; f++ ) {
			INT	small = -1, large = -1;

			for ( k = 0; k < in.nmodes && k < CAM_MAXMODES; k++ ) {
				UW	a = (UW)modes[k].width * modes[k].height;

				if ( modes[k].format != f ) continue;
				if ( small < 0 || a < (UW)modes[small].width * modes[small].height ) small = k;
				if ( large < 0 || a > (UW)modes[large].width * modes[large].height ) large = k;
			}
			if ( small < 0 ) continue;
			n = stream_for(dd, small, (UB)f, 3000, &bad);
			tm_printf((UB *)"  cama%d: format %d %dx%d: %d frames in 3 s, %d not right\n", u, f,
				  modes[small].width, modes[small].height, n, bad);
			KT_ASSERT(n >= 5);
			KT_ASSERT_EQ(bad, 0);
			if ( large != small ) {
				n = stream_for(dd, large, (UB)f, 2000, &bad);
				tm_printf((UB *)"  cama%d: format %d %dx%d: %d frames in 2 s, %d not right\n", u, f,
					  modes[large].width, modes[large].height, n, bad);
				KT_ASSERT(n >= 2);
				KT_ASSERT_EQ(bad, 0);
			}
		}
		tk_cls_dev(dd, 0);
	}
	if ( cams == 0 ) KT_SKIP("no USB camera plugged in");
}

EXPORT void ktest_cam( void )
{
	KT_RUN(test_nocam);
	KT_RUN(test_loopback);
	KT_RUN(test_object);
	KT_RUN(test_uvc_iso);
	KT_RUN(test_uvc_bulk);
	KT_RUN(test_uvc_board);
}
