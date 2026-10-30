/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	kernel.h (host)
 *	tools/mesa/hostshim's kernel types, and the little more the plan of
 *	compositing on Intel's render engine (device/gpu/intel/igt_rplan.c)
 *	uses, for building it into the host test of the shaders
 *	(tools/x64/mesa/test_igt_shaders.c).
 */

#ifndef _X64_HOSTSHIM_KERNEL_H_
#define _X64_HOSTSHIM_KERNEL_H_

#include "../../../mesa/hostshim/kernel.h"

#ifndef Inline
#define Inline		static inline
#endif
#define E_NOMEM		(-33)
#define E_LIMIT		(-34)

#endif /* _X64_HOSTSHIM_KERNEL_H_ */
