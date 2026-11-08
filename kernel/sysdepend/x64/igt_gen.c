/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt_gen.c (x64 PC)
 *	Which Intel graphics a PCI device identifier names, from the 11th
 *	generation of the graphics on (docs/tessronos/13-gpu.md 13.11.16).
 *	Only a table: nothing here reaches the device. The 9th generation is
 *	not in it (igt_ggtt.c knows those identifiers), so a machine of that
 *	generation finds nothing here and goes its way as before.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"
#include "igt_gen.h"

LOCAL CONST T_IGTGEN	gens[] = {
	{ "Ice Lake", 110, FALSE, IGT_UC_NONE, IGT_READ_NO },	/* IGT_P_ICL */
	{ "Elkhart Lake", 110, FALSE, IGT_UC_NONE, IGT_READ_NO },	/* IGT_P_EHL */
	{ "Jasper Lake", 110, FALSE, IGT_UC_NONE, IGT_READ_NO },	/* IGT_P_JSL */
	{ "Tiger Lake", 120, FALSE, IGT_UC_NONE, IGT_READ_PRM },	/* IGT_P_TGL */
	{ "Rocket Lake", 120, FALSE, IGT_UC_NONE, IGT_READ_SAME },	/* IGT_P_RKL */
	{ "Alder Lake-S", 120, FALSE, IGT_UC_HUC, IGT_READ_SAME },	/* IGT_P_ADLS */
	{ "Alder Lake-P", 120, FALSE, IGT_UC_GUC, IGT_READ_SAME },	/* IGT_P_ADLP */
	{ "Alder Lake-N", 120, FALSE, IGT_UC_GUC, IGT_READ_SAME },	/* IGT_P_ADLN */
	{ "Raptor Lake-S", 120, FALSE, IGT_UC_GUC, IGT_READ_SAME },	/* IGT_P_RPLS */
	{ "Raptor Lake-P", 120, FALSE, IGT_UC_GUC, IGT_READ_SAME },	/* IGT_P_RPLP */
	{ "DG1", 120, TRUE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_DG1 */
	{ "DG2", 125, TRUE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_DG2 */
	{ "Arctic Sound-M", 125, TRUE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_ATSM */
	{ "Meteor Lake", 127, FALSE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_MTL */
	{ "Arrow Lake", 127, FALSE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_ARL */
	{ "Lunar Lake", 200, FALSE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_LNL */
	{ "Battlemage", 200, TRUE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_BMG */
	{ "Panther Lake", 300, FALSE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_PTL */
	{ "Wildcat Lake", 300, FALSE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_WCL */
	{ "Nova Lake", 350, FALSE, IGT_UC_GUC, IGT_READ_NO },	/* IGT_P_NVL */
};

enum { IGT_P_ICL, IGT_P_EHL, IGT_P_JSL, IGT_P_TGL, IGT_P_RKL, IGT_P_ADLS, IGT_P_ADLP, IGT_P_ADLN, IGT_P_RPLS, IGT_P_RPLP, IGT_P_DG1, IGT_P_DG2, IGT_P_ATSM, IGT_P_MTL, IGT_P_ARL, IGT_P_LNL, IGT_P_BMG, IGT_P_PTL, IGT_P_WCL, IGT_P_NVL };

/* Sorted by identifier: found by halving */
LOCAL CONST struct {
	UH	id;
	UB	gen;
} ids[] = {
	{ 0x4541, IGT_P_EHL }, { 0x4551, IGT_P_EHL }, { 0x4555, IGT_P_EHL },
	{ 0x4557, IGT_P_EHL }, { 0x4570, IGT_P_EHL }, { 0x4571, IGT_P_EHL },
	{ 0x4626, IGT_P_ADLP }, { 0x4628, IGT_P_ADLP }, { 0x462a, IGT_P_ADLP },
	{ 0x4680, IGT_P_ADLS }, { 0x4682, IGT_P_ADLS }, { 0x4688, IGT_P_ADLS },
	{ 0x468a, IGT_P_ADLS }, { 0x468b, IGT_P_ADLS }, { 0x4690, IGT_P_ADLS },
	{ 0x4692, IGT_P_ADLS }, { 0x4693, IGT_P_ADLS }, { 0x46a0, IGT_P_ADLP },
	{ 0x46a1, IGT_P_ADLP }, { 0x46a2, IGT_P_ADLP }, { 0x46a3, IGT_P_ADLP },
	{ 0x46a6, IGT_P_ADLP }, { 0x46a8, IGT_P_ADLP }, { 0x46aa, IGT_P_ADLP },
	{ 0x46b0, IGT_P_ADLP }, { 0x46b1, IGT_P_ADLP }, { 0x46b2, IGT_P_ADLP },
	{ 0x46b3, IGT_P_ADLP }, { 0x46c0, IGT_P_ADLP }, { 0x46c1, IGT_P_ADLP },
	{ 0x46c2, IGT_P_ADLP }, { 0x46c3, IGT_P_ADLP }, { 0x46d0, IGT_P_ADLN },
	{ 0x46d1, IGT_P_ADLN }, { 0x46d2, IGT_P_ADLN }, { 0x46d3, IGT_P_ADLN },
	{ 0x46d4, IGT_P_ADLN }, { 0x4905, IGT_P_DG1 }, { 0x4906, IGT_P_DG1 }, { 0x4907, IGT_P_DG1 },
	{ 0x4908, IGT_P_DG1 }, { 0x4909, IGT_P_DG1 }, { 0x4c8a, IGT_P_RKL }, { 0x4c8b, IGT_P_RKL },
	{ 0x4c8c, IGT_P_RKL }, { 0x4c90, IGT_P_RKL }, { 0x4c9a, IGT_P_RKL },
	{ 0x4e51, IGT_P_JSL }, { 0x4e55, IGT_P_JSL }, { 0x4e57, IGT_P_JSL },
	{ 0x4e61, IGT_P_JSL }, { 0x4e71, IGT_P_JSL }, { 0x5690, IGT_P_DG2 },
	{ 0x5691, IGT_P_DG2 }, { 0x5692, IGT_P_DG2 }, { 0x5693, IGT_P_DG2 }, { 0x5694, IGT_P_DG2 },
	{ 0x5695, IGT_P_DG2 }, { 0x5696, IGT_P_DG2 }, { 0x5697, IGT_P_DG2 }, { 0x56a0, IGT_P_DG2 },
	{ 0x56a1, IGT_P_DG2 }, { 0x56a2, IGT_P_DG2 }, { 0x56a3, IGT_P_DG2 }, { 0x56a4, IGT_P_DG2 },
	{ 0x56a5, IGT_P_DG2 }, { 0x56a6, IGT_P_DG2 }, { 0x56b0, IGT_P_DG2 }, { 0x56b1, IGT_P_DG2 },
	{ 0x56b2, IGT_P_DG2 }, { 0x56b3, IGT_P_DG2 }, { 0x56ba, IGT_P_DG2 }, { 0x56bb, IGT_P_DG2 },
	{ 0x56bc, IGT_P_DG2 }, { 0x56bd, IGT_P_DG2 }, { 0x56be, IGT_P_DG2 }, { 0x56bf, IGT_P_DG2 },
	{ 0x56c0, IGT_P_ATSM }, { 0x56c1, IGT_P_ATSM }, { 0x56c2, IGT_P_ATSM },
	{ 0x6420, IGT_P_LNL }, { 0x64a0, IGT_P_LNL }, { 0x64b0, IGT_P_LNL }, { 0x7d40, IGT_P_MTL },
	{ 0x7d41, IGT_P_ARL }, { 0x7d45, IGT_P_MTL }, { 0x7d51, IGT_P_ARL }, { 0x7d55, IGT_P_MTL },
	{ 0x7d60, IGT_P_MTL }, { 0x7d67, IGT_P_ARL }, { 0x7dd1, IGT_P_ARL }, { 0x7dd5, IGT_P_MTL },
	{ 0x8a50, IGT_P_ICL }, { 0x8a51, IGT_P_ICL }, { 0x8a52, IGT_P_ICL }, { 0x8a53, IGT_P_ICL },
	{ 0x8a54, IGT_P_ICL }, { 0x8a56, IGT_P_ICL }, { 0x8a57, IGT_P_ICL }, { 0x8a58, IGT_P_ICL },
	{ 0x8a59, IGT_P_ICL }, { 0x8a5a, IGT_P_ICL }, { 0x8a5b, IGT_P_ICL }, { 0x8a5c, IGT_P_ICL },
	{ 0x8a5d, IGT_P_ICL }, { 0x8a70, IGT_P_ICL }, { 0x8a71, IGT_P_ICL },
	{ 0x9a40, IGT_P_TGL }, { 0x9a49, IGT_P_TGL }, { 0x9a59, IGT_P_TGL }, { 0x9a60, IGT_P_TGL },
	{ 0x9a68, IGT_P_TGL }, { 0x9a70, IGT_P_TGL }, { 0x9a78, IGT_P_TGL }, { 0x9ac0, IGT_P_TGL },
	{ 0x9ac9, IGT_P_TGL }, { 0x9ad9, IGT_P_TGL }, { 0x9af8, IGT_P_TGL }, { 0xa720, IGT_P_RPLP },
	{ 0xa721, IGT_P_RPLP }, { 0xa780, IGT_P_RPLS }, { 0xa781, IGT_P_RPLS },
	{ 0xa782, IGT_P_RPLS }, { 0xa783, IGT_P_RPLS }, { 0xa788, IGT_P_RPLS },
	{ 0xa789, IGT_P_RPLS }, { 0xa78a, IGT_P_RPLS }, { 0xa78b, IGT_P_RPLS },
	{ 0xa7a0, IGT_P_RPLP }, { 0xa7a1, IGT_P_RPLP }, { 0xa7a8, IGT_P_RPLP },
	{ 0xa7a9, IGT_P_RPLP }, { 0xa7aa, IGT_P_RPLP }, { 0xa7ab, IGT_P_RPLP },
	{ 0xa7ac, IGT_P_RPLP }, { 0xa7ad, IGT_P_RPLP }, { 0xb080, IGT_P_PTL },
	{ 0xb081, IGT_P_PTL }, { 0xb082, IGT_P_PTL }, { 0xb083, IGT_P_PTL }, { 0xb084, IGT_P_PTL },
	{ 0xb085, IGT_P_PTL }, { 0xb086, IGT_P_PTL }, { 0xb087, IGT_P_PTL }, { 0xb08f, IGT_P_PTL },
	{ 0xb090, IGT_P_PTL }, { 0xb0a0, IGT_P_PTL }, { 0xb0b0, IGT_P_PTL }, { 0xb640, IGT_P_ARL },
	{ 0xd740, IGT_P_NVL }, { 0xd741, IGT_P_NVL }, { 0xd742, IGT_P_NVL }, { 0xd743, IGT_P_NVL },
	{ 0xd745, IGT_P_NVL }, { 0xd750, IGT_P_NVL }, { 0xd751, IGT_P_NVL }, { 0xd752, IGT_P_NVL },
	{ 0xd753, IGT_P_NVL }, { 0xd754, IGT_P_NVL }, { 0xd755, IGT_P_NVL }, { 0xd756, IGT_P_NVL },
	{ 0xd757, IGT_P_NVL }, { 0xd75f, IGT_P_NVL }, { 0xe202, IGT_P_BMG }, { 0xe209, IGT_P_BMG },
	{ 0xe20b, IGT_P_BMG }, { 0xe20c, IGT_P_BMG }, { 0xe20d, IGT_P_BMG }, { 0xe210, IGT_P_BMG },
	{ 0xe211, IGT_P_BMG }, { 0xe212, IGT_P_BMG }, { 0xe215, IGT_P_BMG }, { 0xe216, IGT_P_BMG },
	{ 0xe220, IGT_P_BMG }, { 0xe221, IGT_P_BMG }, { 0xe222, IGT_P_BMG }, { 0xe223, IGT_P_BMG },
	{ 0xfd80, IGT_P_WCL }, { 0xfd81, IGT_P_WCL },
};

EXPORT CONST T_IGTGEN *knl_igt_gen( UH id )
{
	INT	lo = 0, hi = (INT)( sizeof(ids) / sizeof(ids[0]) ) - 1;

	while ( lo <= hi ) {
		INT	m = ( lo + hi ) / 2;

		if ( ids[m].id == id ) return &gens[ids[m].gen];
		if ( ids[m].id < id ) {
			lo = m + 1;
		} else {
			hi = m - 1;
		}
	}
	return NULL;
}

#endif /* X64 */
