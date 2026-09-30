/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xf86drm.h (probe)
 *	The calls of the DRM user library that Mesa's v3d driver makes,
 *	declared only, so that tools/mesa/probe.sh can compile the driver
 *	and see what else it needs (docs/tessronos/13-gpu.md 13.6.4). The
 *	port gives them bodies over the GPU SVCs (13.6.2).
 */

#ifndef TS_PROBE_XF86DRM_H
#define TS_PROBE_XF86DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY	0
#define DRM_NODE_RENDER		2
#define DRM_CLOEXEC		0x80000
#define DRM_RDWR		0x02

typedef struct _drmVersion {
	int	version_major;
	int	version_minor;
	int	version_patchlevel;
	int	name_len;
	char	*name;
	int	date_len;
	char	*date;
	int	desc_len;
	char	*desc;
} drmVersion, *drmVersionPtr;

int	drmIoctl( int fd, unsigned long request, void *arg );
int	drmCommandWriteRead( int fd, unsigned long index, void *data, unsigned long size );
int	drmGetCap( int fd, uint64_t capability, uint64_t *value );
drmVersionPtr drmGetVersion( int fd );
void	drmFreeVersion( drmVersionPtr v );

int	drmPrimeHandleToFD( int fd, uint32_t handle, uint32_t flags, int *prime_fd );
int	drmPrimeFDToHandle( int fd, int prime_fd, uint32_t *handle );

int	drmSyncobjCreate( int fd, uint32_t flags, uint32_t *handle );
int	drmSyncobjDestroy( int fd, uint32_t handle );
int	drmSyncobjHandleToFD( int fd, uint32_t handle, int *obj_fd );
int	drmSyncobjFDToHandle( int fd, int obj_fd, uint32_t *handle );
int	drmSyncobjImportSyncFile( int fd, uint32_t handle, int sync_file_fd );
int	drmSyncobjExportSyncFile( int fd, uint32_t handle, int *sync_file_fd );
int	drmSyncobjWait( int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
			unsigned flags, uint32_t *first_signaled );
int	drmSyncobjReset( int fd, const uint32_t *handles, uint32_t handle_count );
int	drmSyncobjSignal( int fd, const uint32_t *handles, uint32_t handle_count );
int	drmSyncobjTimelineSignal( int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count );
int	drmSyncobjTimelineWait( int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
				int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled );
int	drmSyncobjTransfer( int fd, uint32_t dst_handle, uint64_t dst_point,
			    uint32_t src_handle, uint64_t src_point, uint32_t flags );

#ifdef __cplusplus
}
#endif

#endif /* TS_PROBE_XF86DRM_H */
