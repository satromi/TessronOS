/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_dirent.c
 *	opendir, readdir and closedir on the file layer (include/tsposix/
 *	dirent.h). A directory is opened with fs_open and read a few entries
 *	at a time with fs_getdents.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

/* The file layer's side (include/ts/fs.h) */
typedef struct {
	unsigned long long	ino;
	unsigned int		mode;
	unsigned long long	size;
	unsigned char		name[768];
} FS_DIRENT;

extern int fs_open( const char *path, unsigned int oflags );
extern int fs_close( int fd );
extern int fs_getdents( int fd, FS_DIRENT *buf, int nent );

#define FS_O_DIRECTORY	0x1000
#define FS_IFDIR	0x4000
#define FD_BASE		3		/* as ts_syscalls.c numbers the file layer's descriptors */
#define BATCH		4

struct ts_dir {
	int		fd;
	int		n, next;
	FS_DIRENT	buf[BATCH];
	struct dirent	ent;
};

DIR *opendir( const char *path )
{
	struct ts_dir	*d;
	int		fd = fs_open(path, FS_O_DIRECTORY);

	if ( fd < 0 ) {
		errno = ( fd > -200 ) ? -fd : ENOENT;
		return NULL;
	}
	d = (struct ts_dir *)calloc(1, sizeof(*d));
	if ( d == NULL ) {
		fs_close(fd);
		errno = ENOMEM;
		return NULL;
	}
	d->fd = fd;
	return d;
}

DIR *fdopendir( int fd )
{
	(void)fd;
	errno = ENOTSUP;
	return NULL;
}

struct dirent *readdir( DIR *d )
{
	FS_DIRENT	*e;

	if ( d->next >= d->n ) {
		d->n = fs_getdents(d->fd, d->buf, BATCH);
		d->next = 0;
		if ( d->n <= 0 ) {
			d->n = 0;
			return NULL;
		}
	}
	e = &d->buf[d->next++];
	d->ent.d_ino = (ino_t)e->ino;
	d->ent.d_off = 0;
	d->ent.d_reclen = sizeof(d->ent);
	d->ent.d_type = ( e->mode & FS_IFDIR ) ? DT_DIR : DT_REG;
	strncpy(d->ent.d_name, (const char *)e->name, sizeof(d->ent.d_name) - 1);
	d->ent.d_name[sizeof(d->ent.d_name) - 1] = 0;
	return &d->ent;
}

int readdir_r( DIR *d, struct dirent *e, struct dirent **res )
{
	struct dirent	*r = readdir(d);

	if ( r != NULL ) {
		*e = *r;
		*res = e;
	} else {
		*res = NULL;
	}
	return 0;
}

int closedir( DIR *d )
{
	fs_close(d->fd);
	free(d);
	return 0;
}

void rewinddir( DIR *d )
{
	(void)d;
}

int dirfd( DIR *d )
{
	return d->fd + FD_BASE;
}
