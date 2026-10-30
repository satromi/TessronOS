/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	hoststub.c (host)
 *	What Mesa's Intel device and compiler code names of libdrm but the
 *	host programs of tools/x64/mesa/igt_shaders.sh never call: they take
 *	the device by its PCI number, not from a descriptor.
 */

#include <stddef.h>

typedef struct _drmVersion	drmVersion;

int drmGetDevice2( int fd, unsigned flags, void **device )
{
	(void)fd;
	(void)flags;
	(void)device;
	return -1;
}

void drmFreeDevice( void **device )
{
	(void)device;
}

drmVersion *drmGetVersion( int fd )
{
	(void)fd;
	return NULL;
}

void drmFreeVersion( drmVersion *v )
{
	(void)v;
}
