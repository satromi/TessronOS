/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hid.h
 *	Keyboard and pointer (design 10.4, 16.2.2)
 *
 *	What the devices on the bus report, turned into something the rest
 *	of the system can use: key codes and where the pointer is. The
 *	window manager takes the events here (ts_hid_read); everyone else
 *	reaches the keyboards and pointers through their objects (OB_S_INPUT,
 *	peripheral_kernel/obj/obinput.c), which show a copy of the events
 *	and are the way to put events in.
 *
 *	A keyboard reports which keys are down, not which ones were pressed,
 *	so the change from one report to the next is what becomes a press
 *	or a release. A pointer reports how far it moved, or where it is,
 *	depending on which kind it is; both end up as a place on the screen.
 */

#ifndef __TS_HID_H__
#define __TS_HID_H__

#ifdef __cplusplus
extern "C" {
#endif

/* What kind of thing happened */
#define HID_EV_NONE	0
#define HID_EV_KEY_DOWN	1
#define HID_EV_KEY_UP	2
#define HID_EV_MOVE	3
#define HID_EV_BTN_DOWN	4
#define HID_EV_BTN_UP	5
#define HID_EV_WHEEL	6		/* a wheel turned: code HID_WHEEL_*, dz how far */

/* Which wheel */
#define HID_WHEEL_V	0		/* up and down: dz > 0 turned away, upwards */
#define HID_WHEEL_H	1		/* across (AC Pan): dz > 0 to the right */

/* The keys that are held while another is pressed */
#define HID_MOD_LCTRL	0x01
#define HID_MOD_LSHIFT	0x02
#define HID_MOD_LALT	0x04
#define HID_MOD_LMETA	0x08
#define HID_MOD_RCTRL	0x10
#define HID_MOD_RSHIFT	0x20
#define HID_MOD_RALT	0x40
#define HID_MOD_RMETA	0x80

/*
 * The keys the system takes for itself, by their usages on the keyboard
 * page: the whole's volume a step up or down, and its mute (the window
 * layer, beep.c).
 * A machine's own volume buttons are given as them too (the 5-button
 * array of the ACPI HID event filter, kernel/sysdepend/acpi/acpi_power.c).
 */
#define HID_KEY_MUTE	0x7F
#define HID_KEY_VOLUP	0x80
#define HID_KEY_VOLDOWN	0x81

/*
 * The playing keys, which the keyboard page has no usages for: numbers
 * it leaves unused, given to the consumer page's keys (device/usb/hid.c).
 * They do not repeat. They go to the window that has the keys, as any
 * other key, for a program that plays something to take; a program that
 * does not look for them makes nothing of them, as no character is typed.
 */
#define HID_KEY_PLAYPAUSE	0xE8
#define HID_KEY_STOP		0xE9
#define HID_KEY_PREV		0xEA
#define HID_KEY_NEXT		0xEB

/* One thing that happened */
typedef struct {
	UINT	type;
	UINT	code;			/* the key, or which button */
	UINT	mods;			/* what was held at the time */
	INT	x, y;			/* where the pointer was, on the screen */
	UD	when;			/* nanoseconds since the machine started */
	INT	dz;			/* a wheel: the notches it turned */
} T_HIDEV;

/*
 * Find the keyboards and pointers on the bus and start reading them.
 * Answers how many were found, 0 when there are none, or an error.
 */
IMPORT INT knl_hid_init( void );

/*
 * Take the next thing that happened. Waits up to the time given;
 * E_TMOUT when nothing came, E_NOEXS when there is no such device.
 */
IMPORT ER  ts_hid_read( T_HIDEV *ev, TMO tmout );

/* Where the pointer is now, without waiting for it to move */
IMPORT ER  ts_hid_pointer( INT *p_x, INT *p_y, UINT *p_buttons );

/*
 * Where the first button last went down. While ts_hid_pointer says it
 * is held, this is where the hold began, however far the pointer has
 * gone since and however late the asker heard of the press.
 */
IMPORT ER  ts_hid_press_at( INT *p_x, INT *p_y );

/* What has been seen, for a test or a measurement */
typedef struct {
	UD	reports;		/* reports taken off the devices */
	UD	events;			/* things that came out of them */
	UD	dropped;		/* events nobody took in time */
	UINT	keyboards;
	UINT	pointers;
} T_HIDSTAT;

IMPORT ER  ts_hid_stat( T_HIDSTAT *st );

/*
 * Put one in as if a device had reported it: the interface src (0 for
 * none, the input they share). This is how a test or a remote input
 * drives the layers above without a person pressing anything, and it
 * is reached through the input devices' objects (OB_IN_EVENTS written),
 * whose protection says who may. What is put in this way is taken as
 * it is, without the attributes below.
 */
IMPORT ER  knl_hid_inject( UINT src, CONST T_HIDEV *ev );

/*
 * Whether the event ts_hid_read gave last is a finger's on a touch
 * screen; then *p_px100 is how many pixels of the view 100 mm of that
 * screen is, from the size the device says it is (guessed when it does
 * not say). The window manager asks it of each press it reads, for the
 * handles a finger reaches farther (wm_hit_slop). A mouse's, a pen's
 * and what was put in (knl_hid_inject) are not a finger's.
 */
IMPORT BOOL knl_hid_taken_touch( INT *p_px100 );

/* The key held let stop repeating (a window closing) */
IMPORT void knl_hid_repeat_cut( void );

/*
 * A keyboard or a pointer there is, as its object shows it. id names
 * the interface while it is plugged in; a new one is never given an id
 * that one before had.
 */
typedef struct {
	UINT	id;
	INT	usbdev;			/* the device it is on (T_USBDEV.dev) */
	INT	ifno;			/* its interface on that device */
	BOOL	keyboard;		/* a keyboard, else a pointer */
	BOOL	absolute;		/* a pointer that says where it is (a tablet) */
	BOOL	wheel;
	UINT	buttons;
	UD	reports;		/* reports taken off it */
} T_HIDINFO;

IMPORT INT knl_hid_list( T_HIDINFO *buf, INT max );

/*
 * A copy of the events, looked at without taking them from ts_hid_read:
 * those of interface src (0 every one, and what was put in) since *pos,
 * which a reader keeps (0 at first: from what comes next). Answers how
 * many were copied into buf; what fell out of the copy before it was
 * looked at is added to *p_lost.
 */
IMPORT INT knl_hid_tap( UD *pos, UINT src, T_HIDEV *buf, INT max, UD *p_lost );
IMPORT UD  knl_hid_tap_now( void );

/*
 * How the keyboard and the pointer are to be taken (ユーザ環境設定, design
 * 16.5.23): times in milliseconds. The window layer sets them from the
 * look table whenever one of those entries is written.
 *
 *	pd_on, pd_off	a button counts once held pd_on; a press within pd_off
 *			of the last let go is not taken
 *	speed		1 (the most sensitive) .. 15; 12 moves as the device does
 *	accel		0 (speeds up soonest) .. 7 (never)
 *	main		0: the left button is the main one, 1: the right
 *	absolute	a pointer that says where it is (a pen, a tablet): 1
 *			goes there, 0 moves by how far it moved
 *	middbl		the middle button is a double press of the main one
 *	key_on, key_off	as pd_on and pd_off, for keys
 *	key_sim		a shift pressed this soon after a key goes with it
 *	krp		keys repeat, krp_start after the press, then every krp_int
 *	sclk		a shift pressed and let go this quickly is one-shot, and
 *			twice within 2.5 times it locks; 0 neither
 *	tshift		each press of a shift goes 通常, 一時シフト, 簡易ロック
 *	tch_off		a finger's correction off: a double tap's second tap is
 *			not brought to the first
 *	tch_dbl		how far apart a double tap's taps may be, 0.1 mm of the
 *			screen; 0 the default (5 mm)
 *	tch_hold	held this long (ms), a finger is the second button; 0
 *			the default (1 s)
 *	tch_noscr	two fingers not taken together: a second finger is
 *			left alone, there is no scroll nor tap of two
 *	tch_scr		how far two fingers move for a notch of the wheel,
 *			0.1 mm of the screen; 0 the default (10 mm)
 */
typedef struct {
	UINT	pd_on, pd_off, speed, accel, main, absolute, middbl;
	UINT	key_on, key_off, key_sim, krp, krp_start, krp_int, sclk, tshift;
	UINT	tch_off, tch_dbl, tch_hold;
	UINT	tch_noscr, tch_scr;
} T_HIDATTR;

IMPORT ER  ts_hid_setattr( CONST T_HIDATTR *a );
IMPORT ER  ts_hid_getattr( T_HIDATTR *a );

#ifdef __cplusplus
}
#endif

#endif /* __TS_HID_H__ */
