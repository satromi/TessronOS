/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tadlnk.h
 *
 *	The links of a record made to say on which volume their objects
 *	are (design 8.3): an object of the volume of the record that holds
 *	the link by its UUID alone, one of another volume with that
 *	volume's UUID before it; one on no volume attached, or named on
 *	another machine, as it is. The desktop does it when it saves; a
 *	program that writes its records itself does it before writing,
 *	learning the volumes with ob_ref_vid. The same source serves both:
 *	how a volume is learnt is given (vol_of), and what is written goes
 *	into room the caller gives.
 */

#ifndef __TS_TADLNK_H__
#define __TS_TADLNK_H__

#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The UUID of the volume an object is on, into *p_vol: E_OK, or an error when it is on none */
typedef ER (*TAD_VOLOF)( CONST TS_UUID *obj, TS_UUID *p_vol, void *arg );

/*
 * An xmlTAD text: the room the fixed text may take at most, with its
 * terminator (each link may gain a volume's UUID and its slash); and the
 * text written fixed into out, which has that much room. hv is the
 * volume of the object the text is a record of, mine this machine's
 * UUID (NULL: none known, a link that names a machine is left as it
 * is). Answers the length written (out ends in a 0 after it), and in
 * *p_changed how many links changed.
 */
IMPORT SZ  tad_lnk_vol_room( CONST UB *in, SZ len );
IMPORT SZ  tad_lnk_vol_fix( CONST UB *in, SZ len, UB *out, CONST TS_UUID *hv, CONST TS_UUID *mine,
			    TAD_VOLOF vol_of, void *arg, INT *p_changed );

/*
 * The same for a parsed record (ts/tad.h), each link changed in place
 * with tad_lnk_set. Answers how many changed; a link that could not be
 * changed is passed over and counted in *p_failed (may be NULL).
 */
struct t_tad;
IMPORT INT tad_lnk_vol_doc( struct t_tad *doc, CONST TS_UUID *hv, CONST TS_UUID *mine,
			    TAD_VOLOF vol_of, void *arg, INT *p_failed );

#ifdef __cplusplus
}
#endif
#endif /* __TS_TADLNK_H__ */
