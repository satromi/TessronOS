/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	novfs_stub.c
 *	The file system names the rump kernel base refers to (design 12.6).
 *
 *	The rump kernel here has no file systems: it carries the network
 *	stack only. A few parts of its base (the vnode interface, the event
 *	queue's table of filters, the secmodel of mounting) still name what
 *	the file system faction would define. The data are defined here and
 *	never used; the functions stop the kernel, since nothing that would
 *	call them can exist without a file system.
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>

#include <uvm/uvm_extern.h>
#include <uvm/uvm_object.h>
#include <uvm/uvm_pager.h>

#include <miscfs/deadfs/deadfs.h>

pool_cache_t	pnbuf_cache;
int		(**dead_vnodeop_p)(void *);

const struct uvm_pagerops	uvm_vnodeops;
const struct filterops		fs_filtops;

int	rumpnovfs_stub(void);

int
rumpnovfs_stub(void)
{

	panic("no file systems in this rump kernel");
}

__weak_alias(holdrelel, rumpnovfs_stub);
__weak_alias(vhold, rumpnovfs_stub);
__weak_alias(vn_lock, rumpnovfs_stub);
__weak_alias(ubc_uiomove, rumpnovfs_stub);
__weak_alias(usermount_common_policy, rumpnovfs_stub);
