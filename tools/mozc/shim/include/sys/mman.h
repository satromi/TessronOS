/*
 *	sys/mman.h -- mappings, as memory read from the file (ts_posix.cc)
 *	Copyright (C) 2026 satromi
 *	This software is distributed under the T-License 2.2.
 */
#ifndef TS_SYS_MMAN_H
#define TS_SYS_MMAN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE	0
#define PROT_READ	1
#define PROT_WRITE	2
#define PROT_EXEC	4
#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02
#define MAP_FIXED	0x10
#define MAP_ANONYMOUS	0x20
#define MAP_ANON	MAP_ANONYMOUS
#define MAP_NORESERVE	0x4000
#define MAP_FAILED	((void *)-1)
#define MADV_NORMAL	0
#define MADV_DONTNEED	4
#define MS_SYNC		4
#define MS_ASYNC	1

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t length, int flags);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
int madvise(void *addr, size_t length, int advice);

#ifdef __cplusplus
}
#endif
#endif
