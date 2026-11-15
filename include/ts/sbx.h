/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts/sbx.h
 *	A process shut out of groups of the system's calls (design 5.9):
 *	a restricted process, for what runs code it cannot trust, as the
 *	browser's renderer runs a page's.
 *
 *	The restriction is a set of the groups below the process may no
 *	longer use. It only grows: ts_set_sbx adds groups and never takes
 *	one away, and a child a restricted process makes starts with its
 *	parent's set. It is the process's, all its tasks at once, from the
 *	call on. What a process holds already -- the keys of the objects
 *	it has open or was handed (a stream's end, memory, a BO), its files
 *	open, its own tasks and their objects -- it goes on using; it is
 *	the reaching of new ones that the groups take away.
 *
 *	A call of a group the process is shut out of ends the process
 *	(TS_ABORT_SBX, which waitpid reads as SIGSYS), and the console is
 *	told which call it was. With TS_SBX_REFUSE the call is refused
 *	instead (E_OACV, EX_PERM for the files) and the process goes on;
 *	that is for tests and for finding out what a program calls. Once
 *	a process is restricted without TS_SBX_REFUSE it cannot ask for it
 *	again.
 */

#ifndef __TS_SBX_H__
#define __TS_SBX_H__

#include <tk/typedef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The groups. A call belongs to the group of what it reaches; the
 * table is the kernel's (svc_gate.c, docs/tessronos/05-process.md 5.9).
 */
#define TS_SBX_OBJ	0x0001	/* objects by UUID or by name: opening, making a named
				   or a lasting one, deleting, looking up, links, the
				   attributes and protection by UUID; files and folders
				   by path; ts_lkp_obj */
#define TS_SBX_NET	0x0002	/* the network: so_, and opening a socket's object */
#define TS_SBX_DEV	0x0004	/* devices: sr_, us_, and opening a device's object
				   (sound, camera, USB, input, disks ...) */
#define TS_SBX_GPU	0x0008	/* the GPU: gp_, and opening the GPU's device object */
#define TS_SBX_PROC	0x0010	/* other processes: making one, ending, signalling,
				   waiting for or asking of one, messages and keys to
				   one, the list of them; opening a process's object */
#define TS_SBX_WIN	0x0020	/* the window layer: dp_, wm_, mn_, kc_, the tray, the
				   faces; opening a window's object */
#define TS_SBX_SYS	0x0040	/* the system's own: setting the clock and the zone,
				   volumes, login and passwords, the console's log */
#define TS_SBX_ALL	0x007f

/*
 * The sets for the kinds of process of the browser (14.13.9). The GPU's
 * process and the renderer that still draws on the GPU itself (until
 * the GPU's process is there) are shut out of the same groups.
 */
#define TS_SBX_RENDERER		TS_SBX_ALL
#define TS_SBX_RENDERER_GPU	( TS_SBX_ALL & ~TS_SBX_GPU )
#define TS_SBX_GPUPROC		( TS_SBX_ALL & ~TS_SBX_GPU )
#define TS_SBX_NETWORK		( TS_SBX_ALL & ~( TS_SBX_OBJ | TS_SBX_NET ) )
/*
 * The renderer while it still holds the stores of the pages' storage
 * (localStorage, IndexedDB, the passwords ...) and opens their objects
 * itself, still draws on the GPU, still opens the sound's, the
 * microphone's and the camera's device objects itself, still asks the
 * window layer itself (sharing the screen or a window, the tray's link
 * to a download, the message line) and still opens the sockets of peer
 * connections, WebTransport, push and direct sockets itself: shut out
 * of the other processes and the system's own. Each group comes back
 * into the set as its work moves out of the renderer.
 */
#define TS_SBX_RENDERER_STORE	( TS_SBX_ALL & ~( TS_SBX_OBJ | TS_SBX_GPU | TS_SBX_DEV | TS_SBX_WIN | TS_SBX_NET ) )

/* how: what a call of a group shut out does */
#define TS_SBX_REFUSE	0x0001	/* refused (E_OACV) instead of ending the process */

/* The groups added to the caller's set; E_OK, or E_PAR for an unknown group or how */
IMPORT ER  ts_set_sbx( UINT groups, UINT how );
/* The caller's set (TS_SBX_*), with TS_SBX_REFUSE << 16 when calls are refused */
IMPORT INT ts_get_sbx( void );

/*
 * lib/libcxxrt: what the library would open by name on first use (the
 * key of 乱数 that getrandom reads) opened, and then ts_set_sbx; 0, or
 * -1 with errno (EINVAL, EPERM)
 */
IMPORT int ts_sandbox_enter( unsigned groups, unsigned how );

/* The kernel's side (peripheral_kernel/sysman/proc.c) */
#define TS_SBX_HOW_SHIFT	16
IMPORT ER  knl_prc_set_sbx( UINT groups, UINT how );
IMPORT UW  knl_prc_sbx( void );		/* the caller's set and how, as ts_get_sbx */
IMPORT UW  knl_prc_sbx_of( ID pid );	/* those of process pid, 0 for none */
/*
 * A call shut out met behind the gateway, inside a call that has to come
 * out first: the caller marked to end as its call returns (fncd), and
 * whether it is (0, or the call that was)
 */
IMPORT void knl_prc_sbx_break( UINT fncd );
IMPORT UINT knl_prc_sbx_broke( void );
/*
 * The refusals of a process (design 5.9): how many calls of it were
 * turned away, in all and per group, and the call (function code) of the
 * last. Counted in every build, from the process's start (a child's start
 * from 0); knl_prc_sbx_count adds one for the caller's refusal of fncd in
 * groups and answers the groups it is the first of the process's for.
 * knl_prc_sbx_refused_of fills out[]: [0] the total, [1 + i] for the
 * group of bit i, [TS_SBX_NGROUP + 1] the last call; FALSE for no such
 * process.
 */
#define TS_SBX_NGROUP	7
IMPORT UINT knl_prc_sbx_count( UINT fncd, UINT groups );
IMPORT BOOL knl_prc_sbx_refused_of( ID pid, UW out[TS_SBX_NGROUP + 2] );
/* svc_gate.c: E_OK when the caller may reach what is of groups, else refused or ended for call fncd */
IMPORT ER  knl_sbx_may( UINT groups, UINT fncd );

#ifdef __cplusplus
}
#endif
#endif /* __TS_SBX_H__ */
