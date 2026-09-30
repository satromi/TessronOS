/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_media.c
 *	バックアップ: where the volumes are (design 17.18)
 *
 *	A medium is a disk the system has as a device object: opened with
 *	the object layer and mounted through its key (fs_attach_dev) at
 *	/media/<its name>, or used where it is mounted already, and let go
 *	when the work is done if it was mounted here. /boot is always there.
 *	Beside the media, a volume may be a real object of its own.
 */

#include "bk.h"
#include "bk_vol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_MAX		32

/* Where a device's volume is mounted now, or FALSE */
LOCAL BOOL mounted_at( const TS_UUID *obj, const char *dev, char *dir, INT max )
{
	T_FSMNT	mt[FS_MAX_MOUNT];
	INT	n = fs_mounts(mt, FS_MAX_MOUNT), i;

	for ( i = 0; i < n && i < FS_MAX_MOUNT; i++ ) {
		if ( ( obj != NULL && memcmp(&mt[i].obj, obj, sizeof(TS_UUID)) == 0 )
		  || ( dev != NULL && dev[0] != 0 && strcmp((const char *)mt[i].dev, dev) == 0 ) ) {
			if ( dir != NULL ) {
				strncpy(dir, (const char *)mt[i].path, (size_t)max - 1);
				dir[max - 1] = 0;
			}
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT INT bk_media_list( BKMEDIA *m, INT max )
{
	TS_UUID	ids[DEV_MAX];
	T_OBREF	r;
	INT	cnt = 0, i, n = 0;
	char	dir[FS_PATH_MAX];

	if ( n < max ) {
		memset(&m[n], 0, sizeof(BKMEDIA));
		m[n].kind = BK_MED_OBJECT;
		strcpy(m[n].label, "実身");
		n++;
	}
	if ( n < max ) {
		memset(&m[n], 0, sizeof(BKMEDIA));
		m[n].kind = BK_MED_DIR;
		strcpy(m[n].label, "/boot");
		strcpy(m[n].dir, "/boot");
		n++;
	}
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, ids, DEV_MAX, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < DEV_MAX && n < max; i++ ) {
			/* the device manager lists every kind of device */
			if ( ob_ref_obj(&ids[i], &r) < E_OK || r.sub != OB_S_DISK ) continue;
			/* a disk the system holds for itself is not offered */
			if ( mounted_at(&ids[i], (const char *)r.name, dir, sizeof(dir))
			  && strncmp(dir, FS_MEDIA_DIR "/", strlen(FS_MEDIA_DIR) + 1) != 0 ) continue;
			memset(&m[n], 0, sizeof(BKMEDIA));
			m[n].kind = BK_MED_DIR;
			m[n].obj = ids[i];
			strncpy(m[n].dev, (const char *)r.name, sizeof(m[n].dev) - 1);
			snprintf(m[n].label, sizeof(m[n].label), "%s", m[n].dev);
			n++;
		}
	}
	return n;
}

EXPORT ER bk_media_open( BKMEDIA *m, BOOL write )
{
	ER	er;

	if ( m->kind != BK_MED_DIR || m->dev[0] == 0 ) return E_OK;
	if ( mounted_at(&m->obj, m->dev, m->dir, sizeof(m->dir)) ) return E_OK;
	m->key = ob_opn_obj(&m->obj, write ? ( OB_OP_R | OB_OP_WRITE ) : OB_OP_R);
	if ( m->key <= 0 ) {
		er = ( m->key < 0 ) ? m->key : E_NOEXS;
		m->key = 0;
		return er;
	}
	er = fs_attach_dev(m->key, "fatfs", write ? 0 : FS_MNT_RDONLY);
	if ( er < E_OK ) {
		ob_cls_obj(m->key);
		m->key = 0;
		return er;
	}
	m->mounted = TRUE;
	snprintf(m->dir, sizeof(m->dir), "%s/%s", FS_MEDIA_DIR, m->dev);
	return E_OK;
}

EXPORT void bk_media_close( BKMEDIA *m )
{
	if ( m->mounted ) {
		(void)fs_sync();
		(void)fs_detach_dev(m->key);
		m->mounted = FALSE;
	}
	if ( m->key > 0 ) ob_cls_obj(m->key);
	m->key = 0;
	if ( m->dev[0] != 0 ) m->dir[0] = 0;
}

/*
 * What a volume may take: on a medium its free blocks, less ten for the
 * file system; as an object, the free blocks of the volume the objects
 * go on, less what the volume object needs besides its bytes
 * (BK_OBJ_KEEP blocks). Never more than one volume holds.
 */
EXPORT UD bk_media_free( BKMEDIA *m )
{
	T_FSSTAT st;
	UD	cap;

	if ( m->kind == BK_MED_OBJECT ) {
		static const TS_UUID zero;
		T_OBVOL	v;
		TS_UUID	cab;

		if ( memcmp(&m->into, &zero, sizeof(zero)) != 0 ) cab = m->into;
		else if ( bk_first_cabinet(&cab) < E_OK ) return BK_OBJ_CAP;
		if ( ob_ref_vol(&cab, &v) < E_OK || v.bsize == 0 ) return BK_OBJ_CAP;
		cap = ( v.bfree > BK_OBJ_KEEP ) ? ( v.bfree - BK_OBJ_KEEP ) * v.bsize : 0;
		return ( cap > BK_OBJ_CAP ) ? BK_OBJ_CAP : cap;
	}
	if ( fs_statvfs(m->dir, &st) < E_OK || st.bsize == 0 ) return 0;
	cap = ( st.bfree > 10 ) ? ( st.bfree - 10 ) * st.bsize : 0;
	return ( cap > BK_OBJ_CAP ) ? BK_OBJ_CAP : cap;
}

LOCAL BOOL ends_tad( const UB *s )
{
	INT	n = (INT)strlen((const char *)s);

	return (BOOL)( n > 4 && s[n - 4] == '.' && ( s[n - 3] | 0x20 ) == 't'
		       && ( s[n - 2] | 0x20 ) == 'a' && ( s[n - 1] | 0x20 ) == 'd' );
}

EXPORT INT bk_media_files( BKMEDIA *m, char (*names)[FS_NAME_MAX], INT max )
{
	T_DIRENT de[8];
	INT	fd, k, i, n = 0;

	if ( m->kind != BK_MED_DIR || m->dir[0] == 0 ) return 0;
	fd = fs_open(m->dir, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) return 0;
	while ( n < max && ( k = fs_getdents(fd, de, 8) ) > 0 ) {
		for ( i = 0; i < k && n < max; i++ ) {
			if ( ( de[i].mode & FS_IFMT ) != FS_IFREG || !ends_tad(de[i].name) ) continue;
			strncpy(names[n], (const char *)de[i].name, FS_NAME_MAX - 1);
			names[n][FS_NAME_MAX - 1] = 0;
			n++;
		}
	}
	fs_close(fd);
	return n;
}
