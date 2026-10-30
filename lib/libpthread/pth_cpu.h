/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_cpu.h
 *	What a thread of a program needs of its processor: the thread
 *	pointer, the place of the TLS block beside it, and a pause while it
 *	spins. One header for each architecture under sysdepend/, chosen
 *	by the compiler's own name for it (the programs are not built with
 *	the kernel's TARGET).
 */

#ifndef __PTH_CPU_H__
#define __PTH_CPU_H__

#if defined(__aarch64__)
#include "sysdepend/armv8a/pth_cpu.h"
#elif defined(__x86_64__)
#include "sysdepend/x86_64/pth_cpu.h"
#else
#error "lib/libpthread: no thread pointer for this architecture"
#endif

#endif /* __PTH_CPU_H__ */
