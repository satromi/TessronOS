/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_int.h
 *	Inside lib/libpthread: the record of a thread and the waiting
 *	underneath every lock (pth_wait.c).
 */

#ifndef __PTH_INT_H__
#define __PTH_INT_H__

#include <pthread.h>
#include <stdint.h>

#define PTH_NAME_MAX	16
#define PTH_DEF_STACK	( 1024 * 1024 )		/* a thread's stack when none is asked for */

/* What runs when a thread ends: thread_local objects (__cxa_thread_atexit) */
typedef struct pth_dtor {
	struct pth_dtor	*next;
	void		(*fn)( void * );
	void		*obj;
} PTH_DTOR;

/*
 * TPIDR_EL0 points at the thread's control block: 16 bytes whose first
 * word is the thread's record, followed (on the alignment the program's
 * TLS asks for) by the thread's copy of .tdata and .tbss. The compiler
 * reaches a thread_local variable at a fixed distance from TPIDR_EL0.
 */
typedef struct pth {
	int		tid;		/* the task */
	int		sem;		/* the semaphore it sleeps on, 0 until it first waits */
	void		*(*start)( void * );
	void		*arg;
	void		*ret;

	void		*map;		/* stack and TLS block as asked for (ts_map_mem) */
	size_t		mapsz;
	void		*stk_lo, *stk_hi;	/* the stack it runs on */
	void		*tls;		/* where TPIDR_EL0 points */

	volatile int	detached;
	volatile int	done;		/* it has run to its end */
	struct pth	*joiner;	/* who waits in pthread_join */

	struct pth	*qnext;		/* the list of whoever it waits for */
	volatile int	queued;		/* on such a list */

	void		*tsd[PTHREAD_KEYS_MAX];
	unsigned	tsdseq[PTHREAD_KEYS_MAX];
	PTH_DTOR	*dtors;
	char		name[PTH_NAME_MAX];
	void		**emutls;	/* its copies of the thread local variables (pth_emutls.c) */
	size_t		emutls_n;
} PTH;

/* The calling thread */
static inline PTH *pth_me( void )
{
	PTH	**tp;

	__asm__ volatile ( "mrs %0, tpidr_el0" : "=r"(tp) );
	return ( tp != NULL ) ? *tp : NULL;
}

/*
 * The program's TLS image (etc/linker/user/program_cxx.ld), and a new
 * control block and TLS block for a thread at 'at' (tls_block_size
 * bytes, aligned to tls_block_align): returns the value for TPIDR_EL0.
 */
size_t	pth_tls_size( void );
size_t	pth_tls_align( void );
void	*pth_tls_make( void *at, PTH *owner );

/* The one lock of the kernel's that guards every list of who waits */
void	pth_glock( void );
void	pth_gunlock( void );

/*
 * Sleep until woken or until the monotonic time 'dl' (nanoseconds, 0:
 * no end). Returns 0 when woken, ETIMEDOUT when the time came first.
 * A wake that came before the sleep is not lost.
 */
int	pth_park( PTH *me, uint64_t dl );
void	pth_unpark( PTH *t );
void	pth_prepare( PTH *me );		/* about to sleep for one who will pth_unpark it */

/* A list of who waits: [0] first, [1] last */
void	pth_enq( void **q, PTH *t );
PTH	*pth_deq( void **q );
int	pth_remove( void **q, PTH *t );		/* 1 when it was there */

/* Monotonic nanoseconds, and an absolute time on clock clk as monotonic nanoseconds */
uint64_t pth_now( void );
uint64_t pth_deadline( clockid_t clk, const struct timespec *abstime );

/* The thread records: the main thread's is made at start */
PTH	*pth_main( void );
void	pth_init_main( void *stk_lo, void *stk_hi, void *tls );

int	pth_mutex_lock_dl( pthread_mutex_t *m, uint64_t dl );
void	pth_emutls_free( PTH *me );

#endif /* __PTH_INT_H__ */
