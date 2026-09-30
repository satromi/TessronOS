/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cle/v3d_packet_helpers.h
 *	What the generated packet packing (../v3d_packet_v71_pack.h) calls,
 *	for the kernel (docs/tessronos/13-gpu.md 13.6.4).
 *
 *	The kernel is built without the floating point registers. Every
 *	helper that packs an integer field is plain shifting; the fixed
 *	point and float ones are macros, so the few packets with such
 *	fields cost nothing unless the kernel packs them, which it does not.
 *	The unpacking half of the generated file is left out (it is only
 *	compiled when __gen_unpack_address is defined).
 *
 *	The includer defines, before including the generated file:
 *	  __gen_user_data		what the pack functions pass through
 *	  __gen_address_type		how a field that is a GPU address is held
 *	  __gen_address_offset(a)	the 32 bit GPU address of such a field
 *	  __gen_emit_reloc(d, a)	told of every address field packed
 */

#ifndef _DEVICE_GPU_V3D_MESA_CLE_V3D_PACKET_HELPERS_H_
#define _DEVICE_GPU_V3D_MESA_CLE_V3D_PACKET_HELPERS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern void *memcpy( void *dst, const void *src, size_t n );

/* An unsigned field from bit 'start' to bit 'end' */
#define util_bitpack_uint(v, start, end) \
	( (uint64_t)(v) << (start) )

/* A signed field, cut to its width */
#define util_bitpack_sint(v, start, end) \
	( ( (uint64_t)(int64_t)(v) & ( ~0ULL >> ( 63 - ( (end) - (start) ) ) ) ) << (start) )

/* Fixed point fields, 'frac' bits below the point */
#define util_bitpack_ufixed(v, start, end, frac) \
	util_bitpack_uint((uint64_t)( (v) * (float)( 1 << (frac) ) ), start, end)
#define util_bitpack_sfixed(v, start, end, frac) \
	util_bitpack_sint((int64_t)( (v) * (float)( 1 << (frac) ) ), start, end)

/* The bits of a float, for the fields packed from the top half of one */
#define fui(f) \
	( ( (union { float f_; uint32_t u_; }){ .f_ = (f) } ).u_ )

/* The checks of the value ranges are left out */
#ifndef assert
#define assert(e)	( (void)0 )
#endif

#endif /* _DEVICE_GPU_V3D_MESA_CLE_V3D_PACKET_HELPERS_H_ */
