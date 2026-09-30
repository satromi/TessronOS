/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms_shared.c
 *	Micro Script: what every script on the machine shares
 *
 *	$GV[0]..$GV[49] and the global names of getgnm/setgnm/delgnm live
 *	in one memory object found by its name, MS_SHARED_NAME, mapped into
 *	each script's process at the same address. The first script makes
 *	it; it stays until the machine stops. A process that cannot map it
 *	keeps them to itself.
 *
 *	A C program may use the same object: the layout is MSSHARED below.
 */

#include "ms.h"
#include <string.h>

#define MS_SHARED_NAME	"tessronos.microscript.shared"
#define MS_SHARED_MAGIC	0x4D534748U	/* "MSGH" */
#define GNM_MAX		128
#define GNM_NAME	60

typedef struct {
	UW	magic;
	W	gv[50];
	struct {
		W	used;
		W	val;
		char	name[GNM_NAME];
	} gnm[GNM_MAX];
} MSSHARED;

static MSSHARED	*sh;
static MSSHARED	own;			/* when the shared one cannot be had */
static ID	shkey;

static MSSHARED *shared( void )
{
	T_OBCRE	c;
	T_OBMAP	m;
	TS_UUID	u;
	SZ	asz = 0;
	INT	recno = -1, tries;
	BOOL	made = FALSE;

	if ( sh != NULL ) return sh;
	sh = &own;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.flags = OB_F_GLOBAL;
	c.name = (const UB *)MS_SHARED_NAME;
	if ( ob_fnd_nam((const UB *)MS_SHARED_NAME, &u) < E_OK ) {
		if ( ob_cre_obj(&c, &u) >= E_OK ) made = TRUE;
		else if ( ob_fnd_nam((const UB *)MS_SHARED_NAME, &u) < E_OK ) return sh;
	}
	shkey = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	if ( shkey <= 0 ) { shkey = 0; return sh; }
	if ( made ) {
		MSSHARED	*z = ms_alloc(sizeof(MSSHARED));

		z->magic = MS_SHARED_MAGIC;
		if ( ob_apd_rec(shkey, OB_RT_SYSDATA, 0, &recno) >= E_OK ) {
			(void)ob_wri_rec(shkey, recno, 0, z, sizeof(MSSHARED), &asz);
		}
		ms_free(z);
	}
	/* one that another made may not have its record yet */
	for ( tries = 0; tries < 20; tries++ ) {
		memset(&m, 0, sizeof(m));
		m.flags = OB_M_WRITE;
		m.size = sizeof(MSSHARED);
		if ( ob_map_rec(shkey, 0, 0, &m) >= E_OK && m.addr != NULL ) {
			sh = (MSSHARED *)m.addr;
			return sh;
		}
		(void)tk_dly_tsk(10);
	}
	ob_cls_obj(shkey);
	shkey = 0;
	return sh;
}

INT ms_gv_get( INT i )
{
	return ( i >= 0 && i < 50 ) ? shared()->gv[i] : 0;
}

void ms_gv_set( INT i, INT v )
{
	if ( i >= 0 && i < 50 ) shared()->gv[i] = v;
}

static INT gnm_find( MSSHARED *s, const char *name )
{
	INT	i;

	for ( i = 0; i < GNM_MAX; i++ ) {
		if ( s->gnm[i].used && strncmp(s->gnm[i].name, name, GNM_NAME) == 0 ) return i;
	}
	return -1;
}

BOOL ms_gnm_get( const char *name, INT *p_val )
{
	MSSHARED	*s = shared();
	INT		i = gnm_find(s, name);

	if ( i < 0 ) return FALSE;
	*p_val = s->gnm[i].val;
	return TRUE;
}

BOOL ms_gnm_set( const char *name, INT val )
{
	MSSHARED	*s = shared();
	INT		i = gnm_find(s, name);

	if ( i < 0 ) {
		for ( i = 0; i < GNM_MAX && s->gnm[i].used; i++ ) ;
		if ( i == GNM_MAX ) return FALSE;
		strncpy(s->gnm[i].name, name, GNM_NAME - 1);
		s->gnm[i].name[GNM_NAME - 1] = 0;
		s->gnm[i].used = 1;
	}
	s->gnm[i].val = val;
	return TRUE;
}

BOOL ms_gnm_del( const char *name )
{
	MSSHARED	*s = shared();
	INT		i = gnm_find(s, name);

	if ( i < 0 ) return FALSE;
	s->gnm[i].used = 0;
	memset(s->gnm[i].name, 0, GNM_NAME);
	return TRUE;
}

/* Before the process ends: the mapping taken off, the object left for the others */
void ms_shared_end( void )
{
	if ( shkey > 0 ) {
		(void)ob_unm_rec(shkey, 0, 0);
		ob_cls_obj(shkey);
		shkey = 0;
	}
	sh = NULL;
}
