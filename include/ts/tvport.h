/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tvport.h
 *	What the code that reads, lays out and draws xmlTAD stands on, the
 *	same in the kernel and in a program running as a process (design
 *	11.11, 16.4.3)
 *
 *	The reader of xmlTAD, the models of documents and figures, the
 *	layout of text, the drawing of figures and of virtual objects are
 *	one body of code, linked into the kernel and into a process alike.
 *	They draw with dp_ and fn_ and take memory with Kmalloc. In the
 *	kernel those are the kernel's own. In a process (built with
 *	TV_USER defined) the drawing and the letters are the system calls
 *	of the same names (lib/libts/svc_stub.S, lib/libtv/tv_fn.c), done
 *	in the kernel on the environment of the process's window, and the
 *	memory is the C library's.
 */

#ifndef __TS_TVPORT_H__
#define __TS_TVPORT_H__

#ifndef TV_USER

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>

#else /* TV_USER */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/uuid.h>
#include <stdlib.h>
#include <string.h>

#define Kmalloc(size)		malloc(size)
#define Kcalloc(n, size)	calloc((n), (size))
#define Krealloc(p, size)	realloc((p), (size))
#define Kfree(p)		free(p)

#define knl_memset(p, c, n)	memset((p), (c), (n))
#define knl_memcpy(d, s, n)	memcpy((d), (s), (n))
#define knl_memmove(d, s, n)	memmove((d), (s), (n))
#define knl_memcmp(a, b, n)	memcmp((a), (b), (n))
#define knl_strlen(s)		strlen(s)
#define knl_strcat(d, s)	strcat((d), (s))
#define knl_strcpy(d, s)	strcpy((d), (s))
#define knl_strcmp(a, b)	strcmp((a), (b))

#endif /* TV_USER */

#endif /* __TS_TVPORT_H__ */
