/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_start.c
 *	From _start to main (lib/libcxxrt, design 17.19).
 *
 *	The kernel gives a process's main task a stack of 64KB. A program in
 *	C++ -- an engine that recurses through a document, an interpreter
 *	that keeps its frames on the machine stack -- needs more, so the main
 *	thread gets a stack of __ts_main_stack_size bytes (8MB unless the
 *	program defines the variable itself) asked for with ts_map_mem, with a
 *	page never made below it, and its thread local storage next to it.
 *	The constructors run there, then main, and exit() with what main
 *	returned: the atexit handlers and the destructors, then ts_ext_prc.
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/umem.h>
#include <stdlib.h>
#include <string.h>

#define PAGE	4096UL

IMPORT void ts_ext_prc( INT exitcd );
IMPORT ER   tm_putstring( CONST UB *s );

/* lib/libpthread */
typedef struct pth PTH;
extern size_t	pth_tls_size( void );
extern void	*pth_tls_make( void *at, PTH *owner );
extern PTH	*pth_main( void );
extern void	pth_init_main( void *stk_lo, void *stk_hi, void *tls );
extern void	*__ts_call_on_stack( void *sp, void *(*fn)( void * ), void *arg );

extern void	__libc_init_array( void );
extern int	main( int argc, char **argv, char **envp );
extern char	**environ;
extern void	__ts_time_init( void );

size_t __ts_main_stack_size __attribute__((weak)) = 8UL * 1024 * 1024;

static char	*argv0[2] = { (char *)"program", NULL };

static inline size_t up( size_t v, size_t a )
{
	return ( v + a - 1 ) & ~( a - 1 );
}

static void *main_body( void *arg )
{
	(void)arg;
	__libc_init_array();
	exit(main(1, argv0, environ));
	return NULL;
}

void __ts_start( void )
{
	size_t	stk = up(__ts_main_stack_size, PAGE);
	size_t	tls = up(pth_tls_size(), PAGE);
	void	*map = NULL;
	char	*lo, *hi;
	void	*tp;

	if ( ts_map_mem(&map, (SZ)( PAGE + stk + tls ), 0, TS_MEM_NONE) < E_OK
	  || ts_ctl_mem((char *)map + PAGE, (SZ)( stk + tls ), TS_MEM_RW) < E_OK ) {
		tm_putstring((CONST UB *)"libcxxrt: no memory for the main thread's stack\n");
		ts_ext_prc(-1);
	}
	lo = (char *)map + PAGE;
	hi = lo + stk;
	tp = pth_tls_make(hi, pth_main());
	__asm__ volatile ( "msr tpidr_el0, %0" :: "r"(tp) : "memory" );
	pth_init_main(lo, hi, tp);
	__ts_time_init();

	(void)__ts_call_on_stack(hi, main_body, NULL);
	ts_ext_prc(0);
}
