/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cam.h
 *	The camera device "cama" (design 10.16).
 *
 *	Units: "cama" and "cama0" are camera 0, "cama1" on the cameras after
 *	it, in the order the cameras came; a camera keeps its unit while it
 *	stays. The device has 4 units at first and as many more as cameras
 *	come, up to CAM_UNITS_MAX: a unit's number is the low 8 bits of a
 *	device ID. A program picks a mode (a format and a frame size) from the
 *	list DN_CAMMODES gives, writes it to DN_CAMMODE, starts the stream
 *	with CAM_START on DN_CAMSTREAM, and then reads data number 0
 *	(DN_CAMFRAME) for frames. A read of a frame never waits: it gives the
 *	newest frame the program has not had yet, or 0 bytes; a read of size
 *	0 gives the size of that frame with its header.
 *
 *	The cameras are USB video class cameras (device/usb/uvc.c), and, for
 *	the tests, one the kernel is handed frames for (knl_cam_loopback).
 *
 *	Errors: E_PAR for an unknown data number or a short buffer, E_NOEXS
 *	when the unit has no camera, E_IO when the camera does not answer.
 *
 *	The device's object (design 18.7) has these as records (OB_CAM_*,
 *	include/ts/ob.h).
 */

#ifndef __TS_CAM_H__
#define __TS_CAM_H__

#include <tk/typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_DEVNM	"cama"
#define CAM_UNITS_MAX	255		/* units at most: the device ID's 8 bits of unit (subunits 0-254) */
#define TDK_CAMERA	0x00A0U		/* the kind the camera device registers as */

/* Formats */
#define CAMFMT_NONE	0
#define CAMFMT_YUY2	1		/* packed 4:2:2, bytes Y0 U Y1 V */
#define CAMFMT_NV12	2		/* planar Y, then U and V by turns (4:2:0) */
#define CAMFMT_MJPEG	3		/* a JPEG picture a frame (its DHT may be left out) */
#define CAMFMT_UYVY	4		/* packed 4:2:2, bytes U Y0 V Y1 */

/* Data numbers */
#define DN_CAMFRAME	0		/* R : T_CAMFRAME and the frame's bytes */
#define DN_CAMINFO	(-1)		/* R : T_CAMINFO */
#define DN_CAMMODES	(-2)		/* R : T_CAMMODE[], T_CAMINFO.nmodes of them */
#define DN_CAMMODE	(-3)		/* RW: T_CAMSETMODE; a write picks the mode and gives back what was agreed */
#define DN_CAMSTREAM	(-4)		/* RW: W, CAM_STOP or CAM_START */
#define DN_CAMSTAT	(-5)		/* R : T_CAMSTAT */

#define CAM_STOP	0
#define CAM_START	1

/* T_CAMINFO.state */
#define CAMST_NODEV	1		/* no camera on the unit */
#define CAMST_READY	3		/* a camera, not streaming */
#define CAMST_STREAM	4		/* streaming */
#define CAMST_ERROR	5		/* a camera that cannot be used */

#define CAM_MAXMODES	64
#define CAM_MAXINTV	8

typedef struct {
	W	state;			/* CAMST_* */
	UH	vid, pid;		/* idVendor, idProduct (0 for the kernel's own) */
	UH	bcdUVC;			/* the camera's UVC version */
	UH	rsv;
	W	nmodes;			/* the entries DN_CAMMODES gives */
	W	curMode;		/* the mode chosen, -1 none */
	UB	product[64];		/* its name, UTF-8 */
	UB	manufacturer[64];
} T_CAMINFO;

typedef struct {
	UB	fmtIndex;		/* the camera's bFormatIndex */
	UB	frmIndex;		/* bFrameIndex */
	UB	format;			/* CAMFMT_* */
	UB	nintv;			/* discrete intervals in interval[]; 0: a range */
	UH	width, height;
	UW	maxBytes;		/* the largest frame */
	UW	defInterval;		/* the frame interval it takes by default, 100 ns */
	UW	interval[CAM_MAXINTV];	/* the discrete ones, or [0] min [1] max [2] step */
} T_CAMMODE;

typedef struct {
	W	mode;			/* in : its place in DN_CAMMODES */
	UW	interval;		/* in : the frame interval (100 ns), 0 the mode's default */
	W	altSetting;		/* out: the streaming interface's alternate setting */
	UW	maxPayload;		/* out: dwMaxPayloadTransferSize agreed */
	UW	maxFrame;		/* out: dwMaxVideoFrameSize agreed */
} T_CAMSETMODE;

#define CAMFR_TRUNC	0x0001		/* the buffer was shorter than the frame */

typedef struct {
	UW	seq;			/* the frame's number, from 1 */
	UH	format;			/* CAMFMT_* */
	UH	flags;			/* CAMFR_* */
	UH	width, height;
	UW	bytes;			/* the whole frame (fewer may have been copied) */
	UW	msec;			/* when it was complete, ms since the device started */
	UW	pts;			/* the camera's presentation time stamp, 0 none */
} T_CAMFRAME;

typedef struct {
	UW	frames;			/* frames complete */
	UW	dropped;		/* frames thrown away (an error, a wrong size, too large) */
	UW	delivered;		/* frames read by programs */
	UW	bytes;			/* bytes of pictures that came (wraps) */
	UW	rsv[4];
} T_CAMSTAT;

/* ---------------------------------------------------------------- inside the kernel */

/*
 * A camera as a class driver gives it to the device: what it is, its
 * modes, and what it does when a program picks a mode and starts and
 * stops the stream. Its frames go to the device with knl_cam_frame.
 */
typedef struct cam_src {
	T_CAMINFO	info;		/* vid, pid, bcdUVC, product, manufacturer */
	T_CAMMODE	*modes;
	INT		nmodes;
	ER		(*set_mode)( struct cam_src *s, T_CAMSETMODE *m );
	ER		(*start)( struct cam_src *s );
	void		(*stop)( struct cam_src *s );
	void		*priv;
	INT		unit;		/* the device's: its unit, -1 when it has none */
} T_CAMSRC;

IMPORT ER   knl_cam_init( void );
IMPORT INT  knl_cam_attach( T_CAMSRC *s );		/* its unit, or E_LIMIT */
IMPORT void knl_cam_detach( T_CAMSRC *s );
IMPORT void knl_cam_frame( T_CAMSRC *s, CONST UB *data, INT len, UW pts );
IMPORT void knl_cam_dropped( T_CAMSRC *s );

/*
 * For a test: a camera of the kernel's own (YUY2 and MJPEG, 64 x 48 at
 * 10 frames a second) attached or taken away; knl_cam_feed hands it the
 * frame it gives next, in the mode chosen.
 */
IMPORT void knl_cam_loopback( BOOL on );
/* The formats it offers when it is next attached: bit 0 YUY2, bit 1 MJPEG (both, by default) */
IMPORT void knl_cam_loopback_formats( UINT mask );
IMPORT void knl_cam_feed( CONST UB *data, INT len );

#ifdef __cplusplus
}
#endif

#endif /* __TS_CAM_H__ */
