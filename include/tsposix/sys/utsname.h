/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/utsname.h
 *	The name of the system (uname): TessronOS on aarch64.
 */

#ifndef __TSPOSIX_SYS_UTSNAME_H__
#define __TSPOSIX_SYS_UTSNAME_H__

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
	char	sysname[65];
	char	nodename[65];
	char	release[65];
	char	version[65];
	char	machine[65];
	char	domainname[65];
};

int	uname( struct utsname *u );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_SYS_UTSNAME_H__ */
