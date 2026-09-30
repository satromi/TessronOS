/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dirent.h
 *	Reading a directory of the file layer (fs_getdents), for a program
 *	running as a process (lib/libcxxrt/ts_dirent.c). The C library's own
 *	header refuses to be used on a machine without directories; this one
 *	stands in front of it.
 */

#ifndef __TSPOSIX_DIRENT_H__
#define __TSPOSIX_DIRENT_H__
#define _DIRENT_H_
#define _SYS_DIRENT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN	0
#define DT_DIR		4
#define DT_REG		8

#define NAME_MAX_TF	768

struct dirent {
	ino_t		d_ino;
	off_t		d_off;
	unsigned short	d_reclen;
	unsigned char	d_type;
	char		d_name[NAME_MAX_TF];
};

typedef struct ts_dir DIR;

DIR		*opendir( const char *path );
DIR		*fdopendir( int fd );
struct dirent	*readdir( DIR *d );
int		readdir_r( DIR *d, struct dirent *e, struct dirent **res );
int		closedir( DIR *d );
void		rewinddir( DIR *d );
int		dirfd( DIR *d );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_DIRENT_H__ */
