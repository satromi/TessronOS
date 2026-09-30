/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	figgeom.c
 *	The geometry of a figure's shapes (design 17.13)
 *
 *	Every shape comes down to an outline: a run of points, open or
 *	closed. Filling is filling the outline and a line is the outline
 *	drawn, whatever the shape was -- a rectangle with rounded corners,
 *	a sector, a chord, a piece of an ellipse, a curve through points,
 *	and any of them turned round. One way of drawing means one way of
 *	being right.
 *
 *	There is no floating point here. An angle is in 4096ths of a turn,
 *	a sine is in 16384ths, and the tables below are a quarter of a sine
 *	and the arctangent of the ratios 0 to 1 in 1024ths.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dp.h>
#include <ts/tad.h>
#include <ts/tadview.h>

LOCAL CONST UH sin_q[1025] = {
	0, 25, 50, 75, 101, 126, 151, 176, 201, 226, 251, 276,
	302, 327, 352, 377, 402, 427, 452, 477, 503, 528, 553, 578,
	603, 628, 653, 678, 704, 729, 754, 779, 804, 829, 854, 879,
	904, 929, 955, 980, 1005, 1030, 1055, 1080, 1105, 1130, 1155, 1180,
	1205, 1230, 1255, 1280, 1306, 1331, 1356, 1381, 1406, 1431, 1456, 1481,
	1506, 1531, 1556, 1581, 1606, 1631, 1656, 1681, 1706, 1731, 1756, 1781,
	1806, 1831, 1856, 1881, 1906, 1931, 1956, 1981, 2006, 2031, 2055, 2080,
	2105, 2130, 2155, 2180, 2205, 2230, 2255, 2280, 2305, 2329, 2354, 2379,
	2404, 2429, 2454, 2479, 2503, 2528, 2553, 2578, 2603, 2628, 2652, 2677,
	2702, 2727, 2752, 2776, 2801, 2826, 2851, 2875, 2900, 2925, 2949, 2974,
	2999, 3024, 3048, 3073, 3098, 3122, 3147, 3172, 3196, 3221, 3246, 3270,
	3295, 3320, 3344, 3369, 3393, 3418, 3442, 3467, 3492, 3516, 3541, 3565,
	3590, 3614, 3639, 3663, 3688, 3712, 3737, 3761, 3786, 3810, 3835, 3859,
	3883, 3908, 3932, 3957, 3981, 4005, 4030, 4054, 4078, 4103, 4127, 4151,
	4176, 4200, 4224, 4249, 4273, 4297, 4321, 4346, 4370, 4394, 4418, 4442,
	4467, 4491, 4515, 4539, 4563, 4587, 4612, 4636, 4660, 4684, 4708, 4732,
	4756, 4780, 4804, 4828, 4852, 4876, 4900, 4924, 4948, 4972, 4996, 5020,
	5044, 5068, 5092, 5115, 5139, 5163, 5187, 5211, 5235, 5259, 5282, 5306,
	5330, 5354, 5377, 5401, 5425, 5449, 5472, 5496, 5520, 5543, 5567, 5591,
	5614, 5638, 5661, 5685, 5708, 5732, 5756, 5779, 5803, 5826, 5850, 5873,
	5897, 5920, 5943, 5967, 5990, 6014, 6037, 6060, 6084, 6107, 6130, 6154,
	6177, 6200, 6223, 6247, 6270, 6293, 6316, 6339, 6363, 6386, 6409, 6432,
	6455, 6478, 6501, 6524, 6547, 6570, 6593, 6616, 6639, 6662, 6685, 6708,
	6731, 6754, 6777, 6800, 6823, 6846, 6868, 6891, 6914, 6937, 6960, 6982,
	7005, 7028, 7050, 7073, 7096, 7118, 7141, 7164, 7186, 7209, 7231, 7254,
	7276, 7299, 7321, 7344, 7366, 7389, 7411, 7434, 7456, 7478, 7501, 7523,
	7545, 7568, 7590, 7612, 7635, 7657, 7679, 7701, 7723, 7746, 7768, 7790,
	7812, 7834, 7856, 7878, 7900, 7922, 7944, 7966, 7988, 8010, 8032, 8054,
	8076, 8098, 8119, 8141, 8163, 8185, 8207, 8228, 8250, 8272, 8293, 8315,
	8337, 8358, 8380, 8401, 8423, 8445, 8466, 8488, 8509, 8531, 8552, 8573,
	8595, 8616, 8638, 8659, 8680, 8702, 8723, 8744, 8765, 8787, 8808, 8829,
	8850, 8871, 8892, 8914, 8935, 8956, 8977, 8998, 9019, 9040, 9061, 9082,
	9102, 9123, 9144, 9165, 9186, 9207, 9227, 9248, 9269, 9290, 9310, 9331,
	9352, 9372, 9393, 9413, 9434, 9455, 9475, 9496, 9516, 9537, 9557, 9577,
	9598, 9618, 9638, 9659, 9679, 9699, 9720, 9740, 9760, 9780, 9800, 9820,
	9841, 9861, 9881, 9901, 9921, 9941, 9961, 9981, 10001, 10020, 10040, 10060,
	10080, 10100, 10120, 10139, 10159, 10179, 10198, 10218, 10238, 10257, 10277, 10296,
	10316, 10336, 10355, 10374, 10394, 10413, 10433, 10452, 10471, 10491, 10510, 10529,
	10549, 10568, 10587, 10606, 10625, 10644, 10663, 10683, 10702, 10721, 10740, 10759,
	10778, 10796, 10815, 10834, 10853, 10872, 10891, 10909, 10928, 10947, 10966, 10984,
	11003, 11021, 11040, 11059, 11077, 11096, 11114, 11133, 11151, 11169, 11188, 11206,
	11224, 11243, 11261, 11279, 11297, 11316, 11334, 11352, 11370, 11388, 11406, 11424,
	11442, 11460, 11478, 11496, 11514, 11532, 11550, 11567, 11585, 11603, 11621, 11638,
	11656, 11674, 11691, 11709, 11727, 11744, 11762, 11779, 11797, 11814, 11831, 11849,
	11866, 11883, 11901, 11918, 11935, 11952, 11970, 11987, 12004, 12021, 12038, 12055,
	12072, 12089, 12106, 12123, 12140, 12157, 12173, 12190, 12207, 12224, 12240, 12257,
	12274, 12290, 12307, 12324, 12340, 12357, 12373, 12390, 12406, 12423, 12439, 12455,
	12472, 12488, 12504, 12520, 12537, 12553, 12569, 12585, 12601, 12617, 12633, 12649,
	12665, 12681, 12697, 12713, 12729, 12744, 12760, 12776, 12792, 12807, 12823, 12839,
	12854, 12870, 12885, 12901, 12916, 12932, 12947, 12963, 12978, 12993, 13008, 13024,
	13039, 13054, 13069, 13085, 13100, 13115, 13130, 13145, 13160, 13175, 13190, 13205,
	13219, 13234, 13249, 13264, 13279, 13293, 13308, 13323, 13337, 13352, 13366, 13381,
	13395, 13410, 13424, 13439, 13453, 13467, 13482, 13496, 13510, 13524, 13538, 13553,
	13567, 13581, 13595, 13609, 13623, 13637, 13651, 13665, 13678, 13692, 13706, 13720,
	13733, 13747, 13761, 13774, 13788, 13802, 13815, 13829, 13842, 13856, 13869, 13882,
	13896, 13909, 13922, 13935, 13949, 13962, 13975, 13988, 14001, 14014, 14027, 14040,
	14053, 14066, 14079, 14092, 14104, 14117, 14130, 14143, 14155, 14168, 14181, 14193,
	14206, 14218, 14231, 14243, 14256, 14268, 14280, 14293, 14305, 14317, 14329, 14341,
	14354, 14366, 14378, 14390, 14402, 14414, 14426, 14438, 14449, 14461, 14473, 14485,
	14497, 14508, 14520, 14531, 14543, 14555, 14566, 14578, 14589, 14601, 14612, 14623,
	14635, 14646, 14657, 14668, 14680, 14691, 14702, 14713, 14724, 14735, 14746, 14757,
	14768, 14779, 14789, 14800, 14811, 14822, 14832, 14843, 14854, 14864, 14875, 14885,
	14896, 14906, 14917, 14927, 14937, 14948, 14958, 14968, 14978, 14989, 14999, 15009,
	15019, 15029, 15039, 15049, 15059, 15069, 15078, 15088, 15098, 15108, 15118, 15127,
	15137, 15146, 15156, 15166, 15175, 15184, 15194, 15203, 15213, 15222, 15231, 15240,
	15250, 15259, 15268, 15277, 15286, 15295, 15304, 15313, 15322, 15331, 15340, 15349,
	15357, 15366, 15375, 15383, 15392, 15401, 15409, 15418, 15426, 15435, 15443, 15451,
	15460, 15468, 15476, 15485, 15493, 15501, 15509, 15517, 15525, 15533, 15541, 15549,
	15557, 15565, 15573, 15581, 15588, 15596, 15604, 15611, 15619, 15627, 15634, 15642,
	15649, 15656, 15664, 15671, 15679, 15686, 15693, 15700, 15707, 15715, 15722, 15729,
	15736, 15743, 15750, 15757, 15763, 15770, 15777, 15784, 15791, 15797, 15804, 15810,
	15817, 15824, 15830, 15837, 15843, 15849, 15856, 15862, 15868, 15875, 15881, 15887,
	15893, 15899, 15905, 15911, 15917, 15923, 15929, 15935, 15941, 15946, 15952, 15958,
	15964, 15969, 15975, 15980, 15986, 15991, 15997, 16002, 16008, 16013, 16018, 16024,
	16029, 16034, 16039, 16044, 16049, 16054, 16059, 16064, 16069, 16074, 16079, 16084,
	16088, 16093, 16098, 16103, 16107, 16112, 16116, 16121, 16125, 16130, 16134, 16138,
	16143, 16147, 16151, 16156, 16160, 16164, 16168, 16172, 16176, 16180, 16184, 16188,
	16192, 16195, 16199, 16203, 16207, 16210, 16214, 16218, 16221, 16225, 16228, 16232,
	16235, 16238, 16242, 16245, 16248, 16251, 16255, 16258, 16261, 16264, 16267, 16270,
	16273, 16276, 16279, 16281, 16284, 16287, 16290, 16292, 16295, 16298, 16300, 16303,
	16305, 16308, 16310, 16312, 16315, 16317, 16319, 16321, 16324, 16326, 16328, 16330,
	16332, 16334, 16336, 16338, 16340, 16341, 16343, 16345, 16347, 16348, 16350, 16352,
	16353, 16355, 16356, 16358, 16359, 16360, 16362, 16363, 16364, 16365, 16367, 16368,
	16369, 16370, 16371, 16372, 16373, 16374, 16375, 16375, 16376, 16377, 16378, 16378,
	16379, 16380, 16380, 16381, 16381, 16382, 16382, 16382, 16383, 16383, 16383, 16384,
	16384, 16384, 16384, 16384, 16384
};

LOCAL CONST UH atan_r[1025] = {
	0, 1, 1, 2, 3, 3, 4, 4, 5, 6, 6, 7,
	8, 8, 9, 10, 10, 11, 11, 12, 13, 13, 14, 15,
	15, 16, 17, 17, 18, 18, 19, 20, 20, 21, 22, 22,
	23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29, 30,
	31, 31, 32, 32, 33, 34, 34, 35, 36, 36, 37, 38,
	38, 39, 39, 40, 41, 41, 42, 43, 43, 44, 44, 45,
	46, 46, 47, 48, 48, 49, 50, 50, 51, 51, 52, 53,
	53, 54, 55, 55, 56, 57, 57, 58, 58, 59, 60, 60,
	61, 62, 62, 63, 63, 64, 65, 65, 66, 67, 67, 68,
	69, 69, 70, 70, 71, 72, 72, 73, 74, 74, 75, 75,
	76, 77, 77, 78, 79, 79, 80, 80, 81, 82, 82, 83,
	84, 84, 85, 85, 86, 87, 87, 88, 89, 89, 90, 90,
	91, 92, 92, 93, 94, 94, 95, 95, 96, 97, 97, 98,
	99, 99, 100, 100, 101, 102, 102, 103, 104, 104, 105, 105,
	106, 107, 107, 108, 108, 109, 110, 110, 111, 112, 112, 113,
	113, 114, 115, 115, 116, 117, 117, 118, 118, 119, 120, 120,
	121, 121, 122, 123, 123, 124, 125, 125, 126, 126, 127, 128,
	128, 129, 129, 130, 131, 131, 132, 132, 133, 134, 134, 135,
	136, 136, 137, 137, 138, 139, 139, 140, 140, 141, 142, 142,
	143, 143, 144, 145, 145, 146, 146, 147, 148, 148, 149, 149,
	150, 151, 151, 152, 152, 153, 154, 154, 155, 156, 156, 157,
	157, 158, 159, 159, 160, 160, 161, 161, 162, 163, 163, 164,
	164, 165, 166, 166, 167, 167, 168, 169, 169, 170, 170, 171,
	172, 172, 173, 173, 174, 175, 175, 176, 176, 177, 178, 178,
	179, 179, 180, 180, 181, 182, 182, 183, 183, 184, 185, 185,
	186, 186, 187, 188, 188, 189, 189, 190, 190, 191, 192, 192,
	193, 193, 194, 195, 195, 196, 196, 197, 197, 198, 199, 199,
	200, 200, 201, 202, 202, 203, 203, 204, 204, 205, 206, 206,
	207, 207, 208, 208, 209, 210, 210, 211, 211, 212, 212, 213,
	214, 214, 215, 215, 216, 216, 217, 218, 218, 219, 219, 220,
	220, 221, 222, 222, 223, 223, 224, 224, 225, 225, 226, 227,
	227, 228, 228, 229, 229, 230, 231, 231, 232, 232, 233, 233,
	234, 234, 235, 236, 236, 237, 237, 238, 238, 239, 239, 240,
	241, 241, 242, 242, 243, 243, 244, 244, 245, 246, 246, 247,
	247, 248, 248, 249, 249, 250, 250, 251, 252, 252, 253, 253,
	254, 254, 255, 255, 256, 256, 257, 258, 258, 259, 259, 260,
	260, 261, 261, 262, 262, 263, 263, 264, 265, 265, 266, 266,
	267, 267, 268, 268, 269, 269, 270, 270, 271, 272, 272, 273,
	273, 274, 274, 275, 275, 276, 276, 277, 277, 278, 278, 279,
	279, 280, 281, 281, 282, 282, 283, 283, 284, 284, 285, 285,
	286, 286, 287, 287, 288, 288, 289, 289, 290, 290, 291, 291,
	292, 293, 293, 294, 294, 295, 295, 296, 296, 297, 297, 298,
	298, 299, 299, 300, 300, 301, 301, 302, 302, 303, 303, 304,
	304, 305, 305, 306, 306, 307, 307, 308, 308, 309, 309, 310,
	310, 311, 311, 312, 312, 313, 313, 314, 314, 315, 315, 316,
	316, 317, 317, 318, 318, 319, 319, 320, 320, 321, 321, 322,
	322, 323, 323, 324, 324, 325, 325, 326, 326, 327, 327, 328,
	328, 329, 329, 330, 330, 331, 331, 332, 332, 333, 333, 334,
	334, 335, 335, 335, 336, 336, 337, 337, 338, 338, 339, 339,
	340, 340, 341, 341, 342, 342, 343, 343, 344, 344, 345, 345,
	346, 346, 346, 347, 347, 348, 348, 349, 349, 350, 350, 351,
	351, 352, 352, 353, 353, 354, 354, 354, 355, 355, 356, 356,
	357, 357, 358, 358, 359, 359, 360, 360, 360, 361, 361, 362,
	362, 363, 363, 364, 364, 365, 365, 366, 366, 366, 367, 367,
	368, 368, 369, 369, 370, 370, 371, 371, 371, 372, 372, 373,
	373, 374, 374, 375, 375, 375, 376, 376, 377, 377, 378, 378,
	379, 379, 379, 380, 380, 381, 381, 382, 382, 383, 383, 383,
	384, 384, 385, 385, 386, 386, 387, 387, 387, 388, 388, 389,
	389, 390, 390, 390, 391, 391, 392, 392, 393, 393, 393, 394,
	394, 395, 395, 396, 396, 397, 397, 397, 398, 398, 399, 399,
	399, 400, 400, 401, 401, 402, 402, 402, 403, 403, 404, 404,
	405, 405, 405, 406, 406, 407, 407, 408, 408, 408, 409, 409,
	410, 410, 410, 411, 411, 412, 412, 413, 413, 413, 414, 414,
	415, 415, 415, 416, 416, 417, 417, 417, 418, 418, 419, 419,
	419, 420, 420, 421, 421, 422, 422, 422, 423, 423, 424, 424,
	424, 425, 425, 426, 426, 426, 427, 427, 428, 428, 428, 429,
	429, 430, 430, 430, 431, 431, 432, 432, 432, 433, 433, 434,
	434, 434, 435, 435, 435, 436, 436, 437, 437, 437, 438, 438,
	439, 439, 439, 440, 440, 441, 441, 441, 442, 442, 442, 443,
	443, 444, 444, 444, 445, 445, 446, 446, 446, 447, 447, 447,
	448, 448, 449, 449, 449, 450, 450, 451, 451, 451, 452, 452,
	452, 453, 453, 454, 454, 454, 455, 455, 455, 456, 456, 457,
	457, 457, 458, 458, 458, 459, 459, 459, 460, 460, 461, 461,
	461, 462, 462, 462, 463, 463, 464, 464, 464, 465, 465, 465,
	466, 466, 466, 467, 467, 468, 468, 468, 469, 469, 469, 470,
	470, 470, 471, 471, 471, 472, 472, 473, 473, 473, 474, 474,
	474, 475, 475, 475, 476, 476, 476, 477, 477, 478, 478, 478,
	479, 479, 479, 480, 480, 480, 481, 481, 481, 482, 482, 482,
	483, 483, 483, 484, 484, 484, 485, 485, 486, 486, 486, 487,
	487, 487, 488, 488, 488, 489, 489, 489, 490, 490, 490, 491,
	491, 491, 492, 492, 492, 493, 493, 493, 494, 494, 494, 495,
	495, 495, 496, 496, 496, 497, 497, 497, 498, 498, 498, 499,
	499, 499, 500, 500, 500, 501, 501, 501, 502, 502, 502, 503,
	503, 503, 504, 504, 504, 505, 505, 505, 506, 506, 506, 507,
	507, 507, 508, 508, 508, 508, 509, 509, 509, 510, 510, 510,
	511, 511, 511, 512, 512
};

/* ---------------------------------------------------------------- numbers */

EXPORT INT tv_sin( INT a )
{
	a &= 4095;
	if ( a < 1024 ) return sin_q[a];
	if ( a < 2048 ) return sin_q[2048 - a];
	if ( a < 3072 ) return -(INT)sin_q[a - 2048];

	return -(INT)sin_q[4096 - a];
}

EXPORT INT tv_cos( INT a )
{
	return tv_sin(a + 1024);
}

/* The angle of (dx, dy), screen way round: y runs down, so a turn from x to y */
EXPORT INT tv_atan2( D dy, D dx )
{
	D	ax = ( dx < 0 ) ? -dx : dx, ay = ( dy < 0 ) ? -dy : dy;
	INT	a;

	if ( ax == 0 && ay == 0 ) {
		return 0;
	}
	if ( ay <= ax ) {
		a = atan_r[(INT)( ay * 1024 / ax )];
	} else {
		a = 1024 - atan_r[(INT)( ax * 1024 / ay )];
	}
	if ( dx < 0 ) a = 2048 - a;
	if ( dy < 0 ) a = 4096 - a;

	return a & 4095;
}

EXPORT D tv_isqrt( D v )
{
	D	r = 0, b = (D)1 << 60;

	if ( v <= 0 ) {
		return 0;
	}
	while ( b > v ) {
		b >>= 2;
	}
	while ( b != 0 ) {
		if ( v >= r + b ) {
			v -= r + b;
			r = ( r >> 1 ) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}

	return r;
}

/* ---------------------------------------------------------------- outlines */

/*
 * A point of an ellipse centred at (cx, cy) -- in 16ths of a pixel --
 * with radii rx, ry, at parameter a, the whole turned by rot (4096ths),
 * in whole pixels.
 */
LOCAL void ell_pt( D cx, D cy, D rx, D ry, INT a, INT rot, T_DPPOINT *p )
{
	D	x = rx * tv_cos(a) / 16384, y = ry * tv_sin(a) / 16384;
	D	c = tv_cos(rot), s = tv_sin(rot);
	D	X = cx + ( x * c - y * s ) / 16384;
	D	Y = cy + ( x * s + y * c ) / 16384;

	p->x = (INT)( ( X + 8 ) >> 4 );
	p->y = (INT)( ( Y + 8 ) >> 4 );
}

/* How many points a piece of an ellipse of these radii needs, per turn */
LOCAL INT ell_steps( D rx, D ry )
{
	D	r = ( rx > ry ) ? rx : ry;
	INT	n = (INT)( r / 16 );		/* about one point every six pixels */

	if ( n < 16 )  n = 16;
	if ( n > 256 ) n = 256;

	return n;
}

EXPORT INT tv_arc_points( T_DPPOINT *out, INT max, D cx, D cy, D rx, D ry,
			  INT a0, INT a1, INT rot )
{
	INT	n, i, span, steps;

	span = ( a1 - a0 ) & 4095;
	if ( a1 != a0 && span == 0 ) {
		span = 4096;
	}
	if ( a0 == 0 && a1 == 4096 ) {
		span = 4096;
	}
	steps = ell_steps(rx, ry) * span / 4096;
	if ( steps < 2 ) steps = 2;
	if ( steps > max - 1 ) steps = max - 1;
	n = 0;
	for ( i = 0; i <= steps && n < max; i++ ) {
		ell_pt(cx, cy, rx, ry, a0 + span * i / steps, rot, &out[n++]);
	}

	return n;
}

/* The points turned by rot (4096ths) about (cx, cy), in 16ths of a pixel */
EXPORT void tv_turn_points( T_DPPOINT *p, INT n, D cx, D cy, INT rot )
{
	D	c = tv_cos(rot), s = tv_sin(rot);
	INT	i;

	if ( ( rot & 4095 ) == 0 ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		D	x = (D)p[i].x * 16 - cx, y = (D)p[i].y * 16 - cy;
		D	X = cx + ( x * c - y * s ) / 16384;
		D	Y = cy + ( x * s + y * c ) / 16384;

		p[i].x = (INT)( ( X + 8 ) >> 4 );
		p[i].y = (INT)( ( Y + 8 ) >> 4 );
	}
}

/* A rectangle with corners of radius rh across and rv down */
LOCAL INT round_rect( T_DPPOINT *out, INT max, CONST T_DPRECT *r, INT rh, INT rv )
{
	D	l = (D)r->left * 16, t = (D)r->top * 16;
	D	R = (D)r->right * 16, B = (D)r->bottom * 16;
	D	h = (D)rh * 16, v = (D)rv * 16;
	INT	n = 0, q = max / 4;

	if ( h > ( R - l ) / 2 ) h = ( R - l ) / 2;
	if ( v > ( B - t ) / 2 ) v = ( B - t ) / 2;
	if ( h <= 0 || v <= 0 ) {
		if ( max < 4 ) {
			return 0;
		}
		out[0].x = r->left;   out[0].y = r->top;
		out[1].x = r->right;  out[1].y = r->top;
		out[2].x = r->right;  out[2].y = r->bottom;
		out[3].x = r->left;   out[3].y = r->bottom;
		return 4;
	}
	n += tv_arc_points(out + n, q, R - h, t + v, h, v, 3072, 4096, 0);
	n += tv_arc_points(out + n, q, R - h, B - v, h, v, 0, 1024, 0);
	n += tv_arc_points(out + n, q, l + h, B - v, h, v, 1024, 2048, 0);
	n += tv_arc_points(out + n, q, l + h, t + v, h, v, 2048, 3072, 0);

	return n;
}

/*
 * A curve through points, the way it is drawn by hand: a quadratic
 * piece from the middle of each segment to the middle of the next,
 * bending at the point between.
 */
LOCAL INT quad_to( T_DPPOINT *out, INT n, INT max, D x0, D y0, D cx, D cy,
		   D x1, D y1 )
{
	INT	i;

	for ( i = 1; i <= 8 && n < max; i++ ) {
		D	t = i, u = 8 - i;
		D	x = ( u * u * x0 + 2 * u * t * cx + t * t * x1 ) / 64;
		D	y = ( u * u * y0 + 2 * u * t * cy + t * t * y1 ) / 64;

		out[n].x = (INT)x;
		out[n].y = (INT)y;
		n++;
	}

	return n;
}

EXPORT INT tv_curve_points( T_DPPOINT *out, INT max, CONST T_DPPOINT *p, INT np )
{
	INT	i, n = 0;

	if ( np <= 0 || max <= 0 ) {
		return 0;
	}
	out[n++] = p[0];
	if ( np <= 2 ) {
		if ( np == 2 && n < max ) {
			out[n++] = p[1];
		}
		return n;
	}
	{
		D	x = p[0].x, y = p[0].y;

		for ( i = 1; i < np - 2; i++ ) {
			D	xc = ( (D)p[i].x + p[i + 1].x ) / 2;
			D	yc = ( (D)p[i].y + p[i + 1].y ) / 2;

			n = quad_to(out, n, max, x, y, p[i].x, p[i].y, xc, yc);
			x = xc;
			y = yc;
		}
		n = quad_to(out, n, max, x, y, p[np - 2].x, p[np - 2].y,
			    p[np - 1].x, p[np - 1].y);
	}

	return n;
}

/* A cubic from (x0, y0) to (x3, y3) pulled by two points between */
EXPORT INT tv_bezier_points( T_DPPOINT *out, INT max, D x0, D y0, D x1, D y1,
			     D x2, D y2, D x3, D y3 )
{
	INT	i, n = 0, steps = 24;

	for ( i = 0; i <= steps && n < max; i++ ) {
		D	t = i, u = steps - i, s3 = (D)steps * steps * steps;

		out[n].x = (INT)( ( u * u * u * x0 + 3 * u * u * t * x1
				  + 3 * u * t * t * x2 + t * t * t * x3 ) / s3 );
		out[n].y = (INT)( ( u * u * u * y0 + 3 * u * u * t * y1
				  + 3 * u * t * t * y2 + t * t * t * y3 ) / s3 );
		n++;
	}

	return n;
}

/*
 * The outline of a shape, in the figure's pixels: the points, and
 * whether the last joins back to the first. 0 points for a shape that
 * is not drawn as an outline (a virtual object, a picture, a text).
 */
EXPORT INT tv_shape_outline( CONST T_TVSHAPE *s, T_DPPOINT *out, INT max,
			     BOOL *p_closed )
{
	INT	n = 0;
	D	cx, cy, rx, ry;

	*p_closed = TRUE;
	switch ( s->kind ) {
	case TV_SH_RECT:
		n = round_rect(out, max, &s->r, s->rad_h, s->rad_v);
		break;
	case TV_SH_ELLIPSE:
	case TV_SH_ARC:
	case TV_SH_CHORD:
	case TV_SH_EARC:
		cx = ( (D)s->r.left + s->r.right ) * 8;
		cy = ( (D)s->r.top + s->r.bottom ) * 8;
		rx = ( (D)s->r.right - s->r.left ) * 8;
		ry = ( (D)s->r.bottom - s->r.top ) * 8;
		if ( s->kind == TV_SH_ELLIPSE ) {
			n = tv_arc_points(out, max, cx, cy, rx, ry, 0, 4096,
					  s->angle);
			if ( n > 1 ) n--;	/* the last is the first again */
			break;
		}
		if ( s->kind == TV_SH_ARC && max > 2 ) {
			/* a sector: the middle, then round the arc */
			T_DPPOINT	c;

			c.x = (INT)( ( cx + 8 ) >> 4 );
			c.y = (INT)( ( cy + 8 ) >> 4 );
			out[0] = c;
			n = 1 + tv_arc_points(out + 1, max - 1, cx, cy, rx, ry,
					      s->a0, s->a1, s->angle);
			break;
		}
		n = tv_arc_points(out, max, cx, cy, rx, ry, s->a0, s->a1, s->angle);
		*p_closed = (BOOL)( s->kind == TV_SH_CHORD );
		break;
	case TV_SH_POLY:
	case TV_SH_LINE:
	case TV_SH_CURVE:
		if ( s->kind == TV_SH_CURVE ) {
			n = tv_curve_points(out, max, s->pt, s->npt);
			*p_closed = s->closed;
		} else {
			for ( n = 0; n < s->npt && n < max; n++ ) {
				out[n] = s->pt[n];
			}
			*p_closed = (BOOL)( s->kind == TV_SH_POLY );
		}
		break;
	default:
		return 0;
	}
	if ( s->rot != 0 && s->kind != TV_SH_ELLIPSE && s->kind != TV_SH_ARC
	  && s->kind != TV_SH_CHORD && s->kind != TV_SH_EARC ) {
		/* turned about the middle of its box */
		tv_turn_points(out, n, ( (D)s->r.left + s->r.right ) * 8,
			       ( (D)s->r.top + s->r.bottom ) * 8, s->rot * 4096 / 360);
	}

	return n;
}

/* ---------------------------------------------------------------- stroking */

/* The dashes of each kind of line: lengths on, off, on, off ... ; 0 ends */
LOCAL CONST UB	dash_of[6][8] = {
	{ 0 },				/* 実線 */
	{ 6, 3, 0 },			/* 破線 */
	{ 2, 2, 0 },			/* 点線 */
	{ 8, 3, 2, 3, 0 },		/* 一点鎖線 */
	{ 8, 3, 2, 3, 2, 3, 0 },	/* 二点鎖線 */
	{ 12, 4, 0 }			/* 長破線 */
};

/*
 * An outline drawn as a line 'w' wide of kind 'type', the dashes
 * carried on from one segment to the next so that a curve made of many
 * short pieces is dashed as a whole. The lengths of the dashes grow
 * with the width, as a thicker pen's do.
 */
EXPORT void tv_stroke( INT gid, CONST T_DPPOINT *p, INT n, BOOL closed, INT w,
		       UINT type, CONST T_DPPAT *pat )
{
	tv_stroke_dash(gid, p, n, closed, w, dash_of[( type < 6 ) ? type : 0], pat);
}

EXPORT void tv_stroke_dash( INT gid, CONST T_DPPOINT *p, INT n, BOOL closed, INT w,
			    CONST UB *dash, CONST T_DPPAT *pat )
{
	INT		i, seg = 0, left, scale = ( w > 1 ) ? w : 1;
	BOOL		on = TRUE;

	if ( n < 2 || w <= 0 ) {
		return;
	}
	if ( dash[0] == 0 ) {
		dp_draw_poly(gid, p, n, closed, w, DP_LINE_SOLID, pat);
		return;
	}
	left = dash[0] * scale * 16;
	for ( i = 0; i < n - 1 + ( closed ? 1 : 0 ); i++ ) {
		CONST T_DPPOINT	*a = &p[i], *b = &p[( i + 1 ) % n];
		D		dx = (D)( b->x - a->x ) * 16, dy = (D)( b->y - a->y ) * 16;
		D		len = tv_isqrt(dx * dx + dy * dy), done = 0;

		while ( done < len ) {
			D	step = len - done;

			if ( step > left ) {
				step = left;
			}
			if ( on ) {
				INT	x0 = a->x + (INT)( dx * done / len / 16 );
				INT	y0 = a->y + (INT)( dy * done / len / 16 );
				INT	x1 = a->x + (INT)( dx * ( done + step ) / len / 16 );
				INT	y1 = a->y + (INT)( dy * ( done + step ) / len / 16 );

				dp_line_wide(gid, x0, y0, x1, y1, w, DP_LINE_SOLID, pat);
			}
			done += step;
			left -= (INT)step;
			if ( left <= 0 ) {
				seg++;
				if ( dash[seg] == 0 ) {
					seg = 0;
				}
				on = (BOOL)( ( seg & 1 ) == 0 );
				left = dash[seg] * scale * 16;
			}
		}
	}
}

/* ---------------------------------------------------------------- joined lines */

/* The shape at a place among the figure's elements, or NULL */
EXPORT CONST T_TVSHAPE *tv_fig_place( CONST T_TVFIG *f, INT place )
{
	INT	i;

	for ( i = 0; place >= 0 && i < f->nsh; i++ ) {
		if ( f->sh[i].place == place && f->sh[i].kind != TV_SH_GROUP ) {
			return &f->sh[i];
		}
	}

	return NULL;
}

/*
 * One of the points a line may be joined to on a shape, and which way
 * the shape faces there (in 16384ths). A box has eight: the middle of
 * the top, then round the way a clock goes, corners and middles in
 * turn; an ellipse eight, every eighth of a turn from the right; a
 * polygon its corners, then the middles of its sides.
 */
EXPORT BOOL tv_connector( CONST T_TVSHAPE *s, INT k, T_DPPOINT *p,
			  T_DPPOINT *dir )
{
	LOCAL CONST INT	bx[8] = { 1, 2, 2, 2, 1, 0, 0, 0 };
	LOCAL CONST INT	by[8] = { 0, 0, 1, 2, 2, 2, 1, 0 };
	INT		l = s->r.left, t = s->r.top, r = s->r.right, b = s->r.bottom;

	switch ( s->kind ) {
	case TV_SH_ELLIPSE: {
		D	cx = ( (D)l + r ) * 8, cy = ( (D)t + b ) * 8;
		D	rx = ( (D)r - l ) * 8, ry = ( (D)b - t ) * 8;
		T_DPPOINT	q[2];

		if ( k < 0 || k > 7 ) {
			return FALSE;
		}
		(void)tv_arc_points(q, 2, cx, cy, rx, ry, k * 512, k * 512, s->angle);
		*p = q[0];
		dir->x = tv_cos(k * 512);
		dir->y = tv_sin(k * 512);
		return TRUE;
	}
	case TV_SH_POLY: {
		INT	n = s->npt;

		if ( n < 2 || k < 0 || k >= 2 * n ) {
			return FALSE;
		}
		if ( k < n ) {
			*p = s->pt[k];
		} else {
			p->x = ( s->pt[k - n].x + s->pt[( k - n + 1 ) % n].x ) / 2;
			p->y = ( s->pt[k - n].y + s->pt[( k - n + 1 ) % n].y ) / 2;
		}
		/* facing out from the middle of the box */
		dir->x = p->x - ( l + r ) / 2;
		dir->y = p->y - ( t + b ) / 2;
		return TRUE;
	}
	default:
		if ( k < 0 || k > 7 ) {
			return FALSE;
		}
		p->x = ( bx[k] == 0 ) ? l : ( bx[k] == 2 ) ? r : ( l + r ) / 2;
		p->y = ( by[k] == 0 ) ? t : ( by[k] == 2 ) ? b : ( t + b ) / 2;
		dir->x = ( bx[k] - 1 ) * 16384;
		dir->y = ( by[k] - 1 ) * 16384;
		return TRUE;
	}
}

/*
 * The path of a line: its points, with each end joined to a shape put
 * where that shape's point now is; and with both ends joined, bent at
 * right angles or curved away from each shape, as the line says.
 */
EXPORT INT tv_line_path( CONST T_TVFIG *f, CONST T_TVSHAPE *s, T_DPPOINT *out,
			 INT max )
{
	CONST T_TVSHAPE	*a = NULL, *b = NULL;
	T_DPPOINT	pa, pb, da, db;
	INT		n, k;
	BOOL		ja = FALSE, jb = FALSE;

	if ( s->npt < 2 || max < 2 ) {
		return 0;
	}
	for ( n = 0; n < s->npt && n < max; n++ ) {
		out[n] = s->pt[n];
	}
	if ( s->kind != TV_SH_LINE ) {
		return n;
	}
	if ( s->c0_shape >= 0 ) {
		a = tv_fig_place(f, s->c0_shape);
		ja = (BOOL)( a != NULL && a != s
			  && tv_connector(a, s->c0_point, &pa, &da) );
	}
	if ( s->c1_shape >= 0 ) {
		b = tv_fig_place(f, s->c1_shape);
		jb = (BOOL)( b != NULL && b != s
			  && tv_connector(b, s->c1_point, &pb, &db) );
	}
	if ( ja ) out[0] = pa;
	if ( jb ) out[n - 1] = pb;
	if ( !ja || !jb || s->conn == TV_CONN_STRAIGHT || n != 2 ) {
		return n;
	}
	if ( s->conn == TV_CONN_ELBOW ) {
		/* out along the first shape's side, across, and in */
		INT	mx = ( pa.x + pb.x ) / 2, my = ( pa.y + pb.y ) / 2;
		BOOL	up = (BOOL)( ( da.y < 0 ? -da.y : da.y ) > ( da.x < 0 ? -da.x : da.x ) );

		if ( max < 4 ) {
			return n;
		}
		out[0] = pa;
		if ( up ) {
			out[1].x = pa.x;  out[1].y = my;
			out[2].x = pb.x;  out[2].y = my;
		} else {
			out[1].x = mx;  out[1].y = pa.y;
			out[2].x = mx;  out[2].y = pb.y;
		}
		out[3] = pb;
		return 4;
	}
	/* curved: pulled out from each shape a quarter of the way along */
	{
		D	dx = (D)pb.x - pa.x, dy = (D)pb.y - pa.y;
		D	dist = tv_isqrt(dx * dx + dy * dy) / 4;
		D	la = tv_isqrt((D)da.x * da.x + (D)da.y * da.y);
		D	lb = tv_isqrt((D)db.x * db.x + (D)db.y * db.y);
		D	c1x = pa.x, c1y = pa.y, c2x = pb.x, c2y = pb.y;

		if ( la > 0 ) {
			c1x += da.x * dist / la;
			c1y += da.y * dist / la;
		}
		if ( lb > 0 ) {
			c2x += db.x * dist / lb;
			c2y += db.y * dist / lb;
		}
		k = tv_bezier_points(out, max, pa.x, pa.y, c1x, c1y, c2x, c2y,
				     pb.x, pb.y);
	}

	return k;
}
