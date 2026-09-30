/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ftmodule.h
 *	Which parts of FreeType are built in (design 16.6)
 *
 *	What is registered here decides what the font layer can open and
 *	how large it is. Two outline formats are wanted: the glyf outlines
 *	of TrueType and the PostScript outlines of OpenType/CFF, because
 *	fonts of both kinds are in ordinary use. Two renderers are wanted:
 *	the smooth one for grey coverage and the monochrome one, which is
 *	what the screen driver can put down today (there is no blending in
 *	the drawing layer yet).
 *
 *	The auto-hinter is left out. It is large, and the front end never
 *	asks for it.
 */

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )

/* OpenType with PostScript outlines: the driver and what it leans on */
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
