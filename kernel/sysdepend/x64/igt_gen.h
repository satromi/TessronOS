/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_gen.h (x64 PC)
 *	Which Intel graphics a PCI device identifier names, from the 11th
 *	generation of the graphics on (docs/tessronos/13-gpu.md 13.11.16).
 */

#ifndef _SYSDEPEND_X64_IGT_GEN_H_
#define _SYSDEPEND_X64_IGT_GEN_H_

/* How i915 uses the GuC by default on it: its default, not a requirement of the hardware */
#define IGT_UC_NONE	0	/* not at all: execlists from the host */
#define IGT_UC_HUC	1	/* only to authenticate the HuC: execlists from the host */
#define IGT_UC_GUC	2	/* submission goes through the GuC */

/* How far its registers may be read, nothing written in any case */
#define IGT_READ_NO	0	/* identifier only: no published manual was checked for its registers */
#define IGT_READ_PRM	1	/* read, at the places its own published manual gives */
#define IGT_READ_SAME	2	/* read, at the same places: same graphics version, manual not checked */

typedef struct {
	CONST char	*name;
	UH		verx10;		/* 110 is 11.0, 120 12.0, 125 12.5, 127 12.7, 200 Xe2 ... */
	BOOL		discrete;	/* has its own memory, not on 00:02.0 */
	UB		uc;
	UB		read;
} T_IGTGEN;

/* NULL: not of the 11th generation or later as far as the table knows */
IMPORT CONST T_IGTGEN	*knl_igt_gen( UH id );

#endif /* _SYSDEPEND_X64_IGT_GEN_H_ */
