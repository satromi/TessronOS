/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	xf86drm.h
 *	The calls of the DRM user library that Mesa makes, for a TessronOS
 *	process (docs/tessronos/13-gpu.md 13.6.2). There is no device file:
 *	lib/libgl/gl_drm.c (the DRM shim) gives each request to the GPU
 *	SVCs (gp_, include/ts/gpuapp.h), keeps the synchronisation objects
 *	as fence numbers of its own, and finds no device to enumerate, so
 *	that the loader chooses the software driver when there is no GPU.
 */

#ifndef TS_SHIM_XF86DRM_H
#define TS_SHIM_XF86DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_DIR_NAME			"/dev/dri"
#define DRM_PRIMARY_MINOR_NAME		"card"
#define DRM_RENDER_MINOR_NAME		"renderD"
#define DRM_DEV_NAME			"%s/" DRM_PRIMARY_MINOR_NAME "%d"
#define DRM_RENDER_DEV_NAME		"%s/" DRM_RENDER_MINOR_NAME "%d"
#define DRM_RENDER_NODE_DEV_NAME_FORMAT	DRM_DIR_NAME "/" DRM_RENDER_MINOR_NAME "%d"
#define DRM_RENDER_NODE_MIN_MINOR	128
#define DRM_RENDER_NODE_MAX_NODES	63
#define DRM_RENDER_NODE_MAX_MINOR	( DRM_RENDER_NODE_MIN_MINOR + DRM_RENDER_NODE_MAX_NODES )

#define DRM_NODE_PRIMARY	0
#define DRM_NODE_CONTROL	1
#define DRM_NODE_RENDER		2
#define DRM_NODE_MAX		3

#define DRM_BUS_PCI		0
#define DRM_BUS_USB		1
#define DRM_BUS_PLATFORM	2
#define DRM_BUS_HOST1X		3

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

typedef struct _drmPciBusInfo {
	uint16_t	domain;
	uint8_t		bus;
	uint8_t		dev;
	uint8_t		func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
	uint16_t	vendor_id;
	uint16_t	device_id;
	uint16_t	subvendor_id;
	uint16_t	subdevice_id;
	uint8_t		revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

#define DRM_PLATFORM_DEVICE_NAME_LEN	512

typedef struct _drmPlatformBusInfo {
	char	fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
	char	**compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

#define DRM_HOST1X_DEVICE_NAME_LEN	512

typedef struct _drmHost1xBusInfo {
	char	fullname[DRM_HOST1X_DEVICE_NAME_LEN];
} drmHost1xBusInfo, *drmHost1xBusInfoPtr;

typedef struct _drmHost1xDeviceInfo {
	char	**compatible;
} drmHost1xDeviceInfo, *drmHost1xDeviceInfoPtr;

typedef struct _drmDevice {
	char	**nodes;		/* DRM_NODE_MAX of them */
	int	available_nodes;	/* a bit for each DRM_NODE_* */
	int	bustype;
	union {
		drmPciBusInfoPtr	pci;
		drmPlatformBusInfoPtr	platform;
		drmHost1xBusInfoPtr	host1x;
	} businfo;
	union {
		drmPciDeviceInfoPtr	pci;
		drmPlatformDeviceInfoPtr platform;
		drmHost1xDeviceInfoPtr	host1x;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

int	drmIoctl( int fd, unsigned long request, void *arg );
int	drmCommandWriteRead( int fd, unsigned long index, void *data, unsigned long size );
int	drmGetCap( int fd, uint64_t capability, uint64_t *value );
int	drmClose( int fd );
int	drmDestroyContext( int fd, uint32_t handle );
drmVersionPtr drmGetVersion( int fd );
void	drmFreeVersion( drmVersionPtr v );

int	drmGetDevices2( uint32_t flags, drmDevicePtr devices[], int max_devices );
#define DRM_DEVICE_GET_PCI_REVISION	( 1 << 0 )	/* drmGetDevice2: the PCI revision too */
int	drmGetDevice2( int fd, uint32_t flags, drmDevicePtr *device );
int	drmGetDeviceFromDevId( dev_t dev_id, uint32_t flags, drmDevicePtr *device );
void	drmFreeDevice( drmDevicePtr *device );
void	drmFreeDevices( drmDevicePtr devices[], int count );
int	drmDevicesEqual( drmDevicePtr a, drmDevicePtr b );
int	drmGetNodeTypeFromFd( int fd );
char	*drmGetDeviceNameFromFd2( int fd );
char	*drmGetPrimaryDeviceNameFromFd( int fd );
char	*drmGetRenderDeviceNameFromFd( int fd );

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
int	drmSyncobjQuery( int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count );
int	drmSyncobjQuery2( int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count,
			  uint32_t flags );
int	drmSyncobjTransfer( int fd, uint32_t dst_handle, uint64_t dst_point,
			    uint32_t src_handle, uint64_t src_point, uint32_t flags );

#ifdef __cplusplus
}
#endif

#endif /* TS_SHIM_XF86DRM_H */
