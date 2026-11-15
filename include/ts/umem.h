/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	umem.h
 *	Memory a process asks for (design 9.8.1): address space reserved
 *	in its own half, and pages made there and taken away again.
 *
 *	ts_map_mem reserves size bytes somewhere in [TS_MEM_BASE,
 *	TS_MEM_END): *p_adr, when it is not NULL, is where the program
 *	would like them (taken when that stretch is free and aligned),
 *	'align' a power of two of at least a page (0: a page). With
 *	TS_MEM_RW no page is made then: each is made, zero filled, the
 *	first time the process touches it or hands it to a call of the
 *	system, as the pages of an anonymous mapping are on Linux, so a
 *	large writable reservation costs only the pages used. With another
 *	protection other than TS_MEM_NONE the pages are made at once, zero
 *	filled; with TS_MEM_NONE only the addresses are held. When memory
 *	runs out at a first touch, the process ends with a fault.
 *
 *	ts_ctl_mem changes what a part of the reservations is: TS_MEM_RW
 *	makes the pages there writable and the others are made when
 *	touched; TS_MEM_READ makes the pages that are not there (zero
 *	filled) and gives all of them that protection; TS_MEM_NONE takes the pages
 *	away -- what they held is gone, and making them again gives
 *	zeros. ts_unm_mem takes the pages away and gives the addresses
 *	back. Both take a part of a reservation and split it as needed.
 *
 *	A page is never writable and executable at once (W^X).
 *	TS_MEM_EXEC (with or without TS_MEM_READ) makes the pages
 *	executable and read-only; what was written into them is made
 *	visible to the instruction fetch of every processor before they
 *	can be run. TS_MEM_RW | TS_MEM_EXEC (TS_MEM_JIT) asks for pages
 *	that are written and run by turns: each is made zero filled the
 *	first time it is touched, and is writable or executable, never
 *	both; a write to it while it is executable, or running it while it
 *	is writable, turns it the other way and the access goes on (the
 *	turn to executable makes the instructions fetchable as above). A
 *	compiler to machine code writes and runs such pages as if they were
 *	both. They are not to be handed to a call of the system: the
 *	kernel does not turn them for its own accesses.
 *
 *	ts_ctl_mem with TS_MEM_DISCARD (alone) gives up what the pages
 *	hold and keeps what they allow: the pages read as zeros after it
 *	(writable ones and those written and run by turns are taken away
 *	and go back to the system, to be made again when touched; the
 *	others are zero filled where they are).
 *
 *	A process may clean and invalidate the caches by address itself
 *	(dc cvau, ic ivau and CTR_EL0 are open to EL0).
 *
 *	Errors: E_PAR (not page aligned, not all of it reserved, a size
 *	of 0, an alignment that is not a power of two, a protection not
 *	known), E_NOMEM (no room for the addresses or no pages), E_MACV
 *	(p_adr not the process's to write).
 */

#ifndef __TS_UMEM_H__
#define __TS_UMEM_H__

#ifdef __cplusplus
extern "C" {
#endif

#define TS_MEM_NONE	0x00		/* reserved: no pages, no access */
#define TS_MEM_READ	0x01
#define TS_MEM_WRITE	0x02		/* read and write */
#define TS_MEM_RW	( TS_MEM_READ | TS_MEM_WRITE )
#define TS_MEM_EXEC	0x04		/* read and run, not written */
#define TS_MEM_JIT	( TS_MEM_RW | TS_MEM_EXEC )	/* written and run by turns */
#define TS_MEM_DISCARD	0x08		/* ts_ctl_mem: contents given up, protection kept */

/* Where the reservations are made: below the shared window (design 6.8) */
#define TS_MEM_BASE	0x0000100000000000ULL
#define TS_MEM_END	0x0000400000000000ULL

IMPORT ER ts_map_mem( void **p_adr, SZ size, SZ align, UINT prot );
IMPORT ER ts_unm_mem( void *adr, SZ size );
IMPORT ER ts_ctl_mem( void *adr, SZ size, UINT prot );

#ifdef __cplusplus
}
#endif

#endif /* __TS_UMEM_H__ */
