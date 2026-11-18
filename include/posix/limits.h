/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	limits.h
 *	newlib's, with the POSIX and BSD limits it leaves out (design 5.8.3)
 */

#ifndef __POSIX_LIMITS_H__
#define __POSIX_LIMITS_H__

#include_next <limits.h>
#include <sys/syslimits.h>

#ifndef PATH_MAX
#define PATH_MAX		1024
#endif
#ifndef _POSIX2_LINE_MAX
#define _POSIX2_LINE_MAX	2048
#endif
#ifndef LINE_MAX
#define LINE_MAX		2048
#endif
#ifndef _POSIX_NAME_MAX
#define _POSIX_NAME_MAX		14
#endif
#ifndef _POSIX_PATH_MAX
#define _POSIX_PATH_MAX		256
#endif
/*
 * Bytes of one name of a path, the nought not counted: the most a name
 * of the file systems takes in UTF-8 (FS_NAME_MAX, 255 UTF-16 units of
 * FAT, exFAT and NTFS), as struct dirent's d_name holds it. newlib's
 * 255 is replaced.
 */
#undef NAME_MAX
#define NAME_MAX		767
#ifndef NL_TEXTMAX
#define NL_TEXTMAX		2048
#endif
#ifndef NL_ARGMAX
#define NL_ARGMAX		9
#endif
#ifndef SIZE_T_MAX
#define SIZE_T_MAX		__SIZE_MAX__
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX		__LONG_MAX__
#endif
#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX	255
#endif
#ifndef RE_DUP_MAX
#define RE_DUP_MAX		255
#endif
#ifndef ARG_MAX
#define ARG_MAX			( 256 * 1024 )
#endif
#ifndef CHILD_MAX
#define CHILD_MAX		1000
#endif
#ifndef OPEN_MAX
#define OPEN_MAX		256
#endif
#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX		64
#endif
#ifndef PIPE_BUF
#define PIPE_BUF		512
#endif
#ifndef QUAD_MAX
#define QUAD_MAX		__LONG_LONG_MAX__
#define QUAD_MIN		( -__LONG_LONG_MAX__ - 1 )
#define UQUAD_MAX		( __LONG_LONG_MAX__ * 2ULL + 1 )
#endif
#ifndef _POSIX_ARG_MAX
#define _POSIX_ARG_MAX		4096
#endif
#ifndef _POSIX_TTY_NAME_MAX
#define _POSIX_TTY_NAME_MAX	9
#endif
#ifndef TTY_NAME_MAX
#define TTY_NAME_MAX		32
#endif
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX		255
#endif
#ifndef _POSIX_HOST_NAME_MAX
#define _POSIX_HOST_NAME_MAX	255
#endif
#ifndef _POSIX_SYMLOOP_MAX
#define _POSIX_SYMLOOP_MAX	8
#endif

#endif /* __POSIX_LIMITS_H__ */
