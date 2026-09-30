/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_posix.h
 *	The POSIX that the conversion engine's libraries expect, over newlib
 *
 *	The engine (mozc, abseil, protobuf, zlib) is built with the bare
 *	aarch64-none-elf toolchain: newlib for the C library and a
 *	single-threaded libstdc++. newlib declares most of POSIX but not
 *	threads, semaphores, mappings, directories or the XSI/GNU extras
 *	these libraries reach for. This header is included ahead of every
 *	translation unit; the missing headers sit in shim/include, and the
 *	functions are in ts_posix.cc.
 *
 *	The engine runs in one task. Everything thread-shaped here is for
 *	one thread: a mutex is always free, a condition never waited on
 *	for long, a key has one value.
 */

#ifndef TS_POSIX_H
#define TS_POSIX_H

#include <stddef.h>
#include <string.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sysconf names newlib does not number */
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE		8
#endif
#ifndef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX	70
#endif

#ifndef MAP_FAILED
#define MAP_FAILED	((void *)-1)
#endif

struct tm;
char *strptime(const char *s, const char *format, struct tm *tm);
int mkstemp(char *tmpl);
char *mkdtemp(char *tmpl);

struct FTW {
	int	base;
	int	level;
};
struct stat;
int nftw(const char *dirpath,
	 int (*fn)(const char *, const struct stat *, int, struct FTW *),
	 int nopenfd, int flags);
#ifndef FTW_F
#define FTW_F		0
#define FTW_D		1
#define FTW_DNR		2
#define FTW_NS		3
#define FTW_SL		4
#define FTW_DP		5
#define FTW_SLN		6
#define FTW_PHYS	1
#define FTW_MOUNT	2
#define FTW_CHDIR	4
#define FTW_DEPTH	8
#endif

/* the entropy abseil seeds its generators with (no /dev/urandom here) */
int ts_os_entropy(void *buf, unsigned int len);

#ifdef __cplusplus
}
#include <ts_std_mutex.h>
#endif

#endif /* TS_POSIX_H */
