/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	prcset.h
 *	A set of processors inside the kernel (design 4.3.2): bit n stands
 *	for PRC n+1, as in the T-Kernel's processor assignment, held in as
 *	many words of 64 bits as CNF_MAX_PRCID needs (one on the Raspberry
 *	Pi). Whatever goes through the processors goes through the online
 *	ones only (knl_num_prc), never up to the limit.
 */

#ifndef _PRCSET_H_
#define _PRCSET_H_

#define PRCSET_BITS		64
#define PRCSET_WORDS		( ( CNF_MAX_PRCID + PRCSET_BITS - 1 ) / PRCSET_BITS )

typedef struct {
	UBINT	w[PRCSET_WORDS];
} T_PRCSET;

#define PRCSET_W(i)		( (UINT)(i) / PRCSET_BITS )
#define PRCSET_B(i)		( (UBINT)1 << ( (UINT)(i) % PRCSET_BITS ) )

/* Empty */
Inline void prcset_zero( T_PRCSET *s )
{
	INT	k;

	for ( k = 0; k < PRCSET_WORDS; k++ ) s->w[k] = 0;
}

/* Every processor up to the limit (0..CNF_MAX_PRCID-1) */
Inline void prcset_fill( T_PRCSET *s )
{
	INT	k;

	for ( k = 0; k < PRCSET_WORDS; k++ ) s->w[k] = ~(UBINT)0;
#if CNF_MAX_PRCID % PRCSET_BITS != 0
	s->w[PRCSET_WORDS - 1] = ~(UBINT)0 >> ( PRCSET_BITS - CNF_MAX_PRCID % PRCSET_BITS );
#endif
}

Inline void prcset_add( T_PRCSET *s, INT i )
{
	s->w[PRCSET_W(i)] |= PRCSET_B(i);
}

Inline void prcset_del( T_PRCSET *s, INT i )
{
	s->w[PRCSET_W(i)] &= ~PRCSET_B(i);
}

Inline BOOL prcset_has( CONST T_PRCSET *s, INT i )
{
	return (BOOL)( ( s->w[PRCSET_W(i)] & PRCSET_B(i) ) != 0 );
}

Inline BOOL prcset_empty( CONST T_PRCSET *s )
{
	INT	k;

	for ( k = 0; k < PRCSET_WORDS; k++ ) {
		if ( s->w[k] != 0 ) return FALSE;
	}
	return TRUE;
}

/* d = a & b */
Inline void prcset_and( T_PRCSET *d, CONST T_PRCSET *a, CONST T_PRCSET *b )
{
	INT	k;

	for ( k = 0; k < PRCSET_WORDS; k++ ) d->w[k] = a->w[k] & b->w[k];
}

/* The first processor of s from i on, below n; -1 when there is none */
Inline INT prcset_next( CONST T_PRCSET *s, INT i, INT n )
{
	for ( ; i < n; i++ ) {
		UBINT	w = s->w[PRCSET_W(i)] >> ( (UINT)i % PRCSET_BITS );

		if ( w == 0 ) {
			i = (INT)( ( (UINT)i | ( PRCSET_BITS - 1 ) ) );	/* to the end of this word */
			continue;
		}
		i += __builtin_ctzl(w);
		return ( i < n ) ? i : -1;
	}
	return -1;
}

/* Of a task: whether it may run on processor i (an empty set: on any) */
Inline BOOL prcset_may( CONST T_PRCSET *s, INT i )
{
	return (BOOL)( prcset_empty(s) || prcset_has(s, i) );
}

#endif /* _PRCSET_H_ */
