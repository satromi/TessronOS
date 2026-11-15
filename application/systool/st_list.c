/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	st_list.c
 *	What the system's tools share besides what every list of links
 *	shares (lib/libtv/tv_list.c, tv_app.c): the window started on the
 *	tool's program object, and words (design 11.2.4)
 */

#include "st.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/obatr.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

BOOL	fg_cabinet = TRUE;

/* ---------------------------------------------------------------- words */

INT st_cat( UB *buf, INT at, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && at < max - 1 ) buf[at++] = (UB)*s++;
	buf[at] = 0;
	return at;
}

INT st_cat_n( UB *buf, INT at, INT max, INT v )
{
	char	num[16];

	snprintf(num, sizeof(num), "%d", ( v < 0 ) ? 0 : v);
	return st_cat(buf, at, max, num);
}

/* ---------------------------------------------------------------- the window */

ER st_start( T_TAHOST *host, CONST char *prog, T_OBREF *p_ref )
{
	TS_UUID	self;

	if ( ts_str_to_uuid(prog, &self) < E_OK ) return E_NOEXS;
	return ta_start_on(host, &self, p_ref);
}
