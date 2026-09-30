/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtsys.c
 *	The protection of what the desktop takes in for the system
 *
 *	The system's boxes and programs come as objects in the store
 *	(etc/def), owned by the system in their metadata. What is taken in
 *	into those boxes from outside -- faces, pictures, plugins -- is
 *	given the same owner here: the system, readable by all, not to be
 *	deleted.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include "desktop.h"

/* ---------------------------------------------------------------- the protection */

EXPORT void dt_sys_prt( T_OBPRT *prt )
{
	knl_memset(prt, 0, sizeof(*prt));
	prt->owner = ob_user_system;
	prt->group = ob_group_admin;
	prt->mode = 0755;
	prt->attr = OB_A_PERM;
}
