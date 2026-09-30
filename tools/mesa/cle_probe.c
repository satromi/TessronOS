/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cle_probe.c
 *	Packs a few V3D 7.1 control list packets with the packing code that
 *	tools/mesa/probe.sh generates from Mesa's packet description, and
 *	unpacks one of them again (docs/tessronos/13-gpu.md 13.6.4).
 *
 *	The same source is built for the host, where it runs and prints the
 *	bytes, and for a TessronOS process, where building and linking it is
 *	what shows the toolchain takes the generated code.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* What the generated code asks its user for: no relocations here */
#define __gen_user_data			void
#define __gen_address_type		uint32_t
#define __gen_address_offset(reloc)	(*(reloc))
#define __gen_emit_reloc(cl, reloc)
#define __gen_unpack_address(cl, s, e)	__gen_unpack_uint(cl, s, e)

#include "cle/v3d_packet_v71_pack.h"

#define PACK(buf, at, name, ...)					\
	do {								\
		struct V3D71_##name v = { V3D71_##name##_header, ##__VA_ARGS__ }; \
		V3D71_##name##_pack(NULL, (buf) + (at), &v);		\
		(at) += V3D71_##name##_length;				\
	} while (0)

int main( void )
{
	uint8_t	cl[64];
	int	at = 0, i;
	struct V3D71_TILE_BINNING_MODE_CFG back;

	memset(cl, 0, sizeof(cl));
	PACK(cl, at, TILE_BINNING_MODE_CFG,
	     .width_in_pixels = 1920, .height_in_pixels = 1080,
	     .log2_tile_width = TILE_WIDTH_64_PIXELS, .log2_tile_height = TILE_HEIGHT_64_PIXELS);
	PACK(cl, at, START_TILE_BINNING);
	PACK(cl, at, FLUSH);
	PACK(cl, at, END_OF_RENDERING);

	printf("cle_probe: %d bytes:", at);
	for ( i = 0; i < at; i++ ) {
		printf(" %02x", cl[i]);
	}
	printf("\n");

	V3D71_TILE_BINNING_MODE_CFG_unpack(cl, &back);
	printf("cle_probe: unpacked %ux%u, tiles log2 %u/%u: %s\n",
	       back.width_in_pixels, back.height_in_pixels,
	       back.log2_tile_width, back.log2_tile_height,
	       ( back.width_in_pixels == 1920 && back.height_in_pixels == 1080 ) ? "ok" : "WRONG");
	return 0;
}
