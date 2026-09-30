/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/stat.h
 *	newlib's, with lstat, which newlib declares only on some systems.
 *	The file layer has no symbolic links, so lstat is stat
 *	(lib/libcxxrt/ts_syscalls.c).
 */

#ifndef __TS_SYS_STAT_H__
#define __TS_SYS_STAT_H__

#include_next <sys/stat.h>

#if !defined(__SPU__) && !defined(__rtems__) && !defined(__CYGWIN__)
#ifdef __cplusplus
extern "C" {
#endif
int	lstat( const char *__restrict path, struct stat *__restrict st );
#ifdef __cplusplus
}
#endif
#endif

#endif /* __TS_SYS_STAT_H__ */
