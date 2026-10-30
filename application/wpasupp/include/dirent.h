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
 *	The directories OpenSSL's libcrypto names for the process "wpasupp"
 *	(tools/wpa/proc.sh), which newlib does not have for this system: the
 *	process has no files, so opendir finds none (wps_libc.c).
 */

#ifndef __WPS_DIRENT_H__
#define __WPS_DIRENT_H__

#include <sys/types.h>

struct dirent {
	ino_t	d_ino;
	char	d_name[256];
};

typedef struct __wps_dir DIR;

DIR		*opendir( const char *name );
struct dirent	*readdir( DIR *dir );
int		closedir( DIR *dir );

#endif
