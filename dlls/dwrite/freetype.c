/*
 *    FreeType integration
 *
 * Copyright 2014-2017 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <dlfcn.h>

#ifdef HAVE_FT2BUILD_H
#include <ft2build.h>
#include FT_GLYPH_H
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_SIZES_H
#include FT_MULTIPLE_MASTERS_H
#endif /* HAVE_FT2BUILD_H */

#include "ntstatus.h"
#include "windef.h"
#include "wine/debug.h"
#include "unixlib.h"

#include "dwrite_private.h"

#ifdef SONAME_LIBFREETYPE

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

#ifdef _WIN64
static void wow64_font_registry_attach(void);
static void wow64_font_registry_cleanup(void);
#endif

static void *ft_handle = NULL;
static FT_Library library = 0;
typedef struct
{
    FT_Int major;
    FT_Int minor;
    FT_Int patch;
} FT_Version_t;

#define MAKE_FUNCPTR(f) static typeof(f) * p##f = NULL
MAKE_FUNCPTR(FT_Activate_Size);
MAKE_FUNCPTR(FT_Done_Face);
MAKE_FUNCPTR(FT_Done_FreeType);
MAKE_FUNCPTR(FT_Done_Glyph);
MAKE_FUNCPTR(FT_Done_Size);
MAKE_FUNCPTR(FT_Get_First_Char);
MAKE_FUNCPTR(FT_Get_Glyph);
MAKE_FUNCPTR(FT_Get_Kerning);
MAKE_FUNCPTR(FT_Get_Sfnt_Table);
MAKE_FUNCPTR(FT_Glyph_Copy);
MAKE_FUNCPTR(FT_Glyph_Get_CBox);
MAKE_FUNCPTR(FT_Glyph_Transform);
MAKE_FUNCPTR(FT_Init_FreeType);
MAKE_FUNCPTR(FT_Library_Version);
MAKE_FUNCPTR(FT_Load_Glyph);
MAKE_FUNCPTR(FT_Matrix_Multiply);
MAKE_FUNCPTR(FT_MulDiv);
MAKE_FUNCPTR(FT_New_Memory_Face);
MAKE_FUNCPTR(FT_New_Size);
MAKE_FUNCPTR(FT_Outline_Copy);
MAKE_FUNCPTR(FT_Outline_Decompose);
MAKE_FUNCPTR(FT_Outline_Done);
MAKE_FUNCPTR(FT_Outline_EmboldenXY);
MAKE_FUNCPTR(FT_Outline_Get_Bitmap);
MAKE_FUNCPTR(FT_Outline_New);
MAKE_FUNCPTR(FT_Outline_Transform);
MAKE_FUNCPTR(FT_Outline_Translate);
MAKE_FUNCPTR(FT_Set_Pixel_Sizes);
MAKE_FUNCPTR(FT_Set_Var_Design_Coordinates);
#undef MAKE_FUNCPTR

#define FaceFromObject(o) ((FT_Face)(ULONG_PTR)(o))

static FT_Size freetype_set_face_size(FT_Face face, FT_UInt emsize)
{
    FT_Size size;

    if (pFT_New_Size(face, &size)) return NULL;

    pFT_Activate_Size(size);

    if (pFT_Set_Pixel_Sizes(face, emsize, emsize))
    {
        pFT_Done_Size(size);
        return NULL;
    }

    return size;
}

static BOOL freetype_glyph_has_contours(FT_Face face)
{
    return face->glyph->format == FT_GLYPH_FORMAT_OUTLINE && face->glyph->outline.n_contours;
}

static NTSTATUS process_attach(void *args)
{
    FT_Version_t FT_Version;

    ft_handle = dlopen(SONAME_LIBFREETYPE, RTLD_NOW);
    if (!ft_handle)
    {
        WINE_MESSAGE("Wine cannot find the FreeType font library.\n");
        return STATUS_DLL_NOT_FOUND;
    }

#define LOAD_FUNCPTR(f) if((p##f = dlsym(ft_handle, #f)) == NULL){WARN("Can't find symbol %s\n", #f); goto sym_not_found;}
    LOAD_FUNCPTR(FT_Activate_Size)
    LOAD_FUNCPTR(FT_Done_Face)
    LOAD_FUNCPTR(FT_Done_FreeType)
    LOAD_FUNCPTR(FT_Done_Glyph)
    LOAD_FUNCPTR(FT_Done_Size)
    LOAD_FUNCPTR(FT_Get_First_Char)
    LOAD_FUNCPTR(FT_Get_Glyph)
    LOAD_FUNCPTR(FT_Get_Kerning)
    LOAD_FUNCPTR(FT_Get_Sfnt_Table)
    LOAD_FUNCPTR(FT_Glyph_Copy)
    LOAD_FUNCPTR(FT_Glyph_Get_CBox)
    LOAD_FUNCPTR(FT_Glyph_Transform)
    LOAD_FUNCPTR(FT_Init_FreeType)
    LOAD_FUNCPTR(FT_Library_Version)
    LOAD_FUNCPTR(FT_Load_Glyph)
    LOAD_FUNCPTR(FT_Matrix_Multiply)
    LOAD_FUNCPTR(FT_MulDiv)
    LOAD_FUNCPTR(FT_New_Memory_Face)
    LOAD_FUNCPTR(FT_New_Size)
    LOAD_FUNCPTR(FT_Outline_Copy)
    LOAD_FUNCPTR(FT_Outline_Decompose)
    LOAD_FUNCPTR(FT_Outline_Done)
    LOAD_FUNCPTR(FT_Outline_EmboldenXY)
    LOAD_FUNCPTR(FT_Outline_Get_Bitmap)
    LOAD_FUNCPTR(FT_Outline_New)
    LOAD_FUNCPTR(FT_Outline_Transform)
    LOAD_FUNCPTR(FT_Outline_Translate)
    LOAD_FUNCPTR(FT_Set_Pixel_Sizes)
#undef LOAD_FUNCPTR

    pFT_Set_Var_Design_Coordinates = dlsym(ft_handle, "FT_Set_Var_Design_Coordinates");

    if (pFT_Init_FreeType(&library) != 0)
    {
        ERR("Can't init FreeType library\n");
        dlclose(ft_handle);
        ft_handle = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    pFT_Library_Version(library, &FT_Version.major, &FT_Version.minor, &FT_Version.patch);

#ifdef _WIN64
    wow64_font_registry_attach();
#endif
    TRACE("FreeType version is %d.%d.%d\n", FT_Version.major, FT_Version.minor, FT_Version.patch);
    return STATUS_SUCCESS;

sym_not_found:
    WINE_MESSAGE("Wine cannot find certain functions that it needs from FreeType library.\n");
    dlclose(ft_handle);
    ft_handle = NULL;
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS process_detach(void *args)
{
#ifdef _WIN64
    wow64_font_registry_cleanup();
#endif
    pFT_Done_FreeType(library);
    return STATUS_SUCCESS;
}

static NTSTATUS create_font_object(void *args)
{
    struct create_font_object_params *params = args;
    FT_Fixed *coordinates = NULL;
    FT_Face face = NULL;
    FT_Error fterror;
    unsigned int i;

    *params->object = 0;

    fterror = pFT_New_Memory_Face(library, params->data, params->size, params->index, &face);
    if (fterror != FT_Err_Ok)
    {
        WARN("Failed to create a face object, error %d.\n", fterror);
        return STATUS_UNSUCCESSFUL;
    }

    if (params->axis_values_count)
    {
        if (!pFT_Set_Var_Design_Coordinates)
        {
            if (!params->axis_values_are_default)
            {
                WARN("FreeType does not support variable font coordinates.\n");
                pFT_Done_Face(face);
                return STATUS_NOT_SUPPORTED;
            }
        }
        else if (!(coordinates = calloc(params->axis_values_count, sizeof(*coordinates))))
        {
            pFT_Done_Face(face);
            return STATUS_NO_MEMORY;
        }
        else
        {
            for (i = 0; i < params->axis_values_count; ++i)
                coordinates[i] = params->axis_values[i].value * 65536.0f;

            fterror = pFT_Set_Var_Design_Coordinates(face, params->axis_values_count, coordinates);
            free(coordinates);
            if (fterror != FT_Err_Ok && !params->axis_values_are_default)
            {
                WARN("Failed to set variable font coordinates, error %d.\n", fterror);
                pFT_Done_Face(face);
                return STATUS_UNSUCCESSFUL;
            }
        }
    }

    *params->object = (ULONG_PTR)face;

    return STATUS_SUCCESS;
}

static NTSTATUS release_font_object(void *args)
{
    struct release_font_object_params *params = args;
    pFT_Done_Face(FaceFromObject(params->object));
    return STATUS_SUCCESS;
}

static NTSTATUS get_design_glyph_metrics(void *args)
{
    struct get_design_glyph_metrics_params *params = args;
    FT_Face face = FaceFromObject(params->object);
    FT_Size size;

    if (!(size = freetype_set_face_size(face, params->upem)))
        return STATUS_UNSUCCESSFUL;

    if (!pFT_Load_Glyph(face, params->glyph, FT_LOAD_NO_SCALE))
    {
        FT_Glyph_Metrics *metrics = &face->glyph->metrics;

        params->metrics->leftSideBearing = metrics->horiBearingX;
        params->metrics->advanceWidth = metrics->horiAdvance;
        params->metrics->rightSideBearing = metrics->horiAdvance - metrics->horiBearingX - metrics->width;

        params->metrics->advanceHeight = metrics->vertAdvance;
        params->metrics->verticalOriginY = params->ascent;
        params->metrics->topSideBearing = params->ascent - metrics->horiBearingY;
        params->metrics->bottomSideBearing = metrics->vertAdvance - metrics->height - params->metrics->topSideBearing;

        /* Adjust in case of bold simulation, glyphs without contours are ignored. */
        if (params->simulations & DWRITE_FONT_SIMULATIONS_BOLD && freetype_glyph_has_contours(face))
        {
            if (params->metrics->advanceWidth)
                params->metrics->advanceWidth += (params->upem + 49) / 50;
        }
    }

    pFT_Done_Size(size);

    return STATUS_SUCCESS;
}

struct decompose_context
{
    struct dwrite_outline *outline;
    BOOL figure_started;
    BOOL move_to;     /* last call was 'move_to' */
    FT_Vector origin; /* 'pen' position from last call */
};

static inline void ft_vector_to_d2d_point(const FT_Vector *v, D2D1_POINT_2F *p)
{
    p->x = v->x / 64.0f;
    p->y = v->y / 64.0f;
}

static int dwrite_outline_push_tag(struct dwrite_outline *outline, unsigned char tag)
{
    if (outline->tags.size < outline->tags.count + 1)
        return 1;

    outline->tags.values[outline->tags.count++] = tag;

    return 0;
}

static int dwrite_outline_push_points(struct dwrite_outline *outline, const D2D1_POINT_2F *points, unsigned int count)
{
    if (outline->points.size < outline->points.count + count)
        return 1;

    memcpy(&outline->points.values[outline->points.count], points, sizeof(*points) * count);
    outline->points.count += count;

    return 0;
}

static int decompose_beginfigure(struct decompose_context *ctxt)
{
    D2D1_POINT_2F point;
    int ret;

    if (!ctxt->move_to)
        return 0;

    ft_vector_to_d2d_point(&ctxt->origin, &point);
    if ((ret = dwrite_outline_push_tag(ctxt->outline, OUTLINE_BEGIN_FIGURE))) return ret;
    if ((ret = dwrite_outline_push_points(ctxt->outline, &point, 1))) return ret;

    ctxt->figure_started = TRUE;
    ctxt->move_to = FALSE;

    return 0;
}

static int decompose_move_to(const FT_Vector *to, void *user)
{
    struct decompose_context *ctxt = (struct decompose_context *)user;
    int ret;

    if (ctxt->figure_started)
    {
        if ((ret = dwrite_outline_push_tag(ctxt->outline, OUTLINE_END_FIGURE))) return ret;
        ctxt->figure_started = FALSE;
    }

    ctxt->move_to = TRUE;
    ctxt->origin = *to;
    return 0;
}

static int decompose_line_to(const FT_Vector *to, void *user)
{
    struct decompose_context *ctxt = (struct decompose_context *)user;
    D2D1_POINT_2F point;
    int ret;

    /* Special case for empty contours, in a way freetype returns them. */
    if (ctxt->move_to && !memcmp(to, &ctxt->origin, sizeof(*to)))
        return 0;

    ft_vector_to_d2d_point(to, &point);

    if ((ret = decompose_beginfigure(ctxt))) return ret;
    if ((ret = dwrite_outline_push_points(ctxt->outline, &point, 1))) return ret;
    if ((ret = dwrite_outline_push_tag(ctxt->outline, OUTLINE_LINE))) return ret;

    ctxt->origin = *to;
    return 0;
}

static int decompose_conic_to(const FT_Vector *control, const FT_Vector *to, void *user)
{
    struct decompose_context *ctxt = (struct decompose_context *)user;
    D2D1_POINT_2F points[3];
    FT_Vector cubic[3];
    int ret;

    if ((ret = decompose_beginfigure(ctxt)))
        return ret;

    /* convert from quadratic to cubic */

    /*
       The parametric eqn for a cubic Bezier is, from PLRM:
       r(t) = at^3 + bt^2 + ct + r0
       with the control points:
       r1 = r0 + c/3
       r2 = r1 + (c + b)/3
       r3 = r0 + c + b + a

       A quadratic Bezier has the form:
       p(t) = (1-t)^2 p0 + 2(1-t)t p1 + t^2 p2

       So equating powers of t leads to:
       r1 = 2/3 p1 + 1/3 p0
       r2 = 2/3 p1 + 1/3 p2
       and of course r0 = p0, r3 = p2
    */

    /* r1 = 1/3 p0 + 2/3 p1
       r2 = 1/3 p2 + 2/3 p1 */
    cubic[0].x = (2 * control->x + 1) / 3;
    cubic[0].y = (2 * control->y + 1) / 3;
    cubic[1] = cubic[0];
    cubic[0].x += (ctxt->origin.x + 1) / 3;
    cubic[0].y += (ctxt->origin.y + 1) / 3;
    cubic[1].x += (to->x + 1) / 3;
    cubic[1].y += (to->y + 1) / 3;
    cubic[2] = *to;

    ft_vector_to_d2d_point(cubic, points);
    ft_vector_to_d2d_point(cubic + 1, points + 1);
    ft_vector_to_d2d_point(cubic + 2, points + 2);
    if ((ret = dwrite_outline_push_points(ctxt->outline, points, 3))) return ret;
    if ((ret = dwrite_outline_push_tag(ctxt->outline, OUTLINE_BEZIER))) return ret;
    ctxt->origin = *to;
    return 0;
}

static int decompose_cubic_to(const FT_Vector *control1, const FT_Vector *control2,
    const FT_Vector *to, void *user)
{
    struct decompose_context *ctxt = (struct decompose_context *)user;
    D2D1_POINT_2F points[3];
    int ret;

    if ((ret = decompose_beginfigure(ctxt)))
        return ret;

    ft_vector_to_d2d_point(control1, points);
    ft_vector_to_d2d_point(control2, points + 1);
    ft_vector_to_d2d_point(to, points + 2);
    ctxt->origin = *to;

    if ((ret = dwrite_outline_push_points(ctxt->outline, points, 3))) return ret;
    if ((ret = dwrite_outline_push_tag(ctxt->outline, OUTLINE_BEZIER))) return ret;

    return 0;
}

static int decompose_outline(FT_Outline *ft_outline, struct dwrite_outline *outline)
{
    static const FT_Outline_Funcs decompose_funcs =
    {
        decompose_move_to,
        decompose_line_to,
        decompose_conic_to,
        decompose_cubic_to,
        0,
        0
    };
    struct decompose_context context = { 0 };
    int ret;

    context.outline = outline;

    ret = pFT_Outline_Decompose(ft_outline, &decompose_funcs, &context);

    if (!ret && context.figure_started)
        ret = dwrite_outline_push_tag(outline, OUTLINE_END_FIGURE);

    return ret;
}

static void embolden_glyph_outline(FT_Outline *outline, FLOAT emsize)
{
    FT_Pos strength;

    strength = pFT_MulDiv(emsize, 1 << 6, 24);
    pFT_Outline_EmboldenXY(outline, strength, 0);
}

static void embolden_glyph(FT_Glyph glyph, FLOAT emsize)
{
    FT_OutlineGlyph outline_glyph = (FT_OutlineGlyph)glyph;

    if (glyph->format != FT_GLYPH_FORMAT_OUTLINE)
        return;

    embolden_glyph_outline(&outline_glyph->outline, emsize);
}

static NTSTATUS get_glyph_outline(void *args)
{
    struct get_glyph_outline_params *params = args;
    FT_Face face = FaceFromObject(params->object);
    FT_Size size;

    if (!(size = freetype_set_face_size(face, params->emsize)))
        return STATUS_UNSUCCESSFUL;

    if (!pFT_Load_Glyph(face, params->glyph, FT_LOAD_NO_BITMAP))
    {
        FT_Outline *ft_outline = &face->glyph->outline;
        FT_Matrix m;

        if (params->outline->points.values)
        {
            if (params->simulations & DWRITE_FONT_SIMULATIONS_BOLD)
                embolden_glyph_outline(ft_outline, params->emsize);

            m.xx = 1 << 16;
            m.xy = params->simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE ? (1 << 16) / 3 : 0;
            m.yx = 0;
            m.yy = -(1 << 16); /* flip Y axis */

            pFT_Outline_Transform(ft_outline, &m);

            decompose_outline(ft_outline, params->outline);
        }
        else
        {
            /* Intentionally overestimate numbers to keep it simple. */
            params->outline->points.count = ft_outline->n_points * 3;
            params->outline->tags.count = ft_outline->n_points + ft_outline->n_contours * 2;
        }
    }

    pFT_Done_Size(size);

    return STATUS_SUCCESS;
}

static NTSTATUS get_glyph_count(void *args)
{
    struct get_glyph_count_params *params = args;
    FT_Face face = FaceFromObject(params->object);

    *params->count = face ? face->num_glyphs : 0;

    return STATUS_SUCCESS;
}

static inline void ft_matrix_from_matrix_2x2(const MATRIX_2X2 *m, FT_Matrix *ft_matrix)
{
    ft_matrix->xx =  m->m11 * 0x10000;
    ft_matrix->xy = -m->m21 * 0x10000;
    ft_matrix->yx = -m->m12 * 0x10000;
    ft_matrix->yy =  m->m22 * 0x10000;
}

static BOOL get_glyph_transform(unsigned int simulations, const MATRIX_2X2 *m, FT_Matrix *ret)
{
    FT_Matrix ftm;

    ret->xx = 1 << 16;
    ret->xy = 0;
    ret->yx = 0;
    ret->yy = 1 << 16;

    /* Some fonts provide mostly bitmaps and very few outlines, for example for .notdef.
       Disable transform if that's the case. */
    if (!memcmp(m, &identity_2x2, sizeof(*m)) && !simulations)
        return FALSE;

    if (simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE)
    {
        ftm.xx =  1 << 16;
        ftm.xy = (1 << 16) / 3;
        ftm.yx =  0;
        ftm.yy =  1 << 16;
        pFT_Matrix_Multiply(&ftm, ret);
    }

    ft_matrix_from_matrix_2x2(m, &ftm);
    pFT_Matrix_Multiply(&ftm, ret);

    return TRUE;
}

static NTSTATUS get_glyph_bbox(void *args)
{
    struct get_glyph_bbox_params *params = args;
    FT_Face face = FaceFromObject(params->object);
    FT_Glyph glyph = NULL;
    FT_BBox bbox = { 0 };
    BOOL needs_transform;
    FT_Matrix m;
    FT_Size size;

    SetRectEmpty(params->bbox);

    if (!(size = freetype_set_face_size(face, params->emsize)))
        return STATUS_UNSUCCESSFUL;

    needs_transform = FT_IS_SCALABLE(face) && get_glyph_transform(params->simulations, &params->m, &m);

    if (pFT_Load_Glyph(face, params->glyph, needs_transform ? FT_LOAD_NO_BITMAP : 0))
    {
        WARN("Failed to load glyph %u.\n", params->glyph);
        pFT_Done_Size(size);
        return STATUS_UNSUCCESSFUL;
    }

    pFT_Get_Glyph(face->glyph, &glyph);
    if (needs_transform)
    {
        if (params->simulations & DWRITE_FONT_SIMULATIONS_BOLD)
            embolden_glyph(glyph, params->emsize);

        /* Includes oblique and user transform. */
        pFT_Glyph_Transform(glyph, &m, NULL);
    }

    pFT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &bbox);
    pFT_Done_Glyph(glyph);
    pFT_Done_Size(size);

    /* flip Y axis */
    SetRect(params->bbox, bbox.xMin, -bbox.yMax, bbox.xMax, -bbox.yMin);

    return STATUS_SUCCESS;
}

static NTSTATUS freetype_get_aliased_glyph_bitmap(struct get_glyph_bitmap_params *params, FT_Glyph glyph)
{
    const RECT *bbox = &params->bbox;
    int width = bbox->right - bbox->left;
    int height = bbox->bottom - bbox->top;

    *params->is_1bpp = 1;

    if (glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_OutlineGlyph outline = (FT_OutlineGlyph)glyph;
        const FT_Outline *src = &outline->outline;
        FT_Bitmap ft_bitmap;
        FT_Outline copy;

        ft_bitmap.width = width;
        ft_bitmap.rows = height;
        ft_bitmap.pitch = params->pitch;
        ft_bitmap.pixel_mode = FT_PIXEL_MODE_MONO;
        ft_bitmap.buffer = params->bitmap;

        /* Note: FreeType will only set 'black' bits for us. */
        if (pFT_Outline_New(library, src->n_points, src->n_contours, &copy) == 0) {
            pFT_Outline_Copy(src, &copy);
            pFT_Outline_Translate(&copy, -bbox->left << 6, bbox->bottom << 6);
            pFT_Outline_Get_Bitmap(library, &copy, &ft_bitmap);
            pFT_Outline_Done(library, &copy);
        }
    }
    else if (glyph->format == FT_GLYPH_FORMAT_BITMAP) {
        FT_Bitmap *ft_bitmap = &((FT_BitmapGlyph)glyph)->bitmap;
        BYTE *src = ft_bitmap->buffer, *dst = params->bitmap;
        int w = min(params->pitch, (ft_bitmap->width + 7) >> 3);
        int h = min(height, ft_bitmap->rows);

        while (h--) {
            memcpy(dst, src, w);
            src += ft_bitmap->pitch;
            dst += params->pitch;
        }
    }
    else
        FIXME("format %x not handled\n", glyph->format);

    return STATUS_SUCCESS;
}

static NTSTATUS freetype_get_aa_glyph_bitmap(struct get_glyph_bitmap_params *params, FT_Glyph glyph)
{
    const RECT *bbox = &params->bbox;
    int width = bbox->right - bbox->left;
    int height = bbox->bottom - bbox->top;

    *params->is_1bpp = 0;

    if (glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_OutlineGlyph outline = (FT_OutlineGlyph)glyph;
        const FT_Outline *src = &outline->outline;
        FT_Bitmap ft_bitmap;
        FT_Outline copy;

        ft_bitmap.width = width;
        ft_bitmap.rows = height;
        ft_bitmap.pitch = params->pitch;
        ft_bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
        ft_bitmap.buffer = params->bitmap;

        /* Note: FreeType will only set 'black' bits for us. */
        if (pFT_Outline_New(library, src->n_points, src->n_contours, &copy) == 0) {
            pFT_Outline_Copy(src, &copy);
            pFT_Outline_Translate(&copy, -bbox->left << 6, bbox->bottom << 6);
            pFT_Outline_Get_Bitmap(library, &copy, &ft_bitmap);
            pFT_Outline_Done(library, &copy);
        }
    }
    else if (glyph->format == FT_GLYPH_FORMAT_BITMAP) {
        FT_Bitmap *ft_bitmap = &((FT_BitmapGlyph)glyph)->bitmap;
        BYTE *src = ft_bitmap->buffer, *dst = params->bitmap;
        int w = min(params->pitch, (ft_bitmap->width + 7) >> 3);
        int h = min(height, ft_bitmap->rows);

        while (h--) {
            memcpy(dst, src, w);
            src += ft_bitmap->pitch;
            dst += params->pitch;
        }

        *params->is_1bpp = 1;
    }
    else
    {
        FIXME("format %x not handled\n", glyph->format);
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS get_glyph_bitmap(void *args)
{
    struct get_glyph_bitmap_params *params = args;
    FT_Face face = FaceFromObject(params->object);
    BOOL needs_transform;
    BOOL ret = FALSE;
    FT_Glyph glyph;
    FT_Size size;
    FT_Matrix m;

    *params->is_1bpp = 0;

    if (!(size = freetype_set_face_size(face, params->emsize)))
        return STATUS_UNSUCCESSFUL;

    needs_transform = FT_IS_SCALABLE(face) && get_glyph_transform(params->simulations, &params->m, &m);

    if (!pFT_Load_Glyph(face, params->glyph, needs_transform ? FT_LOAD_NO_BITMAP : 0))
    {
        pFT_Get_Glyph(face->glyph, &glyph);

        if (needs_transform)
        {
            if (params->simulations & DWRITE_FONT_SIMULATIONS_BOLD)
                embolden_glyph(glyph, params->emsize);

            /* Includes oblique and user transform. */
            pFT_Glyph_Transform(glyph, &m, NULL);
        }

        if (params->mode == DWRITE_RENDERING_MODE1_ALIASED)
            ret = freetype_get_aliased_glyph_bitmap(params, glyph);
        else
            ret = freetype_get_aa_glyph_bitmap(params, glyph);

        pFT_Done_Glyph(glyph);
    }

    pFT_Done_Size(size);

    return ret;
}

static NTSTATUS get_glyph_advance(void *args)
{
    struct get_glyph_advance_params *params = args;
    FT_Face face = FaceFromObject(params->object);
    FT_Size size;

    *params->advance = 0;
    *params->has_contours = FALSE;

    if (!(size = freetype_set_face_size(face, params->emsize)))
        return STATUS_UNSUCCESSFUL;

    if (!pFT_Load_Glyph(face, params->glyph, params->mode == DWRITE_MEASURING_MODE_NATURAL ? FT_LOAD_NO_HINTING : 0))
    {
        *params->advance = face->glyph->advance.x >> 6;
        *params->has_contours = freetype_glyph_has_contours(face);
    }

    pFT_Done_Size(size);

    return STATUS_SUCCESS;
}

#else /* SONAME_LIBFREETYPE */

static NTSTATUS process_attach(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS process_detach(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS create_font_object(void *args)
{
    struct create_font_object_params *params = args;

    *params->object = 0;
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS release_font_object(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_glyph_outline(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_glyph_count(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_glyph_advance(void *args)
{
    struct get_glyph_advance_params *params = args;

    *params->has_contours = 0;
    *params->advance = 0;

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_glyph_bbox(void *args)
{
    struct get_glyph_bbox_params *params = args;
    SetRectEmpty(params->bbox);
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_glyph_bitmap(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS get_design_glyph_metrics(void *args)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif /* SONAME_LIBFREETYPE */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_attach,
    process_detach,
    create_font_object,
    release_font_object,
    get_glyph_outline,
    get_glyph_count,
    get_glyph_advance,
    get_glyph_bbox,
    get_glyph_bitmap,
    get_design_glyph_metrics,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

typedef ULONG PTR32;

struct wow64_create_font_object_params
{
    PTR32 data;
    UINT64 size;
    ULONG index;
    PTR32 axis_values;
    ULONG axis_values_count;
    ULONG axis_values_are_default;
    PTR32 object;
};

struct wow64_release_font_object_params
{
    UINT64 object;
};

struct wow64_get_glyph_outline_params
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    float emsize;
    PTR32 outline;
};

struct wow64_get_glyph_count_params
{
    UINT64 object;
    PTR32 count;
};

struct wow64_get_glyph_advance_params
{
    UINT64 object;
    ULONG glyph;
    ULONG mode;
    float emsize;
    PTR32 advance;
    PTR32 has_contours;
};

struct wow64_get_glyph_bbox_params
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    float emsize;
    MATRIX_2X2 m;
    PTR32 bbox;
};

struct wow64_get_glyph_bitmap_params
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    ULONG mode;
    float emsize;
    MATRIX_2X2 m;
    RECT bbox;
    int pitch;
    PTR32 bitmap;
    PTR32 is_1bpp;
};

struct wow64_get_design_glyph_metrics_params
{
    UINT64 object;
    ULONG simulations;
    ULONG glyph;
    ULONG upem;
    ULONG ascent;
    PTR32 metrics;
};

struct dwrite_outline32
{
    struct
    {
        PTR32 values;
        ULONG count;
        ULONG size;
    } tags;

    struct
    {
        PTR32 values;
        ULONG count;
        ULONG size;
    } points;
};

C_ASSERT(sizeof(struct wow64_create_font_object_params) == 40);
C_ASSERT(sizeof(struct wow64_release_font_object_params) == 8);
C_ASSERT(sizeof(struct wow64_get_glyph_outline_params) == 24);
C_ASSERT(sizeof(struct wow64_get_glyph_count_params) == 16);
C_ASSERT(sizeof(struct wow64_get_glyph_advance_params) == 32);
C_ASSERT(sizeof(struct wow64_get_glyph_bbox_params) == 40);
C_ASSERT(sizeof(struct wow64_get_glyph_bitmap_params) == 72);
C_ASSERT(sizeof(struct wow64_get_design_glyph_metrics_params) == 32);
C_ASSERT(sizeof(struct dwrite_outline32) == 24);
C_ASSERT(sizeof(SIZE_T) >= sizeof(UINT64));

static NTSTATUS wow64_guest_pointer(PTR32 address, SIZE_T offset, void **ptr)
{
    if ((!address && offset) || offset > MAXDWORD - address)
        return STATUS_ACCESS_VIOLATION;
    return ntdll_wow64_guest32_to_host(address + offset, ptr);
}

#ifdef SONAME_LIBFREETYPE
static NTSTATUS wow64_copy_from_guest(void *dst, PTR32 src, SIZE_T size)
{
    void *ptr;
    NTSTATUS status;

    if (size && (!src || size > 0x100000000ULL - src)) return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(src, 0, &ptr))) return status;
    return ntdll_wow64_copy_from_user(dst, ptr, size);
}

static NTSTATUS wow64_probe_read_guest(PTR32 src, SIZE_T size)
{
    void *ptr;
    NTSTATUS status;

    if (!size) return STATUS_SUCCESS;
    if (!src || size > 0x100000000ULL - src) return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(src, 0, &ptr))) return status;
    return ntdll_wow64_probe_user_read(ptr, size);
}
#endif

static NTSTATUS wow64_write_guest(PTR32 dst, const void *src, SIZE_T size)
{
    void *ptr;
    NTSTATUS status;

    if (size && (!dst || size > 0x100000000ULL - dst)) return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(dst, 0, &ptr))) return status;
    return ntdll_wow64_faulting_copy_to_user(ptr, src, size);
}

static NTSTATUS wow64_atomic_write_guest(PTR32 dst, const void *src, SIZE_T size)
{
    void *ptr;
    NTSTATUS status;

    if (size && (!dst || size > 0x100000000ULL - dst)) return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(dst, 0, &ptr))) return status;
    return ntdll_wow64_atomic_write_user(ptr, src, size);
}

static NTSTATUS wow64_probe_write_guest(PTR32 dst, SIZE_T size)
{
    void *ptr;
    NTSTATUS status;

    if (!size) return STATUS_SUCCESS;
    if (!dst || size > 0x100000000ULL - dst) return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(dst, 0, &ptr))) return status;
    return ntdll_wow64_probe_user_write(ptr, size);
}

static NTSTATUS wow64_probe_write_pair(PTR32 dst1, SIZE_T size1, PTR32 dst2, SIZE_T size2)
{
    struct ntdll_wow64_user_write_range ranges[2];
    NTSTATUS status;
    void *ptr;

    if (size1 && (!dst1 || size1 > 0x100000000ULL - dst1)) return STATUS_ACCESS_VIOLATION;
    if (size1 && (status = wow64_guest_pointer(dst1, 0, &ptr))) return status;
    if (!size1) ptr = NULL;
    ranges[0] = (struct ntdll_wow64_user_write_range){ ptr, NULL, size1 };
    if (size2 && (!dst2 || size2 > 0x100000000ULL - dst2)) return STATUS_ACCESS_VIOLATION;
    if (size2 && (status = wow64_guest_pointer(dst2, 0, &ptr))) return status;
    if (!size2) ptr = NULL;
    ranges[1] = (struct ntdll_wow64_user_write_range){ ptr, NULL, size2 };
    return ntdll_wow64_probe_user_writev(ranges, ARRAY_SIZE(ranges));
}

static NTSTATUS wow64_atomic_write_pair(PTR32 dst1, const void *src1, SIZE_T size1,
                                        PTR32 dst2, const void *src2, SIZE_T size2)
{
    struct ntdll_wow64_user_write_range ranges[2];
    NTSTATUS status;
    void *ptr;

    if (size1 && (!dst1 || size1 > 0x100000000ULL - dst1)) return STATUS_ACCESS_VIOLATION;
    if (size1 && (status = wow64_guest_pointer(dst1, 0, &ptr))) return status;
    if (!size1) ptr = NULL;
    ranges[0] = (struct ntdll_wow64_user_write_range){ ptr, src1, size1 };
    if (size2 && (!dst2 || size2 > 0x100000000ULL - dst2)) return STATUS_ACCESS_VIOLATION;
    if (size2 && (status = wow64_guest_pointer(dst2, 0, &ptr))) return status;
    if (!size2) ptr = NULL;
    ranges[1] = (struct ntdll_wow64_user_write_range){ ptr, src2, size2 };
    return ntdll_wow64_atomic_writev(ranges, ARRAY_SIZE(ranges));
}

#ifdef SONAME_LIBFREETYPE

#define WOW64_FONT_AXIS_MAX_COUNT 0xffff
#define WOW64_FONT_OBJECT_BUCKETS 64
#define WOW64_FONT_TOKEN_PREFIX 0x4457000000000000ULL
#define WOW64_FONT_TOKEN_MASK   0x0000ffffffffffffULL

struct wow64_font_object
{
    struct wow64_font_object *next;
    UINT64 token;
    unsigned int refs;
    pthread_mutex_t mutex;
    FT_Face face;
    void *data;
};

static pthread_mutex_t wow64_font_objects_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wow64_font_objects_cond = PTHREAD_COND_INITIALIZER;
static struct wow64_font_object *wow64_font_objects[WOW64_FONT_OBJECT_BUCKETS];
static UINT64 wow64_font_next_token = 1;
static unsigned int wow64_font_object_count;
static BOOL wow64_font_registry_detaching;

static unsigned int wow64_font_object_hash(UINT64 token)
{
    return (token ^ (token >> 32)) % WOW64_FONT_OBJECT_BUCKETS;
}

static struct wow64_font_object *wow64_find_font_object_locked(UINT64 token)
{
    struct wow64_font_object *object;

    for (object = wow64_font_objects[wow64_font_object_hash(token)]; object; object = object->next)
        if (object->token == token) return object;
    return NULL;
}

static void wow64_destroy_font_object(struct wow64_font_object *object)
{
    pFT_Done_Face(object->face);
    free(object->data);
    pthread_mutex_destroy(&object->mutex);
    free(object);

    pthread_mutex_lock(&wow64_font_objects_mutex);
    --wow64_font_object_count;
    pthread_cond_broadcast(&wow64_font_objects_cond);
    pthread_mutex_unlock(&wow64_font_objects_mutex);
}

static void wow64_font_registry_attach(void)
{
    pthread_mutex_lock(&wow64_font_objects_mutex);
    wow64_font_registry_detaching = FALSE;
    pthread_mutex_unlock(&wow64_font_objects_mutex);
}

static void wow64_font_registry_cleanup(void)
{
    struct wow64_font_object *destroy_list = NULL, *object, *next;
    unsigned int bucket;

    pthread_mutex_lock(&wow64_font_objects_mutex);
    wow64_font_registry_detaching = TRUE;
    for (bucket = 0; bucket < ARRAY_SIZE(wow64_font_objects); ++bucket)
    {
        for (object = wow64_font_objects[bucket]; object; object = next)
        {
            next = object->next;
            object->next = NULL;
            if (!--object->refs)
            {
                object->next = destroy_list;
                destroy_list = object;
            }
        }
        wow64_font_objects[bucket] = NULL;
    }
    pthread_mutex_unlock(&wow64_font_objects_mutex);

    while ((object = destroy_list))
    {
        destroy_list = object->next;
        wow64_destroy_font_object(object);
    }

    pthread_mutex_lock(&wow64_font_objects_mutex);
    while (wow64_font_object_count)
        pthread_cond_wait(&wow64_font_objects_cond, &wow64_font_objects_mutex);
    pthread_mutex_unlock(&wow64_font_objects_mutex);
}

static NTSTATUS wow64_register_font_object(struct wow64_font_object *object, UINT64 *token)
{
    unsigned int bucket;

    if (pthread_mutex_init(&object->mutex, NULL)) return STATUS_NO_MEMORY;
    object->refs = 1; /* registry reference */

    pthread_mutex_lock(&wow64_font_objects_mutex);
    if (wow64_font_registry_detaching)
    {
        pthread_mutex_unlock(&wow64_font_objects_mutex);
        pthread_mutex_destroy(&object->mutex);
        return STATUS_UNSUCCESSFUL;
    }
    do
    {
        object->token = WOW64_FONT_TOKEN_PREFIX | (wow64_font_next_token++ & WOW64_FONT_TOKEN_MASK);
        if (!(wow64_font_next_token & WOW64_FONT_TOKEN_MASK)) wow64_font_next_token = 1;
    } while (!object->token || wow64_find_font_object_locked(object->token));
    bucket = wow64_font_object_hash(object->token);
    object->next = wow64_font_objects[bucket];
    wow64_font_objects[bucket] = object;
    ++wow64_font_object_count;
    pthread_mutex_unlock(&wow64_font_objects_mutex);

    *token = object->token;
    return STATUS_SUCCESS;
}

static NTSTATUS wow64_acquire_font_object(UINT64 token, struct wow64_font_object **ret)
{
    struct wow64_font_object *object;
    BOOL destroy;

    *ret = NULL;
    if (!token) return STATUS_INVALID_HANDLE;

    pthread_mutex_lock(&wow64_font_objects_mutex);
    if (!(object = wow64_find_font_object_locked(token)) || object->refs == UINT_MAX)
    {
        pthread_mutex_unlock(&wow64_font_objects_mutex);
        return STATUS_INVALID_HANDLE;
    }
    ++object->refs;
    pthread_mutex_unlock(&wow64_font_objects_mutex);

    if (pthread_mutex_lock(&object->mutex))
    {
        pthread_mutex_lock(&wow64_font_objects_mutex);
        destroy = !--object->refs;
        pthread_mutex_unlock(&wow64_font_objects_mutex);
        if (destroy) wow64_destroy_font_object(object);
        return STATUS_UNSUCCESSFUL;
    }
    *ret = object;
    return STATUS_SUCCESS;
}

static void wow64_release_font_object_ref(struct wow64_font_object *object)
{
    BOOL destroy;

    pthread_mutex_unlock(&object->mutex);
    pthread_mutex_lock(&wow64_font_objects_mutex);
    destroy = !--object->refs;
    pthread_mutex_unlock(&wow64_font_objects_mutex);
    if (destroy) wow64_destroy_font_object(object);
}

static NTSTATUS wow64_unregister_font_object(UINT64 token)
{
    struct wow64_font_object **cursor, *object;
    unsigned int bucket;
    BOOL destroy;

    if (!token) return STATUS_INVALID_HANDLE;
    bucket = wow64_font_object_hash(token);
    pthread_mutex_lock(&wow64_font_objects_mutex);
    for (cursor = &wow64_font_objects[bucket]; (object = *cursor); cursor = &object->next)
    {
        if (object->token != token) continue;
        *cursor = object->next;
        destroy = !--object->refs;
        pthread_mutex_unlock(&wow64_font_objects_mutex);
        if (destroy) wow64_destroy_font_object(object);
        return STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&wow64_font_objects_mutex);

    return STATUS_INVALID_HANDLE;
}

#endif

static NTSTATUS wow64_create_font_object(void *args)
{
    const struct wow64_create_font_object_params *params32 = args;
    UINT64 value = 0;
    NTSTATUS status;
#ifdef SONAME_LIBFREETYPE
    DWRITE_FONT_AXIS_VALUE *axis_values = NULL;
    struct wow64_font_object *object = NULL;
    FT_Fixed *coordinates = NULL;
    FT_Face face = NULL;
    FT_Error fterror;
    void *data = NULL;
    SIZE_T axis_size, size;
    unsigned int i;
#endif

    if ((status = wow64_write_guest(params32->object, &value, sizeof(value)))) return status;

#ifdef SONAME_LIBFREETYPE
    if (params32->size > (UINT64)LONG_MAX)
        return STATUS_INVALID_PARAMETER;
    size = params32->size;
    if (size)
    {
        if (!params32->data || size > 0x100000000ULL - params32->data)
            return STATUS_ACCESS_VIOLATION;
        if ((status = wow64_probe_read_guest(params32->data, size))) return status;
        if (!(data = malloc(size))) return STATUS_NO_MEMORY;
        if ((status = wow64_copy_from_guest(data, params32->data, size)))
        {
            free(data);
            return status;
        }
    }

    fterror = pFT_New_Memory_Face(library, data, params32->size, params32->index, &face);
    if (fterror != FT_Err_Ok)
    {
        WARN("Failed to create a face object, error %d.\n", fterror);
        free(data);
        return STATUS_UNSUCCESSFUL;
    }

    if (params32->axis_values_count)
    {
        if (!pFT_Set_Var_Design_Coordinates)
        {
            if (!params32->axis_values_are_default)
            {
                WARN("FreeType does not support variable font coordinates.\n");
                status = STATUS_NOT_SUPPORTED;
                goto failed;
            }
        }
        else
        {
            if (params32->axis_values_count > WOW64_FONT_AXIS_MAX_COUNT)
            {
                status = STATUS_INVALID_PARAMETER;
                goto failed;
            }
            axis_size = (SIZE_T)params32->axis_values_count * sizeof(*axis_values);
            if (!params32->axis_values || axis_size > 0x100000000ULL - params32->axis_values)
            {
                status = STATUS_ACCESS_VIOLATION;
                goto failed;
            }
            if ((status = wow64_probe_read_guest(params32->axis_values, axis_size))) goto failed;
            if (!(coordinates = calloc(params32->axis_values_count, sizeof(*coordinates))) ||
                !(axis_values = malloc(axis_size)))
            {
                status = STATUS_NO_MEMORY;
                goto failed;
            }
            if ((status = wow64_copy_from_guest(axis_values, params32->axis_values, axis_size)))
                goto failed;

            for (i = 0; i < params32->axis_values_count; ++i)
                coordinates[i] = axis_values[i].value * 65536.0f;

            fterror = pFT_Set_Var_Design_Coordinates(face, params32->axis_values_count, coordinates);
            if (fterror != FT_Err_Ok && !params32->axis_values_are_default)
            {
                WARN("Failed to set variable font coordinates, error %d.\n", fterror);
                status = STATUS_UNSUCCESSFUL;
                goto failed;
            }
        }
    }

    if (!(object = malloc(sizeof(*object))))
    {
        status = STATUS_NO_MEMORY;
        goto failed;
    }
    object->face = face;
    object->data = data;
    if ((status = wow64_register_font_object(object, &value)))
    {
        free(object);
        goto failed;
    }
    object = NULL;
    face = NULL;
    data = NULL;
    if ((status = wow64_atomic_write_guest(params32->object, &value, sizeof(value))))
    {
        wow64_unregister_font_object(value);
        free(axis_values);
        free(coordinates);
        return status;
    }

    free(axis_values);
    free(coordinates);
    return STATUS_SUCCESS;

failed:
    free(axis_values);
    free(coordinates);
    pFT_Done_Face(face);
    free(data);
    return status;
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS wow64_release_font_object(void *args)
{
    const struct wow64_release_font_object_params *params32 = args;

#ifdef SONAME_LIBFREETYPE
    return wow64_unregister_font_object(params32->object);
#else
    return release_font_object((void *)params32);
#endif
}

#ifdef SONAME_LIBFREETYPE
static NTSTATUS wow64_publish_outline(PTR32 outline_address, const struct dwrite_outline32 *outline32,
                                      const BYTE *tags, ULONG tags_count,
                                      const D2D1_POINT_2F *points, ULONG points_count,
                                      ULONG result_tags_count, ULONG result_points_count)
{
    struct ntdll_wow64_user_write_range ranges[4];
    SIZE_T bytes, offset;
    ULONG count = 0;
    void *ptr;
    NTSTATUS status;

    if (tags_count)
    {
        offset = (SIZE_T)outline32->tags.count * sizeof(*tags);
        bytes = (SIZE_T)tags_count * sizeof(*tags);
        if (!outline32->tags.values || offset > 0x100000000ULL - outline32->tags.values ||
            bytes > 0x100000000ULL - outline32->tags.values - offset ||
            (status = wow64_guest_pointer(outline32->tags.values, offset, &ptr)))
            return STATUS_ACCESS_VIOLATION;
        ranges[count++] = (struct ntdll_wow64_user_write_range)
                          { ptr, tags, bytes };
    }
    if (points_count)
    {
        offset = (SIZE_T)outline32->points.count * sizeof(*points);
        bytes = (SIZE_T)points_count * sizeof(*points);
        if (!outline32->points.values || offset > 0x100000000ULL - outline32->points.values ||
            bytes > 0x100000000ULL - outline32->points.values - offset ||
            (status = wow64_guest_pointer(outline32->points.values, offset, &ptr)))
            return STATUS_ACCESS_VIOLATION;
        ranges[count++] = (struct ntdll_wow64_user_write_range)
                          { ptr, points, bytes };
    }
    offset = offsetof(struct dwrite_outline32, points.count);
    if (!outline_address || offset > 0x100000000ULL - outline_address ||
        sizeof(result_points_count) > 0x100000000ULL - outline_address - offset)
        return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(outline_address,
                                      offset, &ptr)))
        return status;
    ranges[count++] = (struct ntdll_wow64_user_write_range)
                      { ptr, &result_points_count, sizeof(result_points_count) };
    offset = offsetof(struct dwrite_outline32, tags.count);
    if (offset > 0x100000000ULL - outline_address ||
        sizeof(result_tags_count) > 0x100000000ULL - outline_address - offset)
        return STATUS_ACCESS_VIOLATION;
    if ((status = wow64_guest_pointer(outline_address,
                                      offset, &ptr)))
        return status;
    ranges[count++] = (struct ntdll_wow64_user_write_range)
                      { ptr, &result_tags_count, sizeof(result_tags_count) };
    return ntdll_wow64_atomic_writev(ranges, count);
}
#endif

static NTSTATUS wow64_get_glyph_outline(void *args)
{
#ifdef SONAME_LIBFREETYPE
    const struct wow64_get_glyph_outline_params *params32 = args;
    struct dwrite_outline32 outline32;
    ULONG result_tags_count, result_points_count;
    ULONG tags_count = 0, points_count = 0;
    D2D1_POINT_2F *points = NULL;
    BYTE *tags = NULL;
    NTSTATUS status;

    if ((status = wow64_copy_from_guest(&outline32, params32->outline, sizeof(outline32))))
        return status;
    result_tags_count = outline32.tags.count;
    result_points_count = outline32.points.count;

    {
        struct wow64_font_object *object;
        FT_Face face;
        FT_Size size;

        if ((status = wow64_acquire_font_object(params32->object, &object))) return status;
        face = object->face;
        if (!(size = freetype_set_face_size(face, params32->emsize)))
            status = STATUS_UNSUCCESSFUL;
        else
        {
            status = STATUS_SUCCESS;
            if (!pFT_Load_Glyph(face, params32->glyph, FT_LOAD_NO_BITMAP))
            {
                FT_Outline *ft_outline = &face->glyph->outline;

                if (outline32.points.values)
                {
                    struct dwrite_outline outline = {0};
                    ULONG tags_capacity = 0, points_capacity = 0;
                    ULONG max_tags = 0, max_points = 0;

                    if (ft_outline->n_points > 0)
                    {
                        max_points = (ULONG)ft_outline->n_points * 3;
                        max_tags = ft_outline->n_points;
                    }
                    if (ft_outline->n_contours > 0)
                        max_tags += (ULONG)ft_outline->n_contours * 2;
                    if (outline32.tags.size >= outline32.tags.count)
                        tags_capacity = min(outline32.tags.size - outline32.tags.count, max_tags);
                    if (outline32.points.size >= outline32.points.count)
                        points_capacity = min(outline32.points.size - outline32.points.count, max_points);

                    if ((tags_capacity && !(tags = malloc(tags_capacity * sizeof(*tags)))) ||
                        (points_capacity && !(points = malloc(points_capacity * sizeof(*points)))))
                        status = STATUS_NO_MEMORY;
                    else
                    {
                        FT_Matrix m;

                        outline.tags.values = tags;
                        outline.tags.size = tags_capacity;
                        outline.points.values = points;
                        outline.points.size = points_capacity;
                        if (params32->simulations & DWRITE_FONT_SIMULATIONS_BOLD)
                            embolden_glyph_outline(ft_outline, params32->emsize);

                        m.xx = 1 << 16;
                        m.xy = params32->simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE ? (1 << 16) / 3 : 0;
                        m.yx = 0;
                        m.yy = -(1 << 16);
                        pFT_Outline_Transform(ft_outline, &m);
                        decompose_outline(ft_outline, &outline);
                        tags_count = outline.tags.count;
                        points_count = outline.points.count;
                        result_tags_count += tags_count;
                        result_points_count += points_count;
                    }
                }
                else
                {
                    result_points_count = ft_outline->n_points * 3;
                    result_tags_count = ft_outline->n_points + ft_outline->n_contours * 2;
                }
            }
            pFT_Done_Size(size);
        }
        wow64_release_font_object_ref(object);
    }
    {
        NTSTATUS publish_status = wow64_publish_outline(params32->outline, &outline32, tags, tags_count,
                                                        points, points_count, result_tags_count,
                                                        result_points_count);
        free(tags);
        free(points);
        return publish_status ? publish_status : status;
    }
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS wow64_get_glyph_count(void *args)
{
    const struct wow64_get_glyph_count_params *params32 = args;
#ifdef SONAME_LIBFREETYPE
    struct wow64_font_object *object;
    unsigned int count;
    struct get_glyph_count_params params;
    NTSTATUS status, publish_status;

    if ((status = wow64_acquire_font_object(params32->object, &object))) return status;
    params.object = (ULONG_PTR)object->face;
    params.count = &count;
    status = get_glyph_count(&params);
    wow64_release_font_object_ref(object);
    publish_status = wow64_atomic_write_guest(params32->count, &count, sizeof(count));
    return publish_status ? publish_status : status;
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS wow64_get_glyph_advance(void *args)
{
    const struct wow64_get_glyph_advance_params *params32 = args;
    int advance = 0;
    unsigned int has_contours = 0;
    struct get_glyph_advance_params params;
    NTSTATUS status, publish_status;
#ifdef SONAME_LIBFREETYPE
    struct wow64_font_object *object;
#endif

    if ((status = wow64_probe_write_pair(params32->advance, sizeof(advance),
                                         params32->has_contours, sizeof(has_contours)))) return status;
#ifdef SONAME_LIBFREETYPE
    if ((status = wow64_acquire_font_object(params32->object, &object))) return status;
    params.object = (ULONG_PTR)object->face;
#else
    params.object = params32->object;
#endif
    params.glyph = params32->glyph;
    params.mode = params32->mode;
    params.emsize = params32->emsize;
    params.advance = &advance;
    params.has_contours = &has_contours;
    status = get_glyph_advance(&params);
#ifdef SONAME_LIBFREETYPE
    wow64_release_font_object_ref(object);
#endif
    publish_status = wow64_atomic_write_pair(params32->advance, &advance, sizeof(advance),
                                             params32->has_contours, &has_contours,
                                             sizeof(has_contours));
    return publish_status ? publish_status : status;
}

static NTSTATUS wow64_get_glyph_bbox(void *args)
{
    const struct wow64_get_glyph_bbox_params *params32 = args;
    RECT bbox = {0};
    struct get_glyph_bbox_params params;
    NTSTATUS status, publish_status;
#ifdef SONAME_LIBFREETYPE
    struct wow64_font_object *object;
#endif

    if ((status = wow64_probe_write_guest(params32->bbox, sizeof(bbox)))) return status;
#ifdef SONAME_LIBFREETYPE
    if ((status = wow64_acquire_font_object(params32->object, &object))) return status;
    params.object = (ULONG_PTR)object->face;
#else
    params.object = params32->object;
#endif
    params.simulations = params32->simulations;
    params.glyph = params32->glyph;
    params.emsize = params32->emsize;
    params.m = params32->m;
    params.bbox = &bbox;
    status = get_glyph_bbox(&params);
#ifdef SONAME_LIBFREETYPE
    wow64_release_font_object_ref(object);
#endif
    publish_status = wow64_atomic_write_guest(params32->bbox, &bbox, sizeof(bbox));
    return publish_status ? publish_status : status;
}

static NTSTATUS wow64_get_glyph_bitmap(void *args)
{
    const struct wow64_get_glyph_bitmap_params *params32 = args;
    unsigned int is_1bpp = 0;
    NTSTATUS status;

#ifdef SONAME_LIBFREETYPE
    struct wow64_font_object *object;
    LONGLONG width = (LONGLONG)params32->bbox.right - params32->bbox.left;
    LONGLONG height = (LONGLONG)params32->bbox.bottom - params32->bbox.top;
    SIZE_T min_pitch, bitmap_size;

    if ((status = wow64_probe_write_guest(params32->is_1bpp, sizeof(is_1bpp)))) return status;
    if (width < 0 || width > MAXLONG || height < 0 || height > MAXLONG || params32->pitch < 0)
    {
        status = wow64_atomic_write_guest(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp));
        return status ? status : STATUS_INVALID_PARAMETER;
    }
    if (params32->mode == DWRITE_RENDERING_MODE1_ALIASED)
        min_pitch = (((SIZE_T)width + 31) >> 5) << 2;
    else
        min_pitch = (((SIZE_T)width + 3) / 4) * 4;
    if ((SIZE_T)params32->pitch < min_pitch ||
        (ULONGLONG)height > ~(SIZE_T)0 / (unsigned int)max(params32->pitch, 1))
    {
        status = wow64_atomic_write_guest(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp));
        return status ? status : STATUS_INVALID_PARAMETER;
    }
    bitmap_size = (SIZE_T)height * params32->pitch;
    if ((status = wow64_probe_write_guest(params32->bitmap, bitmap_size))) return status;
    if ((status = wow64_acquire_font_object(params32->object, &object))) return status;
    {
        FT_Face face = object->face;
        BOOL needs_transform;
        NTSTATUS ret = STATUS_SUCCESS;
        FT_Glyph glyph;
        FT_Size size;
        FT_Matrix m;

        if (!(size = freetype_set_face_size(face, params32->emsize)))
        {
            wow64_release_font_object_ref(object);
            status = wow64_atomic_write_guest(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp));
            return status ? status : STATUS_UNSUCCESSFUL;
        }

        needs_transform = FT_IS_SCALABLE(face) && get_glyph_transform(params32->simulations, &params32->m, &m);
        if (!pFT_Load_Glyph(face, params32->glyph, needs_transform ? FT_LOAD_NO_BITMAP : 0))
        {
            BYTE *bitmap;
            struct get_glyph_bitmap_params params;

            if (!(bitmap = calloc(1, max(bitmap_size, (SIZE_T)1))))
            {
                pFT_Done_Size(size);
                wow64_release_font_object_ref(object);
                status = wow64_atomic_write_guest(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp));
                return status ? status : STATUS_NO_MEMORY;
            }

            if (pFT_Get_Glyph(face->glyph, &glyph))
            {
                free(bitmap);
                pFT_Done_Size(size);
                wow64_release_font_object_ref(object);
                status = wow64_atomic_write_guest(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp));
                return status ? status : STATUS_UNSUCCESSFUL;
            }
            if (needs_transform)
            {
                if (params32->simulations & DWRITE_FONT_SIMULATIONS_BOLD)
                    embolden_glyph(glyph, params32->emsize);
                pFT_Glyph_Transform(glyph, &m, NULL);
            }

            params.object = (ULONG_PTR)face;
            params.simulations = params32->simulations;
            params.glyph = params32->glyph;
            params.mode = params32->mode;
            params.emsize = params32->emsize;
            params.m = params32->m;
            params.bbox = params32->bbox;
            params.pitch = params32->pitch;
            params.bitmap = bitmap;
            params.is_1bpp = &is_1bpp;
            if (params32->mode == DWRITE_RENDERING_MODE1_ALIASED)
                ret = freetype_get_aliased_glyph_bitmap(&params, glyph);
            else
                ret = freetype_get_aa_glyph_bitmap(&params, glyph);
            pFT_Done_Glyph(glyph);

            if (!ret)
            {
                ret = wow64_atomic_write_pair(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp),
                                               params32->bitmap, bitmap, bitmap_size);
            }
            free(bitmap);
        }
        else ret = wow64_atomic_write_guest(params32->is_1bpp, &is_1bpp, sizeof(is_1bpp));
        pFT_Done_Size(size);
        wow64_release_font_object_ref(object);
        return ret;
    }
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS wow64_get_design_glyph_metrics(void *args)
{
    const struct wow64_get_design_glyph_metrics_params *params32 = args;

#ifdef SONAME_LIBFREETYPE
    DWRITE_GLYPH_METRICS metrics;
    struct wow64_font_object *object;
    FT_Face face;
    FT_Size size;
    NTSTATUS status = STATUS_SUCCESS;
    BOOL loaded = FALSE;

    if ((status = wow64_acquire_font_object(params32->object, &object))) return status;
    face = object->face;
    if (!(size = freetype_set_face_size(face, params32->upem)))
    {
        wow64_release_font_object_ref(object);
        return STATUS_UNSUCCESSFUL;
    }

    if (!pFT_Load_Glyph(face, params32->glyph, FT_LOAD_NO_SCALE))
    {
        FT_Glyph_Metrics *ft_metrics = &face->glyph->metrics;

        metrics.leftSideBearing = ft_metrics->horiBearingX;
        metrics.advanceWidth = ft_metrics->horiAdvance;
        metrics.rightSideBearing = ft_metrics->horiAdvance - ft_metrics->horiBearingX - ft_metrics->width;
        metrics.advanceHeight = ft_metrics->vertAdvance;
        metrics.verticalOriginY = params32->ascent;
        metrics.topSideBearing = params32->ascent - ft_metrics->horiBearingY;
        metrics.bottomSideBearing = ft_metrics->vertAdvance - ft_metrics->height - metrics.topSideBearing;
        if (params32->simulations & DWRITE_FONT_SIMULATIONS_BOLD && freetype_glyph_has_contours(face) &&
            metrics.advanceWidth)
            metrics.advanceWidth += (params32->upem + 49) / 50;
        loaded = TRUE;
    }
    pFT_Done_Size(size);
    wow64_release_font_object_ref(object);
    if (loaded) status = wow64_atomic_write_guest(params32->metrics, &metrics, sizeof(metrics));
    return status;
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
};

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    process_attach,
    process_detach,
    wow64_create_font_object,
    wow64_release_font_object,
    wow64_get_glyph_outline,
    wow64_get_glyph_count,
    wow64_get_glyph_advance,
    wow64_get_glyph_bbox,
    wow64_get_glyph_bitmap,
    wow64_get_design_glyph_metrics,
};

static const struct wine_unixlib_dispatch_entry_v2 wow64_dispatch_entries[] =
{
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    { 0, WINE_UNIXLIB_DISPATCH_ENTRY_REVIEWED },
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_create_font_object_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_release_font_object_params, 0),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_get_glyph_outline_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_get_glyph_count_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_get_glyph_advance_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_get_glyph_bbox_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_get_glyph_bitmap_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
    WINE_UNIXLIB_DISPATCH_ARGS_V2(struct wow64_get_design_glyph_metrics_params,
                                  WINE_UNIXLIB_DISPATCH_ENTRY_NESTED |
                                  WINE_UNIXLIB_DISPATCH_ENTRY_HAS_OUTPUT),
};

WINE_UNIXLIB_DISPATCH_SOURCE_V2(__wine_unix_call_wow64_funcs, wow64_dispatch_entries);

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif  /* _WIN64 */
