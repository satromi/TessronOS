/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pth_emutls.c
 *	Thread local variables as the toolchain compiles them.
 *
 *	The compiler for aarch64-none-elf has no native thread local storage
 *	and turns every access to a thread local variable into a call of
 *	__emutls_get_address with the variable's control object. The
 *	runtime's own version was built for one thread and keeps one copy.
 *	Here each variable gets a number the first time any thread reaches
 *	it, and each thread a table of its copies by that number, made on
 *	first use from the variable's initial image (or zeros) and freed
 *	when the thread ends.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pth_int.h"

typedef struct {
	size_t	size;
	size_t	align;
	union {
		uintptr_t	index;		/* 1, 2, ...; 0 before first use */
		void		*ptr;
	} loc;
	void	*templ;
} EMUTLS_OBJ;

static uintptr_t	emutls_count;

static uintptr_t obj_index( EMUTLS_OBJ *o )
{
	uintptr_t	i = __atomic_load_n(&o->loc.index, __ATOMIC_ACQUIRE);

	if ( i == 0 ) {
		pth_glock();
		i = o->loc.index;
		if ( i == 0 ) {
			i = ++emutls_count;
			__atomic_store_n(&o->loc.index, i, __ATOMIC_RELEASE);
		}
		pth_gunlock();
	}
	return i;
}

static void *make_copy( EMUTLS_OBJ *o )
{
	size_t	a = ( o->align < sizeof(void *) ) ? sizeof(void *) : o->align;
	char	*raw, *p;

	/* the block keeps the address malloc gave just before the aligned copy */
	raw = (char *)malloc(o->size + a + sizeof(void *));
	if ( raw == NULL ) abort();
	p = (char *)( ( (uintptr_t)raw + sizeof(void *) + a - 1 ) & ~(uintptr_t)( a - 1 ) );
	((void **)p)[-1] = raw;
	if ( o->templ != NULL ) {
		memcpy(p, o->templ, o->size);
	} else {
		memset(p, 0, o->size);
	}
	return p;
}

void *__emutls_get_address( void *obj )
{
	EMUTLS_OBJ	*o = (EMUTLS_OBJ *)obj;
	PTH		*me = pth_me();
	uintptr_t	i = obj_index(o);
	void		**t;

	if ( me == NULL ) {
		me = pth_main();
	}
	if ( i > me->emutls_n ) {
		size_t	n = me->emutls_n ? me->emutls_n : 16;

		while ( n < i ) n *= 2;
		t = (void **)realloc(me->emutls, n * sizeof(void *));
		if ( t == NULL ) abort();
		memset(t + me->emutls_n, 0, ( n - me->emutls_n ) * sizeof(void *));
		me->emutls = t;
		me->emutls_n = n;
	}
	t = me->emutls;
	if ( t[i - 1] == NULL ) {
		t[i - 1] = make_copy(o);
	}
	return t[i - 1];
}

void __emutls_register_common( void *obj, size_t size, size_t align, void *templ )
{
	EMUTLS_OBJ	*o = (EMUTLS_OBJ *)obj;

	if ( o->size < size ) {
		o->size = size;
		o->templ = NULL;
	}
	if ( o->align < align ) {
		o->align = align;
	}
	if ( templ != NULL && size == o->size ) {
		o->templ = templ;
	}
}

/* The copies of a thread that ends */
void pth_emutls_free( PTH *me )
{
	size_t	i;

	for ( i = 0; i < me->emutls_n; i++ ) {
		if ( me->emutls[i] != NULL ) {
			free(((void **)me->emutls[i])[-1]);
		}
	}
	free(me->emutls);
	me->emutls = NULL;
	me->emutls_n = 0;
}
