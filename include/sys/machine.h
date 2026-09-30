/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2026/07.
 *
 *----------------------------------------------------------------------
 */

/*
 *	machine.h
 *	Machine type definition 
 */

#ifndef __SYS_MACHINE_H__
#define __SYS_MACHINE_H__

/* ===== System dependencies definitions ================================ */

/*
 * TessronOS のターゲットは makefile の TARGET(-D_RPI5_ / -D_QEMU_VIRT_)で選ぶ。
 * どちらも AArch64(armv8a コア)で、ボードとSoCの違いは sysdepend 配下で吸収する。
 */

#ifdef _RPI5_
#include "sysdepend/rpi5/machine.h"
#define Csym(sym) sym
#endif

#ifdef _QEMU_VIRT_
#include "sysdepend/qemu_virt/machine.h"
#define Csym(sym) sym
#endif

#if !defined(_RPI5_) && !defined(_QEMU_VIRT_)
#error "TARGET is not defined (use make TARGET=_RPI5_ or TARGET=_QEMU_VIRT_)"
#endif

/* ===== C compiler dependencies definitions ============================= */

#ifdef __GNUC__

#define Inline static __inline__
#define Asm __asm__ volatile
#define Noinit(decl) decl __attribute__((section(".noinit")))
#define	Section(decl,name) decl __attribute__((section(#name)))
#define WEAK_FUNC __attribute__((weak))

#define _VECTOR_ENTRY(name) .word name
#define _WEAK_ENTRY(name) .weak name

#endif /* __GNUC__ */

#endif /* __SYS_MACHINE_H__ */
