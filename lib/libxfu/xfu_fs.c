/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_fs.c
 *	A directory of the file layer (fs_) as a tree of files
 *
 *	The file layer names a file by a path of at most FS_PATH_MAX bytes,
 *	each name at most FS_NAME_MAX (any name FAT holds); a path longer
 *	is refused with E_LIMIT. A file's time is set with fs_utime where
 *	the volume keeps one.
 */

#include "xfu_in.h"
#include <ts/fs.h>

LOCAL T_XFUFS *fs_of( void *ctx )
{
	return (T_XFUFS *)ctx;
}

/* The whole path of a path of the tree, in fs->path */
LOCAL ER full( T_XFUFS *fs, CONST UB *path )
{
	ER	er = xfu_join((UB *)fs->path, sizeof(fs->path), (CONST UB *)fs->root, path);

	if ( er >= E_OK && xfu_slen((CONST UB *)fs->path) >= FS_PATH_MAX ) {
		er = E_LIMIT;
	}
	return er;
}

LOCAL INT t_open( void *ctx, CONST UB *path, UINT mode )
{
	T_XFUFS	*fs = fs_of(ctx);
	INT	h, fd;
	ER	er;

	for ( h = 0; h < XFU_FS_OPEN && fs->fd[h] >= 0; h++ ) ;
	if ( h == XFU_FS_OPEN ) {
		return E_LIMIT;
	}
	er = full(fs, path);
	if ( er < E_OK ) {
		return (INT)er;
	}
	fd = fs_open(fs->path, ( mode == XFU_O_WRITE ) ? ( O_WRONLY | O_CREAT | O_TRUNC ) : O_RDONLY);
	if ( fd < 0 ) {
		return ( fd == EX_NOENT ) ? E_NOEXS : E_IO;
	}
	fs->fd[h] = fd;
	return h;
}

LOCAL BOOL ok_h( T_XFUFS *fs, INT h )
{
	return (BOOL)( h >= 0 && h < XFU_FS_OPEN && fs->fd[h] >= 0 );
}

LOCAL INT t_read( void *ctx, INT h, void *buf, SZ n )
{
	T_XFUFS	*fs = fs_of(ctx);
	INT	k;

	if ( !ok_h(fs, h) ) {
		return E_ID;
	}
	k = fs_read(fs->fd[h], buf, n);
	return ( k < 0 ) ? E_IO : k;
}

LOCAL INT t_write( void *ctx, INT h, CONST void *buf, SZ n )
{
	T_XFUFS	*fs = fs_of(ctx);
	INT	k;

	if ( !ok_h(fs, h) ) {
		return E_ID;
	}
	k = fs_write(fs->fd[h], buf, n);
	return ( k < 0 ) ? ( ( k == EX_NOSPC ) ? E_LIMIT : E_IO ) : k;
}

LOCAL ER t_close( void *ctx, INT h )
{
	T_XFUFS	*fs = fs_of(ctx);
	ER	er;

	if ( !ok_h(fs, h) ) {
		return E_ID;
	}
	er = fs_close(fs->fd[h]);
	fs->fd[h] = -1;
	return ( er < 0 ) ? E_IO : E_OK;
}

LOCAL ER t_stat( void *ctx, CONST UB *path, T_XFUENT *e )
{
	T_XFUFS	*fs = fs_of(ctx);
	T_FSTAT	st;
	INT	i, base = 0;
	ER	er;

	er = full(fs, path);
	if ( er < E_OK ) {
		return er;
	}
	if ( fs_stat(fs->path, &st) < 0 ) {
		return E_NOEXS;
	}
	for ( i = 0; path[i] != 0; i++ ) {
		if ( path[i] == '/' && path[i + 1] != 0 ) base = i + 1;
	}
	(void)xfu_scpy(e->name, sizeof(e->name), path + base);
	e->kind = ( ( st.mode & FS_IFMT ) == FS_IFDIR ) ? XFU_K_DIR : XFU_K_FILE;
	e->size = (D)st.size;
	e->mtime = (D)st.mtime;
	return E_OK;
}

LOCAL ER t_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	T_XFUFS		*fs = fs_of(ctx);
	T_DIRENT	*de;
	T_XFUENT	*e;
	T_FSTAT		st;
	INT		fd, cnt, i, plen;
	ER		er;

	er = full(fs, path);
	if ( er < E_OK ) {
		return er;
	}
	fd = fs_open(fs->path, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) {
		return E_NOEXS;
	}
	plen = xfu_slen((CONST UB *)fs->path);
	de = (T_DIRENT *)xfu_sys_alloc(sizeof(T_DIRENT) * 8);
	e = (T_XFUENT *)xfu_sys_alloc(sizeof(T_XFUENT));
	er = ( de != NULL && e != NULL ) ? E_OK : E_NOMEM;
	while ( er >= E_OK && ( cnt = fs_getdents(fd, de, 8) ) > 0 ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( de[i].name[0] == '.' && ( de[i].name[1] == 0
			  || ( de[i].name[1] == '.' && de[i].name[2] == 0 ) ) ) {
				continue;
			}
			(void)xfu_scpy(e->name, sizeof(e->name), de[i].name);
			e->kind = ( ( de[i].mode & FS_IFMT ) == FS_IFDIR ) ? XFU_K_DIR : XFU_K_FILE;
			e->size = (D)de[i].size;
			e->mtime = 0;
			fs->path[plen] = 0;
			if ( xfu_join((UB *)fs->path, FS_PATH_MAX, (CONST UB *)fs->path, de[i].name) >= E_OK
			  && fs_stat(fs->path, &st) >= 0 ) {
				e->mtime = (D)st.mtime;
			}
			fs->path[plen] = 0;
			if ( cb(arg, e) < 0 ) {
				cnt = 0;
				break;
			}
		}
		if ( cnt == 0 ) break;
	}
	fs_close(fd);
	if ( de != NULL ) xfu_sys_free(de);
	if ( e != NULL ) xfu_sys_free(e);
	return er;
}

LOCAL ER t_mkdir( void *ctx, CONST UB *path )
{
	T_XFUFS	*fs = fs_of(ctx);
	ER	er = full(fs, path);

	if ( er < E_OK ) {
		return er;
	}
	return ( fs_mkdir(fs->path) < 0 ) ? E_IO : E_OK;
}

LOCAL ER t_utime( void *ctx, CONST UB *path, D mtime )
{
	T_XFUFS	*fs = fs_of(ctx);
	ER	er = full(fs, path);

	if ( er < E_OK ) {
		return er;
	}
	er = fs_utime(fs->path, (UD)mtime);
	return ( er >= 0 ) ? E_OK : ( er == EX_NOTSUP ) ? E_NOSPT : ( er == EX_NOENT ) ? E_NOEXS : E_IO;
}

EXPORT ER xfu_fs_tree( T_XFUFS *fs, CONST char *root, T_XFUTREE *t )
{
	INT	i;

	if ( fs == NULL || root == NULL || t == NULL ) {
		return E_PAR;
	}
	if ( xfu_scpy((UB *)fs->root, sizeof(fs->root), (CONST UB *)root) != xfu_slen((CONST UB *)root) ) {
		return E_LIMIT;
	}
	for ( i = 0; i < XFU_FS_OPEN; i++ ) fs->fd[i] = -1;
	t->ctx = fs;
	t->open = t_open;
	t->read = t_read;
	t->write = t_write;
	t->close = t_close;
	t->list = t_list;
	t->stat = t_stat;
	t->mkdir = t_mkdir;
	t->utime = t_utime;
	t->namemax = FS_NAME_MAX - 1;
	return E_OK;
}
