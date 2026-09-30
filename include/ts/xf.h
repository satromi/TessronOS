/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xf.h
 *	Files from outside taken in as real objects, and given out again
 *	(design 18.20)
 *
 *	Whatever arrives from outside the world of real objects -- sent over
 *	FTP, lying on a FAT32 partition, an SD card or a USB stick, put on
 *	/boot with the system -- is taken in here before anything uses it,
 *	and whatever leaves goes out from here. The object keeps the file's
 *	bytes unchanged in record 1 and an xmlTAD record 0 that shows it; an
 *	xmlTAD file is its own record 0, unchanged. What the file was -- its
 *	name, type, size, when it was changed and where it came from -- is
 *	the object's management information, "tessronos.file" in its metadata.
 */

#ifndef __TS_XF_H__
#define __TS_XF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/ob.h>

#define XF_NAME_MAX	256		/* bytes of a file's name, with its terminator */

/* Where the bytes come from: read gives up to n, 0 at the end, below 0 on a fault */
typedef INT (*XF_READ)( void *ctx, void *buf, SZ n );
typedef INT (*XF_WRITE)( void *ctx, CONST void *buf, SZ n );

typedef struct {
	CONST UB	*filename;	/* the file's name where it came from */
	CONST char	*source;	/* "boot", "ftp", "fat", "usb", "sd" */
	D		mtime;		/* when it was changed (TS_TIME), 0 when not known */
	XF_READ		read;
	void		*ctx;
	CONST char	*tessronos;	/* members added to the metadata's "tessronos", or NULL */
	CONST T_OBPRT	*prt;		/* NULL: the maker's default */
} T_XFSRC;

/* A file made a real object; with a box, a link to it is added to the box */
IMPORT ER  xf_import( CONST T_XFSRC *src, CONST TS_UUID *box, TS_UUID *p_uuid );

/* The same, the bytes read from a path of the file layer (fs_) */
IMPORT ER  xf_import_path( CONST char *path, CONST char *source, CONST char *tessronos,
			   CONST T_OBPRT *prt, CONST TS_UUID *box, TS_UUID *p_uuid );

/*
 * A TADjs object -- {uuid}.json, {uuid}_N.xtad and {uuid}.ico in a
 * directory -- taken in under the UUID its files are named by: the
 * metadata as it came with "tessronos.file" put in, each record as it
 * came. One the store has already is left as it is.
 */
IMPORT ER  xf_import_tadjs( CONST char *dir, CONST TS_UUID *uuid, CONST char *source,
			    CONST T_OBPRT *prt, CONST TS_UUID *box );

/* An object given out as a file: its bytes to write, its file name to name */
IMPORT ER  xf_export( CONST TS_UUID *uuid, XF_WRITE write, void *ctx,
		      UB *name, INT max );

/* The name an object goes out as */
IMPORT ER  xf_name( CONST TS_UUID *uuid, UB *name, INT max );

/* The object in a box that was taken in from a file of this name (case ignored) */
IMPORT ER  xf_find( CONST TS_UUID *box, CONST UB *filename, TS_UUID *p_uuid );

/* An object renamed as a file: its name, and "tessronos.file.name" when it has one */
IMPORT ER  xf_rename( CONST TS_UUID *uuid, CONST UB *filename );

/* The record the file's bytes are in: 1, or 0 for an xmlTAD file */
IMPORT INT xf_data_rec( CONST TS_UUID *uuid );

#ifdef __cplusplus
}
#endif

#endif /* __TS_XF_H__ */
