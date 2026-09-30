/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	unp.h
 *	書庫解凍: the program's own parts, which make system calls
 */

#ifndef __UNP_H__
#define __UNP_H__

#include <ts/bpk.h>
#include <ts/uapp.h>
#include <ts/dtreq.h>

/* One archive object and what was made from it */
typedef struct {
	TS_UUID		archive;
	char		name[128];		/* the archive object's name */
	BOOL		autoref;		/* the store taken out into counts links itself */
	UB		*raw;
	UINT		rawlen;
	BPKARC		arc;
	TS_UUID		*made;			/* the object of each file */
	char		(*ids)[40];
	INT		nmade;
	TS_UUID		root;
	ID		curkey;			/* the object whose records are being written */
	INT		curfile;		/* and its file in the archive */
	UINT		vseq;
	INT		npic, noicon;
	UINT		nskipped;
	INT		nskip;
	BPKSKIP		skip[BPK_SKIP_KINDS];
	char		err[160];
} UNPJOB;

IMPORT void unp_uuid_str( const TS_UUID *u, char *out );
IMPORT BOOL unp_uuid_parse( const char *s, TS_UUID *u );
IMPORT UB  *unp_read_rec( ID key, INT recno, UINT *p_len );

/* The archive object read and its archive opened; err says why not */
IMPORT ER   unp_job_load( UNPJOB *j, const TS_UUID *archive );

/* The objects made and written, on the volume of into, the object they go into */
IMPORT ER   unp_job_run( UNPJOB *j, const TS_UUID *into,
			 void (*progress)( UNPJOB *j, INT done, INT total ) );
IMPORT void unp_job_undo( UNPJOB *j );
IMPORT void unp_job_free( UNPJOB *j );

/*
 * The root's link carried out of the window by the desktop, held at
 * (grab_x, grab_y) of its frame: E_OK and the object of the window it
 * was let go over, and the carry's seq; E_NOEXS let go where no link
 * goes, E_ABORT not wanted. Then the link put there with unp_place.
 */
IMPORT ER   unp_carry( UNPJOB *j, const T_DTLOOK *look, INT grab_x, INT grab_y,
		       TS_UUID *into, UINT *p_seq );
IMPORT ER   unp_place( UNPJOB *j, const T_DTLOOK *look, UINT seq );

IMPORT ER   ts_get_mono( UD *p_ns );

#endif /* __UNP_H__ */
