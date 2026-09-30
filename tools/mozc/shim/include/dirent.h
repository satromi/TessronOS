/*
 *	dirent.h -- directories, which the engine lists nowhere (ts_posix.cc)
 *	Copyright (C) 2026 satromi
 *	This software is distributed under the T-License 2.2.
 */
#ifndef TS_DIRENT_H
#define TS_DIRENT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
	ino_t		d_ino;
	unsigned char	d_type;
	char		d_name[256];
};
#define DT_UNKNOWN	0
#define DT_DIR		4
#define DT_REG		8

typedef struct ts_dir DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#ifdef __cplusplus
}
#endif
#endif
