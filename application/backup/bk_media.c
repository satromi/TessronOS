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
 *	the object layer, and its file system's root opened from that key
 *	(fs_opn_vol), the volumes' files named by paths below that root
 *	only, and let go when the work is done. The boot disk is always
 *	there: the system mounts it for itself and its device object is
 *	the system's alone, so its files are reached where the system has
 *	it in the file layer, without asking for the device. The person is
 *	shown each by its device object's name, never by the place the
 *	file layer has mounted it at. Beside the media, a volume may be a
 *	real object of its own.
 */

#include "bk.h"
#include "bk_vol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEV_PAGE	64		/* devices asked for at a time */

/* Where a device's volume is mounted now, or FALSE */
LOCAL BOOL mounted_at( const TS_UUID *obj, const char *dev, char *dir, INT max )
{
	T_FSMNT	*mt;
	INT	n = fs_mounts(NULL, 0), got, i;
	BOOL	hit = FALSE;

	/* as many as there are now, and a few more that may come meanwhile */
	if ( n <= 0 || ( mt = (T_FSMNT *)malloc(sizeof(T_FSMNT) * (size_t)( n + 4 )) ) == NULL ) {
		return FALSE;
	}
	got = fs_mounts(mt, n + 4);
	for ( i = 0; i < got && i < n + 4 && !hit; i++ ) {
		if ( ( obj != NULL && memcmp(&mt[i].obj, obj, sizeof(TS_UUID)) == 0 )
		  || ( dev != NULL && dev[0] != 0 && strcmp((const char *)mt[i].dev, dev) == 0 ) ) {
			if ( dir != NULL ) {
				strncpy(dir, (const char *)mt[i].path, (size_t)max - 1);
				dir[max - 1] = 0;
			}
			hit = TRUE;
		}
	}
	free(mt);
	return hit;
}

/* The name of the boot disk's device object, as the person is shown it ("起動ディスク" when none is said), and the object */
LOCAL void boot_name( char *out, INT max, TS_UUID *p_obj )
{
	T_FSMNT	*mt;
	T_OBREF	r;
	INT	n = fs_mounts(NULL, 0), got, i;

	snprintf(out, (size_t)max, "起動ディスク");
	if ( n <= 0 || ( mt = (T_FSMNT *)malloc(sizeof(T_FSMNT) * (size_t)( n + 4 )) ) == NULL ) return;
	got = fs_mounts(mt, n + 4);
	for ( i = 0; i < got && i < n + 4; i++ ) {
		if ( strcmp((const char *)mt[i].path, "/boot") == 0 && ob_ref_obj(&mt[i].obj, &r) >= E_OK && r.name[0] != 0 ) {
			snprintf(out, (size_t)max, "%s（起動ディスク）", (const char *)r.name);
			*p_obj = mt[i].obj;
			break;
		}
	}
	free(mt);
}

/* Room for n places in *p_m: FALSE for no memory */
LOCAL BOOL media_room( BKMEDIA **p_m, INT *p_cap, INT n )
{
	BKMEDIA	*t;
	INT	cap = ( *p_cap > 0 ) ? *p_cap : 16;

	if ( n <= *p_cap ) return TRUE;
	while ( cap < n ) cap *= 2;
	t = (BKMEDIA *)realloc(*p_m, sizeof(BKMEDIA) * (size_t)cap);
	if ( t == NULL ) return FALSE;
	*p_m = t;
	*p_cap = cap;
	return TRUE;
}

EXPORT INT bk_media_list( BKMEDIA **p_m )
{
	TS_UUID	ids[DEV_PAGE], last;
	T_OBREF	r;
	BKMEDIA	*m = NULL;
	INT	cnt, i, n = 0, cap = 0, all = 0;
	char	dir[FS_PATH_MAX];

	*p_m = NULL;
	if ( !media_room(&m, &cap, 2) ) return 0;
	memset(&m[n], 0, sizeof(BKMEDIA));
	m[n].kind = BK_MED_OBJECT;
	strcpy(m[n].label, "実身");
	n++;
	memset(&m[n], 0, sizeof(BKMEDIA));
	m[n].kind = BK_MED_DIR;
	m[n].boot = TRUE;
	strcpy(m[n].dir, "/boot");
	boot_name(m[n].label, sizeof(m[n].label), &m[n].obj);
	n++;
	/* every disk, a page at a time */
	do {
		cnt = 0;
		if ( ob_lst_obj(OB_T_DEVICE, OB_S_DISK, ( all > 0 ) ? &last : NULL, ids, DEV_PAGE, &cnt) < E_OK ) break;
		for ( i = 0; i < cnt; i++ ) {
			/* the device manager lists every kind of device */
			if ( ob_ref_obj(&ids[i], &r) < E_OK || r.sub != OB_S_DISK ) continue;
			/* a disk the system holds for itself is not offered */
			if ( mounted_at(&ids[i], (const char *)r.name, dir, sizeof(dir))
			  && strncmp(dir, FS_MEDIA_DIR "/", strlen(FS_MEDIA_DIR) + 1) != 0 ) continue;
			if ( !media_room(&m, &cap, n + 1) ) break;
			memset(&m[n], 0, sizeof(BKMEDIA));
			m[n].kind = BK_MED_DIR;
			m[n].obj = ids[i];
			strncpy(m[n].dev, (const char *)r.name, sizeof(m[n].dev) - 1);
			snprintf(m[n].label, sizeof(m[n].label), "%s", m[n].dev);
			n++;
		}
		if ( cnt > 0 ) last = ids[cnt - 1];
		all += cnt;
	} while ( cnt == DEV_PAGE );
	*p_m = m;
	return n;
}

EXPORT ER bk_media_open( BKMEDIA *m, BOOL write )
{
	static const TS_UUID zero;
	ER	er;

	if ( m->kind != BK_MED_DIR || m->mounted ) return E_OK;
	if ( m->boot || memcmp(&m->obj, &zero, sizeof(zero)) == 0 ) {
		return E_OK;			/* a place of the file layer's (the boot disk's, or a batch's) */
	}
	/* the backup written piece by piece: nobody reads it half written */
	m->key = ob_opn_obj(&m->obj, write ? ( OB_OP_R | OB_OP_WRITE | OB_O_EXCL ) : OB_OP_R);
	if ( m->key <= 0 ) {
		er = ( m->key < 0 ) ? m->key : E_NOEXS;
		m->key = 0;
		return er;
	}
	m->vd = fs_opn_vol(m->key, NULL, write ? 0 : FS_MNT_RDONLY);
	if ( m->vd < 0 ) {
		er = (ER)m->vd;
		ob_cls_obj(m->key);
		m->key = 0;
		return er;
	}
	m->mounted = TRUE;
	return E_OK;
}

EXPORT void bk_media_close( BKMEDIA *m )
{
	if ( m->mounted ) {
		(void)fs_close(m->vd);
		m->mounted = FALSE;
	}
	if ( m->key > 0 ) ob_cls_obj(m->key);
	m->key = 0;
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
	if ( ( m->mounted ? fs_fstatvfs(m->vd, &st) : fs_statvfs(m->dir, &st) ) < E_OK || st.bsize == 0 ) return 0;
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

	if ( m->kind != BK_MED_DIR || ( !m->mounted && m->dir[0] == 0 ) ) return 0;
	fd = m->mounted ? fs_openat(m->vd, ".", O_RDONLY | O_DIRECTORY) : fs_open(m->dir, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) return 0;
	/* every file there is counted; those past max are not given */
	while ( ( k = fs_getdents(fd, de, 8) ) > 0 ) {
		for ( i = 0; i < k; i++ ) {
			if ( ( de[i].mode & FS_IFMT ) != FS_IFREG || !ends_tad(de[i].name) ) continue;
			if ( n < max ) {
				strncpy(names[n], (const char *)de[i].name, FS_NAME_MAX - 1);
				names[n][FS_NAME_MAX - 1] = 0;
			}
			n++;
		}
	}
	fs_close(fd);
	return n;
}
