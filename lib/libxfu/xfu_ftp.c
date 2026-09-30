/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xfu_ftp.c
 *	A directory of an FTP server as a tree of files
 *
 *	A file open is a transfer under way (RETR or STOR), so one file is
 *	open at a time; the importer lists a directory whole before it opens
 *	anything in it, and the exporter writes one file after another. What
 *	a path is, is found in the listing of the directory it is in. A time
 *	is set only when the server offers MFMT.
 */

#include "xfu_in.h"

LOCAL T_XFUFTP *ft_of( void *ctx )
{
	return (T_XFUFTP *)ctx;
}

LOCAL ER full( T_XFUFTP *ft, CONST UB *path )
{
	return xfu_join(ft->path, sizeof(ft->path), ft->root, path);
}

LOCAL INT t_open( void *ctx, CONST UB *path, UINT mode )
{
	T_XFUFTP	*ft = ft_of(ctx);
	ER		er;

	if ( ft->busy ) {
		return E_BUSY;
	}
	er = full(ft, path);
	if ( er >= E_OK ) {
		er = ( mode == XFU_O_WRITE ) ? ftp_put(ft->ftp, ft->path) : ftp_get(ft->ftp, ft->path);
	}
	if ( er < E_OK ) {
		return (INT)er;
	}
	ft->busy = TRUE;
	ft->eof = FALSE;
	return 0;
}

/* After the end, the end again: the data connection has closed and is not read */
LOCAL INT t_read( void *ctx, INT h, void *buf, SZ n )
{
	T_XFUFTP	*ft = ft_of(ctx);
	INT		k;

	if ( !ft->busy || h != 0 ) {
		return E_ID;
	}
	if ( ft->eof ) {
		return 0;
	}
	k = ftp_read(ft->ftp, buf, n);
	if ( k == 0 ) ft->eof = TRUE;
	return k;
}

LOCAL INT t_write( void *ctx, INT h, CONST void *buf, SZ n )
{
	T_XFUFTP	*ft = ft_of(ctx);

	return ( ft->busy && h == 0 ) ? ftp_write(ft->ftp, buf, n) : E_ID;
}

LOCAL ER t_close( void *ctx, INT h )
{
	T_XFUFTP	*ft = ft_of(ctx);

	if ( !ft->busy || h != 0 ) {
		return E_ID;
	}
	ft->busy = FALSE;
	return ftp_end(ft->ftp);
}

/* A listing's names handed on as entries of the tree */
typedef struct {
	XFU_ENTCB	cb;
	void		*arg;
	T_XFUENT	*e;
	T_XFUFTP	*ft;		/* a stat: the name looked for */
	BOOL		found;
} LCTX;

LOCAL void to_ent( CONST T_FTPENT *fe, T_XFUENT *e )
{
	(void)xfu_scpy(e->name, sizeof(e->name), fe->name);
	e->kind = fe->dir ? XFU_K_DIR : XFU_K_FILE;
	e->size = fe->size;
	e->mtime = fe->mtime;
}

LOCAL INT l_each( void *arg, CONST T_FTPENT *fe )
{
	LCTX	*l = (LCTX *)arg;

	to_ent(fe, l->e);
	return l->cb(l->arg, l->e);
}

LOCAL INT l_find( void *arg, CONST T_FTPENT *fe )
{
	LCTX	*l = (LCTX *)arg;

	if ( !l->found && xfu_same(fe->name, (CONST char *)l->ft->want) ) {
		to_ent(fe, l->e);
		l->found = TRUE;
	}
	return 0;
}

LOCAL ER t_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	T_XFUFTP	*ft = ft_of(ctx);
	LCTX		l;
	ER		er;

	if ( ft->busy ) {
		return E_BUSY;
	}
	er = full(ft, path);
	if ( er < E_OK ) {
		return er;
	}
	l.cb = cb;
	l.arg = arg;
	l.e = (T_XFUENT *)xfu_sys_alloc(sizeof(T_XFUENT));
	if ( l.e == NULL ) {
		return E_NOMEM;
	}
	er = ftp_list(ft->ftp, ( ft->path[0] != 0 ) ? ft->path : NULL, l_each, &l);
	xfu_sys_free(l.e);
	return er;
}

LOCAL ER t_stat( void *ctx, CONST UB *path, T_XFUENT *e )
{
	T_XFUFTP	*ft = ft_of(ctx);
	LCTX		l;
	UB		*par;
	INT		i, base = -1;
	ER		er;

	if ( ft->busy ) {
		return E_BUSY;
	}
	if ( path == NULL || path[0] == 0 ) {
		e->name[0] = 0;
		e->kind = XFU_K_DIR;
		e->size = -1;
		e->mtime = 0;
		return E_OK;
	}
	for ( i = 0; path[i] != 0; i++ ) {
		if ( path[i] == '/' && path[i + 1] != 0 ) base = i;
	}
	(void)xfu_scpy(ft->want, sizeof(ft->want), path + base + 1);
	i = xfu_slen(ft->want);
	if ( i > 0 && ft->want[i - 1] == '/' ) ft->want[i - 1] = 0;
	/* the directory it is in */
	par = (UB *)xfu_sys_alloc(XFU_PATH_MAX);
	if ( par == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < base; i++ ) par[i] = path[i];
	par[( base > 0 ) ? base : 0] = 0;
	if ( base == 0 ) {
		par[0] = '/';
		par[1] = 0;
	}
	er = full(ft, par);
	xfu_sys_free(par);
	if ( er < E_OK ) {
		return er;
	}
	l.e = e;
	l.ft = ft;
	l.found = FALSE;
	er = ftp_list(ft->ftp, ( ft->path[0] != 0 ) ? ft->path : NULL, l_find, &l);
	if ( er >= E_OK && !l.found ) {
		er = E_NOEXS;
	}
	return er;
}

LOCAL ER t_mkdir( void *ctx, CONST UB *path )
{
	T_XFUFTP	*ft = ft_of(ctx);
	ER		er = full(ft, path);

	return ( er >= E_OK ) ? ftp_mkd(ft->ftp, ft->path) : er;
}

LOCAL ER t_utime( void *ctx, CONST UB *path, D mtime )
{
	T_XFUFTP	*ft = ft_of(ctx);
	ER		er = full(ft, path);

	return ( er >= E_OK ) ? ftp_mfmt(ft->ftp, ft->path, mtime) : er;
}

EXPORT ER xfu_ftp_tree( T_XFUFTP *ft, T_FTP *ftp, CONST UB *root, T_XFUTREE *t )
{
	if ( ft == NULL || ftp == NULL || t == NULL ) {
		return E_PAR;
	}
	ft->ftp = ftp;
	ft->busy = FALSE;
	if ( xfu_scpy(ft->root, sizeof(ft->root), root) != xfu_slen(root) ) {
		return E_LIMIT;
	}
	t->ctx = ft;
	t->open = t_open;
	t->read = t_read;
	t->write = t_write;
	t->close = t_close;
	t->list = t_list;
	t->stat = t_stat;
	t->mkdir = t_mkdir;
	t->utime = ( ( ftp->feat & FTP_F_MFMT ) != 0 ) ? t_utime : NULL;
	t->namemax = 0;
	return E_OK;
}
