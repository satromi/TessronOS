/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cam.c
 *	The camera device "cama" (design 10.16): the cameras the class
 *	drivers attach (device/usb/uvc.c), and one of the kernel's own for
 *	the tests, as units a program picks a mode of, starts and reads
 *	frames from.
 *
 *	A camera's frames come in from its driver (knl_cam_frame) into one of
 *	three buffers: the newest frame complete, the one a program copies
 *	from, and the one the next frame goes into. A buffer is picked and
 *	marked under the camera's spin lock, and filled outside it, so the
 *	driver, which hands frames over with the USB manager's lock held,
 *	never waits for a reader; a frame that finds no buffer free is
 *	counted dropped. A read gives the newest frame the program has not
 *	had yet (each program's last frame number is kept), or nothing.
 *
 *	The buffers are as large as the largest frame of the mode picked;
 *	they are made when a mode is picked, while the camera does not
 *	stream.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/proc.h>
#include <ts/time.h>
#include <ts/cam.h>
#include <ts/ob.h>

#define NBUF		3
#define NSEEN		8		/* the programs whose last frame is kept */
#define FRAME_MAX	( 8 * 1024 * 1024 )

typedef struct {
	T_CAMSRC	*src;		/* NULL: no camera on the unit */
	T_CAMSETMODE	setm;
	INT		curmode;
	BOOL		streaming;
	UB		*buf[NBUF];
	INT		cap;		/* each buffer's size */
	INT		len[NBUF];
	UW		seq[NBUF], msec[NBUF], pts[NBUF];
	INT		readers[NBUF];
	INT		newest;		/* -1 none */
	INT		filling;	/* -1 none */
	UW		seqno;
	T_CAMSTAT	stat;
	struct {
		ID	key;
		UW	seen;
	}		seen[NSEEN];
	T_SPLOCK	lk;
} CAM;

/*
 * The units, as many as cameras have been attached at once: the device
 * starts with CAM_FIRST and is registered again with twice as many when
 * they are all taken, up to the CAM_UNITS_MAX a device ID has room for.
 * Each unit is allocated on its own and never freed, and a larger table
 * is made whole before it is put in place, so a driver handing a frame
 * over finds its unit without a lock while the table grows.
 */
#define CAM_FIRST	4

typedef struct {
	INT	n;
	CAM	*e[];
} CAMTBL;

LOCAL CAMTBL *volatile	cam_tbl = NULL;
LOCAL T_DDEV		cam_ddev;
LOCAL ID	cam_devid = 0;
LOCAL BOOL	cam_ready = FALSE;
LOCAL ID	cam_mtx = 0;		/* the requests, and the attaching, one at a time */

LOCAL UW now_ms( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return (UW)( ns / 1000000 );
}

LOCAL ID requester( void )
{
	ID	pid = ts_get_pid();

	return ( pid > 0 ) ? pid : -tk_get_tid();
}

/* The unit u, NULL none */
LOCAL CAM *cam_at( INT u )
{
	CAMTBL	*t = cam_tbl;

	return ( t != NULL && u >= 0 && u < t->n ) ? t->e[u] : NULL;
}

/* The camera of a device id: "cama" and "cama0" are camera 0 */
LOCAL CAM *cam_of( ID devid )
{
	INT	u = (INT)( devid - cam_devid );

	return cam_at(( u == 0 ) ? 0 : u - 1);
}

/*
 * Twice as many units (CAM_FIRST the first time), with cam_mtx held
 * once the device is registered: the device is registered again with
 * that many subunits.
 */
LOCAL ER cam_grow( void )
{
	CAMTBL	*t = cam_tbl, *nt;
	INT	n = ( t != NULL ) ? t->n : 0, cap, i;
	ID	id;

	if ( n >= CAM_UNITS_MAX ) {
		return E_LIMIT;
	}
	cap = ( n > 0 ) ? n * 2 : CAM_FIRST;
	if ( cap > CAM_UNITS_MAX ) {
		cap = CAM_UNITS_MAX;
	}
	nt = (CAMTBL *)Kcalloc(1, sizeof(CAMTBL) + sizeof(CAM *) * (SZ)cap);
	if ( nt == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < cap; i++ ) {
		if ( i < n ) {
			nt->e[i] = t->e[i];
			continue;
		}
		nt->e[i] = (CAM *)Kcalloc(1, sizeof(CAM));
		if ( nt->e[i] == NULL ) {
			while ( --i >= n ) Kfree(nt->e[i]);
			Kfree(nt);
			return E_NOMEM;
		}
		InitSpinLock(&nt->e[i]->lk);
		nt->e[i]->newest = nt->e[i]->filling = nt->e[i]->curmode = -1;
	}
	nt->n = cap;
	__sync_synchronize();
	cam_tbl = nt;			/* the table before is not freed */
	if ( cam_devid > 0 ) {
		cam_ddev.nsub = cap;
		id = tk_def_dev((UB *)CAM_DEVNM, &cam_ddev, NULL);
		if ( id < E_OK ) {
			return id;
		}
	}
	return E_OK;
}

LOCAL void free_bufs( CAM *c )
{
	INT	i;

	for ( i = 0; i < NBUF; i++ ) {
		if ( c->buf[i] != NULL ) Kfree(c->buf[i]);
		c->buf[i] = NULL;
		c->len[i] = 0;
		c->readers[i] = 0;
	}
	c->cap = 0;
	c->newest = c->filling = -1;
}

/* ---------------------------------------------------------------- from the drivers */

EXPORT INT knl_cam_attach( T_CAMSRC *s )
{
	INT	i, u = E_LIMIT;
	BOOL	grew = FALSE;

	if ( !cam_ready || s == NULL ) return E_OBJ;
	tk_loc_mtx(cam_mtx, TMO_FEVR);
	for ( i = 0; u < 0; i++ ) {
		if ( i == cam_tbl->n ) {
			ER	er = cam_grow();

			if ( er < E_OK ) {
				u = er;
				break;
			}
			grew = TRUE;
		}
		if ( cam_tbl->e[i]->src == NULL ) {
			CAM	*c = cam_tbl->e[i];

			free_bufs(c);
			knl_memset(&c->setm, 0, sizeof(c->setm));
			knl_memset(&c->stat, 0, sizeof(c->stat));
			knl_memset(c->seen, 0, sizeof(c->seen));
			c->curmode = -1;
			c->streaming = FALSE;
			c->seqno = 0;
			c->src = s;
			s->unit = i;
			u = i;
		}
	}
	tk_unl_mtx(cam_mtx);
	if ( grew ) {
		knl_obdev_changed();		/* the new units have objects */
	}
	if ( u >= 0 ) {
		tm_printf((UB *)"cam: %s on cama%d, %d modes\n", s->info.product, u, s->nmodes);
	}
	return u;
}

EXPORT void knl_cam_detach( T_CAMSRC *s )
{
	CAM	*c;
	UINT	im;

	if ( s == NULL || ( c = cam_at(s->unit) ) == NULL ) return;
	tk_loc_mtx(cam_mtx, TMO_FEVR);
	ISpinLock(&c->lk, &im);
	c->src = NULL;
	c->streaming = FALSE;
	ISpinUnlock(&c->lk, &im);
	/* a reader copying out still holds its buffer: wait for it */
	for ( ;; ) {
		INT	i, busy = 0;

		ISpinLock(&c->lk, &im);
		for ( i = 0; i < NBUF; i++ ) busy += c->readers[i];
		ISpinUnlock(&c->lk, &im);
		if ( busy == 0 ) break;
		tk_dly_tsk(1);
	}
	free_bufs(c);
	s->unit = -1;
	tk_unl_mtx(cam_mtx);
}

/* A whole frame from the camera's driver: never waits */
EXPORT void knl_cam_frame( T_CAMSRC *s, CONST UB *data, INT len, UW pts )
{
	CAM	*c;
	UINT	im;
	INT	i, b = -1;

	if ( s == NULL || ( c = cam_at(s->unit) ) == NULL ) return;
	ISpinLock(&c->lk, &im);
	if ( c->streaming && c->cap > 0 && len > 0 && len <= c->cap && c->filling < 0 ) {
		for ( i = 0; i < NBUF && b < 0; i++ ) {
			if ( i != c->newest && c->readers[i] == 0 && c->buf[i] != NULL ) b = i;
		}
		if ( b >= 0 ) c->filling = b;
	}
	if ( b < 0 ) {
		c->stat.dropped++;
		ISpinUnlock(&c->lk, &im);
		return;
	}
	ISpinUnlock(&c->lk, &im);

	knl_memcpy(c->buf[b], data, (SZ)len);

	ISpinLock(&c->lk, &im);
	c->len[b] = len;
	c->pts[b] = pts;
	c->msec[b] = now_ms();
	c->seq[b] = ++c->seqno;
	c->newest = b;
	c->filling = -1;
	c->stat.frames++;
	c->stat.bytes += (UW)len;
	ISpinUnlock(&c->lk, &im);
}

EXPORT void knl_cam_dropped( T_CAMSRC *s )
{
	UINT	im;
	CAM	*c;

	if ( s == NULL || ( c = cam_at(s->unit) ) == NULL ) return;
	ISpinLock(&c->lk, &im);
	c->stat.dropped++;
	ISpinUnlock(&c->lk, &im);
}

/* ---------------------------------------------------------------- requests */

/* The last frame number the program had, and its slot */
LOCAL INT seen_slot( CAM *c, ID key )
{
	INT	i, free = -1;

	for ( i = 0; i < NSEEN; i++ ) {
		if ( c->seen[i].key == key ) return i;
		if ( c->seen[i].key == 0 && free < 0 ) free = i;
	}
	if ( free < 0 ) free = (INT)( (UW)key % NSEEN );	/* the oldest guess: a slot taken over */
	c->seen[free].key = key;
	c->seen[free].seen = 0;
	return free;
}

LOCAL ER read_frame( CAM *c, T_DEVREQ *req )
{
	T_CAMFRAME	h;
	UINT		im;
	INT		b, n, slot;
	ID		key = requester();

	ISpinLock(&c->lk, &im);
	slot = seen_slot(c, key);
	b = c->newest;
	if ( b < 0 || c->seq[b] == c->seen[slot].seen ) {
		ISpinUnlock(&c->lk, &im);
		req->asize = 0;
		return E_OK;
	}
	if ( req->size <= 0 ) {
		req->asize = (SZ)( sizeof(h) + c->len[b] );
		ISpinUnlock(&c->lk, &im);
		return E_OK;
	}
	c->readers[b]++;
	h.seq = c->seq[b];
	h.format = ( c->src != NULL && c->curmode >= 0 ) ? c->src->modes[c->curmode].format : CAMFMT_NONE;
	h.flags = 0;
	h.width = ( c->src != NULL && c->curmode >= 0 ) ? c->src->modes[c->curmode].width : 0;
	h.height = ( c->src != NULL && c->curmode >= 0 ) ? c->src->modes[c->curmode].height : 0;
	h.bytes = (UW)c->len[b];
	h.msec = c->msec[b];
	h.pts = c->pts[b];
	c->seen[slot].seen = h.seq;
	ISpinUnlock(&c->lk, &im);

	if ( req->size < (SZ)sizeof(h) ) {
		ISpinLock(&c->lk, &im);
		c->readers[b]--;
		ISpinUnlock(&c->lk, &im);
		return E_PAR;
	}
	n = (INT)req->size - (INT)sizeof(h);
	if ( n > c->len[b] ) n = c->len[b];
	if ( n < c->len[b] ) h.flags |= CAMFR_TRUNC;
	knl_memcpy(req->buf, &h, sizeof(h));
	knl_memcpy((UB *)req->buf + sizeof(h), c->buf[b], (SZ)n);
	req->asize = (SZ)( sizeof(h) + n );

	ISpinLock(&c->lk, &im);
	c->readers[b]--;
	c->stat.delivered++;
	ISpinUnlock(&c->lk, &im);
	return E_OK;
}

LOCAL ER give( T_DEVREQ *req, CONST void *src, SZ len )
{
	if ( req->size <= 0 ) {
		req->asize = len;
		return E_OK;
	}
	if ( req->size < len ) return E_PAR;
	knl_memcpy(req->buf, src, len);
	req->asize = len;
	return E_OK;
}

LOCAL ER proc_read( CAM *c, T_DEVREQ *req )
{
	switch ( req->start ) {
	case DN_CAMFRAME:
		if ( c->src == NULL ) return E_NOEXS;
		return read_frame(c, req);
	case DN_CAMINFO: {
		T_CAMINFO	in;

		knl_memset(&in, 0, sizeof(in));
		if ( c->src == NULL ) {
			in.state = CAMST_NODEV;
			in.curMode = -1;
		} else {
			in = c->src->info;
			in.state = c->streaming ? CAMST_STREAM : CAMST_READY;
			in.nmodes = c->src->nmodes;
			in.curMode = c->curmode;
		}
		return give(req, &in, sizeof(in));
	}
	case DN_CAMMODES: {
		SZ	all;

		if ( c->src == NULL ) return E_NOEXS;
		all = (SZ)( sizeof(T_CAMMODE) * (SZ)c->src->nmodes );
		if ( req->size <= 0 ) {
			req->asize = all;
			return E_OK;
		}
		if ( req->size < all ) all = req->size - req->size % (SZ)sizeof(T_CAMMODE);
		knl_memcpy(req->buf, c->src->modes, all);
		req->asize = all;
		return E_OK;
	}
	case DN_CAMMODE:
		if ( c->src == NULL ) return E_NOEXS;
		return give(req, &c->setm, sizeof(c->setm));
	case DN_CAMSTREAM: {
		W	v = c->streaming ? CAM_START : CAM_STOP;

		return give(req, &v, sizeof(v));
	}
	case DN_CAMSTAT:
		return give(req, &c->stat, sizeof(c->stat));
	default:
		return E_PAR;
	}
}

LOCAL void stop_stream( CAM *c )
{
	UINT	im;

	if ( !c->streaming ) return;
	ISpinLock(&c->lk, &im);
	c->streaming = FALSE;
	ISpinUnlock(&c->lk, &im);
	if ( c->src != NULL && c->src->stop != NULL ) c->src->stop(c->src);
}

LOCAL ER proc_write( CAM *c, T_DEVREQ *req )
{
	switch ( req->start ) {
	case DN_CAMMODE: {
		T_CAMSETMODE	m;
		T_CAMMODE	*md;
		INT		i, cap;
		ER		er;

		if ( c->src == NULL ) return E_NOEXS;
		if ( req->size < (SZ)sizeof(m) ) return E_PAR;
		knl_memcpy(&m, req->buf, sizeof(m));
		if ( m.mode < 0 || m.mode >= c->src->nmodes ) return E_PAR;
		stop_stream(c);
		er = ( c->src->set_mode != NULL ) ? c->src->set_mode(c->src, &m) : E_OK;
		if ( er < E_OK ) return er;
		md = &c->src->modes[m.mode];
		cap = (INT)( m.maxFrame > md->maxBytes ? m.maxFrame : md->maxBytes );
		if ( cap <= 0 ) cap = (INT)md->width * md->height * 2;
		if ( cap > FRAME_MAX ) cap = FRAME_MAX;
		if ( cap != c->cap ) {
			free_bufs(c);
			for ( i = 0; i < NBUF; i++ ) {
				c->buf[i] = (UB *)Kmalloc((SZ)cap);
				if ( c->buf[i] == NULL ) {
					free_bufs(c);
					return E_NOMEM;
				}
			}
			c->cap = cap;
		}
		c->newest = c->filling = -1;
		c->setm = m;
		c->curmode = m.mode;
		knl_memcpy(req->buf, &m, sizeof(m));
		req->asize = sizeof(m);
		return E_OK;
	}
	case DN_CAMSTREAM: {
		W	v;
		ER	er;
		UINT	im;

		if ( c->src == NULL ) return E_NOEXS;
		if ( req->size < (SZ)sizeof(v) ) return E_PAR;
		knl_memcpy(&v, req->buf, sizeof(v));
		req->asize = sizeof(v);
		if ( v == CAM_STOP ) {
			stop_stream(c);
			return E_OK;
		}
		if ( v != CAM_START ) return E_PAR;
		if ( c->streaming ) return E_OK;
		if ( c->curmode < 0 || c->cap <= 0 ) return E_OBJ;	/* no mode picked */
		ISpinLock(&c->lk, &im);
		c->streaming = TRUE;
		ISpinUnlock(&c->lk, &im);
		er = ( c->src->start != NULL ) ? c->src->start(c->src) : E_OK;
		if ( er < E_OK ) {
			ISpinLock(&c->lk, &im);
			c->streaming = FALSE;
			ISpinUnlock(&c->lk, &im);
		}
		return er;
	}
	default:
		return E_PAR;
	}
}

LOCAL ER cam_open( ID devid, UINT omode, void *exinf )
{
	return E_OK;
}

/* The last close of a unit: its stream stops */
LOCAL ER cam_close( ID devid, UINT option, void *exinf )
{
	CAM	*c = cam_of(devid);

	if ( c == NULL ) return E_OK;
	tk_loc_mtx(cam_mtx, TMO_FEVR);
	stop_stream(c);
	tk_unl_mtx(cam_mtx);
	return E_OK;
}

LOCAL ER cam_exec( T_DEVREQ *req, TMO tmout, void *exinf )
{
	CAM	*c = cam_of(req->devid);

	req->asize = 0;
	if ( c == NULL ) {
		req->error = E_PAR;
		return E_OK;
	}
	tk_loc_mtx(cam_mtx, TMO_FEVR);
	req->error = ( req->cmd == TDC_READ ) ? proc_read(c, req) : proc_write(c, req);
	tk_unl_mtx(cam_mtx);
	return E_OK;
}

LOCAL INT cam_wait( T_DEVREQ *req, INT nreq, TMO tmout, void *exinf )
{
	return ( nreq > 0 ) ? 0 : E_PAR;
}

LOCAL ER cam_abort( ID tskid, T_DEVREQ *req, INT nreq, void *exinf )
{
	return E_OK;
}

/* Suspend: every stream stops */
LOCAL INT cam_event( INT evttyp, void *evtinf, void *exinf )
{
	INT	i;

	if ( evttyp != TDV_SUSPEND ) return E_OK;
	for ( i = 0; cam_at(i) != NULL; i++ ) stop_stream(cam_at(i));
	return E_OK;
}

EXPORT ER knl_cam_init( void )
{
	T_DDEV	ddev;
	T_CMTX	cm;
	ID	id;

	if ( cam_ready ) return E_OK;
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	cm.ceilpri = 0;
	cam_mtx = tk_cre_mtx(&cm);
	if ( cam_mtx <= 0 ) return cam_mtx;
	if ( cam_grow() < E_OK ) return E_NOMEM;
	ddev.exinf   = NULL;
	ddev.drvatr  = 0;
	ddev.devatr  = TDK_CAMERA;
	ddev.nsub    = cam_tbl->n;
	ddev.blksz   = 1;
	ddev.openfn  = (FP)cam_open;
	ddev.closefn = (FP)cam_close;
	ddev.execfn  = (FP)cam_exec;
	ddev.waitfn  = (FP)cam_wait;
	ddev.abortfn = (FP)cam_abort;
	ddev.eventfn = (FP)cam_event;
	cam_ddev = ddev;
	id = tk_def_dev((UB *)CAM_DEVNM, &ddev, NULL);
	if ( id <= 0 ) return id;
	cam_devid = id;
	cam_ready = TRUE;
	return E_OK;
}

/* ---------------------------------------------------------------- the kernel's own, for the tests */

LOCAL T_CAMMODE	loop_modes[2];
LOCAL T_CAMSRC	loop_src;
LOCAL BOOL	loop_on = FALSE;
LOCAL UINT	loop_fmts = 3;		/* bit 0: YUY2, bit 1: MJPEG */

LOCAL ER loop_set_mode( T_CAMSRC *s, T_CAMSETMODE *m )
{
	m->altSetting = 0;
	m->maxPayload = s->modes[m->mode].maxBytes;
	m->maxFrame = s->modes[m->mode].maxBytes;
	return E_OK;
}

LOCAL ER loop_start( T_CAMSRC *s )
{
	return E_OK;
}

LOCAL void loop_stop( T_CAMSRC *s )
{
}

EXPORT void knl_cam_loopback_formats( UINT mask )
{
	loop_fmts = ( mask & 3 ) != 0 ? ( mask & 3 ) : 3;
}

EXPORT void knl_cam_loopback( BOOL on )
{
	INT	i, n = 0;

	if ( on == loop_on ) return;
	if ( !on ) {
		knl_cam_detach(&loop_src);
		loop_on = FALSE;
		return;
	}
	knl_memset(&loop_src, 0, sizeof(loop_src));
	knl_memset(loop_modes, 0, sizeof(loop_modes));
	for ( i = 0; i < 2; i++ ) {
		if ( ( loop_fmts & ( 1U << i ) ) == 0 ) continue;
		loop_modes[n].fmtIndex = (UB)( i + 1 );
		loop_modes[n].frmIndex = 1;
		loop_modes[n].format = ( i == 0 ) ? CAMFMT_YUY2 : CAMFMT_MJPEG;
		loop_modes[n].width = 64;
		loop_modes[n].height = 48;
		loop_modes[n].maxBytes = ( i == 0 ) ? 64 * 48 * 2 : 64 * 1024;
		loop_modes[n].defInterval = 1000000;	/* 10 frames a second */
		loop_modes[n].nintv = 1;
		loop_modes[n].interval[0] = 1000000;
		n++;
	}
	knl_strcpy((char *)loop_src.info.product, "Test camera");
	knl_strcpy((char *)loop_src.info.manufacturer, "TessronOS");
	loop_src.modes = loop_modes;
	loop_src.nmodes = n;
	loop_src.set_mode = loop_set_mode;
	loop_src.start = loop_start;
	loop_src.stop = loop_stop;
	loop_src.unit = -1;
	if ( knl_cam_attach(&loop_src) >= 0 ) loop_on = TRUE;
}

EXPORT void knl_cam_feed( CONST UB *data, INT len )
{
	if ( loop_on ) knl_cam_frame(&loop_src, data, len, 0);
}
