/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	actessronos.h
 *	The ACPI interpreter's (ACPICA's) configuration for the TessronOS
 *	kernel on x86-64: what its include/platform/acenv.h takes when
 *	__TESSRONOS_KERNEL__ is defined (tools/x64/acpica/build.sh puts that
 *	choice into its copy of acenv.h). The interpreter is built as the
 *	kernel is: 64 bits, no floating point, its own small C library
 *	(utclib.c, no system one), its own caches of objects, and no
 *	debugger or disassembler.
 */

#ifndef __ACTESSRONOS_H__
#define __ACTESSRONOS_H__

#define ACPI_MACHINE_WIDTH		64
#define COMPILER_DEPENDENT_INT64	long long
#define COMPILER_DEPENDENT_UINT64	unsigned long long

#define ACPI_USE_LOCAL_CACHE
#define ACPI_USE_DO_WHILE_0
#define ACPI_USE_NATIVE_DIVIDE
#undef ACPI_DEBUGGER
#undef ACPI_DISASSEMBLER

/* The cache written back before the machine sleeps or turns off */
#define ACPI_FLUSH_CPU_CACHE()		__asm__ volatile ( "wbinvd" ::: "memory" )

/* The firmware's global lock (in the FACS), taken and given back as ACPI lays it down (osl.c) */
extern int	AcpiOsAcquireGlobalLock( unsigned int *lock );
extern int	AcpiOsReleaseGlobalLock( unsigned int *lock );
#define ACPI_ACQUIRE_GLOBAL_LOCK(Facs, Acquired)	( (Acquired) = AcpiOsAcquireGlobalLock(&(Facs)->GlobalLock) )
#define ACPI_RELEASE_GLOBAL_LOCK(Facs, Pending)		( (Pending) = AcpiOsReleaseGlobalLock(&(Facs)->GlobalLock) )

#endif /* __ACTESSRONOS_H__ */
