/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	face.h
 *	Faces (書体) as objects, for process programs (docs/tessronos/10-display.md 10.3).
 *
 *	The 書体箱 (SYSDEF_FONT_BOX) links to one object a face. The
 *	object's record 1 is the font file (OpenType or TrueType), and its
 *	metadata names the file (tessronos.file.name). A program that draws
 *	letters itself (with its own rasteriser) finds the face here and
 *	reads record 1 with ob_opn_obj and ob_rea_rec.
 */

#ifndef __TS_FACE_H__
#define __TS_FACE_H__

#include <tk/typedef.h>
#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The face whose object name or file name is 'name', without regard to
 * case and with or without the file name's extension
 * ("NotoSansJP-Regular", "NotoSansJP-Regular.otf"). With 'name' NULL
 * or "", the first face in the box. E_NOEXS when there is none.
 */
IMPORT ER ts_face_find( CONST char *name, TS_UUID *p_uuid );

/* The faces in the box, as many as fit in 'buf'; answers how many there are */
IMPORT INT ts_face_list( TS_UUID *buf, INT max );

#ifdef __cplusplus
}
#endif

#endif /* __TS_FACE_H__ */
