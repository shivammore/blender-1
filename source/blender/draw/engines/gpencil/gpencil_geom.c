/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008, Blender Foundation
 * This is a new part of Blender
 *
 * Contributor(s): Antonio Vazquez
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file draw/engines/gpencil/gpencil_geom.c
 *  \ingroup draw
 */


#include "BLI_polyfill_2d.h"
#include "BLI_math_color.h"

#include "DNA_gpencil_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BKE_gpencil.h"
#include "BKE_action.h"

#include "DRW_render.h"

#include "GPU_immediate.h"
#include "GPU_draw.h"

#include "ED_gpencil.h"
#include "ED_view3d.h"

#include "UI_resources.h"

#include "gpencil_engine.h"

/* set stroke point to vbo */
static void gpencil_set_stroke_point(
        Gwn_VertBuf *vbo, float matrix[4][4], const bGPDspoint *pt, int idx,
        uint pos_id, uint color_id,
        uint thickness_id, uint uvdata_id, short thickness,
        const float ink[4])
{
	float viewfpt[3];

	float alpha = ink[3] * pt->strength;
	CLAMP(alpha, GPENCIL_STRENGTH_MIN, 1.0f);
	float col[4];
	ARRAY_SET_ITEMS(col, ink[0], ink[1], ink[2], alpha);

	GWN_vertbuf_attr_set(vbo, color_id, idx, col);

	/* transfer both values using the same shader variable */
	float uvdata[2] = { pt->uv_fac, pt->uv_rot };
	GWN_vertbuf_attr_set(vbo, uvdata_id, idx, uvdata);

	/* the thickness of the stroke must be affected by zoom, so a pixel scale is calculated */
	mul_v3_m4v3(viewfpt, matrix, &pt->x);
	float thick = max_ff(pt->pressure * thickness, 1.0f);
	GWN_vertbuf_attr_set(vbo, thickness_id, idx, &thick);
	
	GWN_vertbuf_attr_set(vbo, pos_id, idx, &pt->x);
}

/* create batch geometry data for points stroke shader */
Gwn_Batch *DRW_gpencil_get_point_geom(bGPDstroke *gps, short thickness, const float ink[4])
{
	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id, size_id, uvdata_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
		size_id = GWN_vertformat_attr_add(&format, "thickness", GWN_COMP_F32, 1, GWN_FETCH_FLOAT);
		uvdata_id = GWN_vertformat_attr_add(&format, "uvdata", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, gps->totpoints);

	/* draw stroke curve */
	const bGPDspoint *pt = gps->points;
	int idx = 0;
	float alpha;
	float col[4];

	for (int i = 0; i < gps->totpoints; i++, pt++) {
		/* set point */
		alpha = ink[3] * pt->strength;
		CLAMP(alpha, GPENCIL_STRENGTH_MIN, 1.0f);
		ARRAY_SET_ITEMS(col, ink[0], ink[1], ink[2], alpha);

		float thick = max_ff(pt->pressure * thickness, 1.0f);

		GWN_vertbuf_attr_set(vbo, color_id, idx, col);
		GWN_vertbuf_attr_set(vbo, size_id, idx, &thick);

		/* transfer both values using the same shader variable */
		float uvdata[2] = { pt->uv_fac, pt->uv_rot };
		GWN_vertbuf_attr_set(vbo, uvdata_id, idx, uvdata);

		GWN_vertbuf_attr_set(vbo, pos_id, idx, &pt->x);
		++idx;
	}

	return GWN_batch_create_ex(GWN_PRIM_POINTS, vbo, NULL, GWN_BATCH_OWNS_VBO);
}

/* create batch geometry data for stroke shader */
Gwn_Batch *DRW_gpencil_get_stroke_geom(bGPDframe *gpf, bGPDstroke *gps, short thickness, const float ink[4])
{
	bGPDspoint *points = gps->points;
	int totpoints = gps->totpoints;
	/* if cyclic needs more vertex */
	int cyclic_add = (gps->flag & GP_STROKE_CYCLIC) ? 1 : 0;

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id, thickness_id, uvdata_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
		thickness_id = GWN_vertformat_attr_add(&format, "thickness", GWN_COMP_F32, 1, GWN_FETCH_FLOAT);
		uvdata_id = GWN_vertformat_attr_add(&format, "uvdata", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, totpoints + cyclic_add + 2);

	/* draw stroke curve */
	const bGPDspoint *pt = points;
	int idx = 0;
	for (int i = 0; i < totpoints; i++, pt++) {
		/* first point for adjacency (not drawn) */
		if (i == 0) {
			if (gps->flag & GP_STROKE_CYCLIC && totpoints > 2) {
				gpencil_set_stroke_point(vbo, gpf->viewmatrix, &points[totpoints - 1], idx, 
										 pos_id, color_id, thickness_id, uvdata_id, thickness, ink);
				++idx;
			}
			else {
				gpencil_set_stroke_point(vbo, gpf->viewmatrix, &points[1], idx, 
										 pos_id, color_id, thickness_id, uvdata_id, thickness, ink);
				++idx;
			}
		}
		/* set point */
		gpencil_set_stroke_point(vbo, gpf->viewmatrix, pt, idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, ink);
		++idx;
	}

	if (gps->flag & GP_STROKE_CYCLIC && totpoints > 2) {
		/* draw line to first point to complete the cycle */
		gpencil_set_stroke_point(vbo, gpf->viewmatrix, &points[0], idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, ink);
		++idx;
		/* now add adjacency point (not drawn) */
		gpencil_set_stroke_point(vbo, gpf->viewmatrix, &points[1], idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, ink);
		++idx;
	}
	/* last adjacency point (not drawn) */
	else {
		gpencil_set_stroke_point(vbo, gpf->viewmatrix, &points[totpoints - 2], idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, ink);
	}

	return GWN_batch_create_ex(GWN_PRIM_LINE_STRIP_ADJ, vbo, NULL, GWN_BATCH_OWNS_VBO);
}

/* create batch geometry data for current buffer stroke shader */
Gwn_Batch *DRW_gpencil_get_buffer_stroke_geom(bGPdata *gpd, float matrix[4][4], short thickness)
{
	const DRWContextState *draw_ctx = DRW_context_state_get();
	Scene *scene = draw_ctx->scene;
	View3D *v3d = draw_ctx->v3d;
	ARegion *ar = draw_ctx->ar;
	RegionView3D *rv3d = draw_ctx->rv3d;
	ToolSettings *ts = scene->toolsettings;
	Object *ob = draw_ctx->obact;

	tGPspoint *points = gpd->sbuffer;
	int totpoints = gpd->sbuffer_size;

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id, thickness_id, uvdata_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
		thickness_id = GWN_vertformat_attr_add(&format, "thickness", GWN_COMP_F32, 1, GWN_FETCH_FLOAT);
		uvdata_id = GWN_vertformat_attr_add(&format, "uvdata", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, totpoints + 2);

	/* draw stroke curve */
	const tGPspoint *tpt = points;
	bGPDspoint pt, pt2;
	int idx = 0;
	
	/* get origin to reproject point */
	float origin[3];
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);
	ED_gp_get_drawing_reference(v3d, scene, ob, gpl, ts->gpencil_v3d_align, origin);

	for (int i = 0; i < totpoints; i++, tpt++) {
		ED_gpencil_tpoint_to_point(ar, origin, tpt, &pt);
		ED_gp_project_point_to_plane(ob, rv3d, origin, ts->gp_sculpt.lock_axis - 1, ts->gpencil_src, &pt);

		/* first point for adjacency (not drawn) */
		if (i == 0) {
			if (totpoints > 1) {
				ED_gpencil_tpoint_to_point(ar, origin, &points[1], &pt2);
				gpencil_set_stroke_point(vbo, matrix, &pt2, idx, 
										 pos_id, color_id, thickness_id, uvdata_id, thickness, gpd->scolor);
			}
			else {
				gpencil_set_stroke_point(vbo, matrix, &pt, idx, 
										 pos_id, color_id, thickness_id, uvdata_id, thickness, gpd->scolor);
			}
			idx++;
		}
		/* set point */
		gpencil_set_stroke_point(vbo, matrix, &pt, idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, gpd->scolor);
		idx++;
	}

	/* last adjacency point (not drawn) */
	if (totpoints > 2) {
		ED_gpencil_tpoint_to_point(ar, origin, &points[totpoints - 2], &pt2);
		gpencil_set_stroke_point(vbo, matrix, &pt2, idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, gpd->scolor);
	}
	else {
		gpencil_set_stroke_point(vbo, matrix, &pt, idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, gpd->scolor);
	}

	return GWN_batch_create_ex(GWN_PRIM_LINE_STRIP_ADJ, vbo, NULL, GWN_BATCH_OWNS_VBO);
}

/* create batch geometry data for current buffer point shader */
Gwn_Batch *DRW_gpencil_get_buffer_point_geom(bGPdata *gpd, float matrix[4][4], short thickness)
{
	const DRWContextState *draw_ctx = DRW_context_state_get();
	Scene *scene = draw_ctx->scene;
	View3D *v3d = draw_ctx->v3d;
	ARegion *ar = draw_ctx->ar;
	RegionView3D *rv3d = draw_ctx->rv3d;
	ToolSettings *ts = scene->toolsettings;
	Object *ob = draw_ctx->obact;

	tGPspoint *points = gpd->sbuffer;
	int totpoints = gpd->sbuffer_size;

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id, thickness_id, uvdata_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
		thickness_id = GWN_vertformat_attr_add(&format, "thickness", GWN_COMP_F32, 1, GWN_FETCH_FLOAT);
		uvdata_id = GWN_vertformat_attr_add(&format, "uvdata", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, totpoints);

	/* draw stroke curve */
	const tGPspoint *tpt = points;
	bGPDspoint pt;
	int idx = 0;

	/* get origin to reproject point */
	float origin[3];
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);
	ED_gp_get_drawing_reference(v3d, scene, ob, gpl, ts->gpencil_v3d_align, origin);

	for (int i = 0; i < totpoints; i++, tpt++) {
		ED_gpencil_tpoint_to_point(ar, origin, tpt, &pt);
		ED_gp_project_point_to_plane(ob, rv3d, origin, ts->gp_sculpt.lock_axis - 1, ts->gpencil_src, &pt);

		/* set point */
		gpencil_set_stroke_point(vbo, matrix, &pt, idx, 
								 pos_id, color_id, thickness_id, uvdata_id, thickness, gpd->scolor);
		++idx;
	}

	return GWN_batch_create_ex(GWN_PRIM_POINTS, vbo, NULL, GWN_BATCH_OWNS_VBO);
}

/* create batch geometry data for current buffer fill shader */
Gwn_Batch *DRW_gpencil_get_buffer_fill_geom(bGPdata *gpd)
{
	if (gpd == NULL) {
		return NULL;
	}

	const tGPspoint *points = gpd->sbuffer;
	int totpoints = gpd->sbuffer_size;
	if (totpoints < 3) {
		return NULL;
	}

	const DRWContextState *draw_ctx = DRW_context_state_get();
	Scene *scene = draw_ctx->scene;
	View3D *v3d = draw_ctx->v3d;
	ARegion *ar = draw_ctx->ar;
	RegionView3D *rv3d = draw_ctx->rv3d;
	ToolSettings *ts = scene->toolsettings;
	Object *ob = draw_ctx->obact;

	/* get origin to reproject point */
	float origin[3];
	bGPDlayer *gpl = BKE_gpencil_layer_getactive(gpd);
	ED_gp_get_drawing_reference(v3d, scene, ob, gpl, ts->gpencil_v3d_align, origin);

	int tot_triangles = totpoints - 2;
	/* allocate memory for temporary areas */
	uint (*tmp_triangles)[3] = MEM_mallocN(sizeof(*tmp_triangles) * tot_triangles, __func__);
	float (*points2d)[2] = MEM_mallocN(sizeof(*points2d) * totpoints, __func__);

	/* Convert points to array and triangulate
	* Here a cache is not used because while drawing the information changes all the time, so the cache
	* would be recalculated constantly, so it is better to do direct calculation for each function call
	*/
	for (int i = 0; i < totpoints; i++) {
		const tGPspoint *pt = &points[i];
		points2d[i][0] = pt->x;
		points2d[i][1] = pt->y;
	}
	BLI_polyfill_calc(points2d, (uint)totpoints, 0, tmp_triangles);

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);

	/* draw triangulation data */
	if (tot_triangles > 0) {
		GWN_vertbuf_data_alloc(vbo, tot_triangles * 3);

		const tGPspoint *tpt;
		bGPDspoint pt;

		int idx = 0;
		for (int i = 0; i < tot_triangles; i++) {
			for (int j = 0; j < 3; j++) {
				tpt = &points[tmp_triangles[i][j]];
				ED_gpencil_tpoint_to_point(ar, origin, tpt, &pt);
				GWN_vertbuf_attr_set(vbo, pos_id, idx, &pt.x);
				GWN_vertbuf_attr_set(vbo, color_id, idx, gpd->sfill);
				idx++;
			}
		}
	}

	/* clear memory */
	if (tmp_triangles) {
		MEM_freeN(tmp_triangles);
	}
	if (points2d) {
		MEM_freeN(points2d);
	}

	return GWN_batch_create_ex(GWN_PRIM_TRIS, vbo, NULL, GWN_BATCH_OWNS_VBO);
}


/* Helper for doing all the checks on whether a stroke can be drawn */
bool gpencil_can_draw_stroke(const bGPDstroke *gps, const bool onion)
{
	/* skip stroke if it doesn't have any valid data */
	if ((gps->points == NULL) || (gps->totpoints < 1))
		return false;

	/* check if the color is visible */
	PaletteColor *palcolor = gps->palcolor;
	if ((gps->palette == NULL) || (palcolor == NULL) ||
	    (palcolor->flag & PC_COLOR_HIDE) ||
	    (onion && (palcolor->flag & PC_COLOR_ONIONSKIN)))
	{
		return false;
	}

	/* stroke can be drawn */
	return true;
}

/* calc bounding box in 2d using flat projection data */
static void gpencil_calc_2d_bounding_box(
        const float(*points2d)[2], int totpoints, float minv[2], float maxv[2], bool expand)
{
	minv[0] = points2d[0][0];
	minv[1] = points2d[0][1];
	maxv[0] = points2d[0][0];
	maxv[1] = points2d[0][1];

	for (int i = 1; i < totpoints; i++) {
		/* min */
		if (points2d[i][0] < minv[0]) {
			minv[0] = points2d[i][0];
		}
		if (points2d[i][1] < minv[1]) {
			minv[1] = points2d[i][1];
		}
		/* max */
		if (points2d[i][0] > maxv[0]) {
			maxv[0] = points2d[i][0];
		}
		if (points2d[i][1] > maxv[1]) {
			maxv[1] = points2d[i][1];
		}
	}
	/* If not expanded, use a perfect square */
	if (expand == false) {
		if (maxv[0] > maxv[1]) {
			maxv[1] = maxv[0];
		}
		else {
			maxv[0] = maxv[1];
		}
	}
}

/* calc texture coordinates using flat projected points */
static void gpencil_calc_stroke_fill_uv(const float(*points2d)[2], int totpoints, float minv[2], float maxv[2], float(*r_uv)[2])
{
	float d[2];
	d[0] = maxv[0] - minv[0];
	d[1] = maxv[1] - minv[1];
	for (int i = 0; i < totpoints; i++) {
		r_uv[i][0] = (points2d[i][0] - minv[0]) / d[0];
		r_uv[i][1] = (points2d[i][1] - minv[1]) / d[1];
	}
}

/* Get points of stroke always flat to view not affected by camera view or view position */
static void gpencil_stroke_2d_flat(const bGPDspoint *points, int totpoints, float(*points2d)[2], int *r_direction)
{
	const bGPDspoint *pt0 = &points[0];
	const bGPDspoint *pt1 = &points[1];
	const bGPDspoint *pt3 = &points[(int)(totpoints * 0.75)];

	float locx[3];
	float locy[3];
	float loc3[3];
	float normal[3];

	/* local X axis (p0 -> p1) */
	sub_v3_v3v3(locx, &pt1->x, &pt0->x);

	/* point vector at 3/4 */
	sub_v3_v3v3(loc3, &pt3->x, &pt0->x);

	/* vector orthogonal to polygon plane */
	cross_v3_v3v3(normal, locx, loc3);

	/* local Y axis (cross to normal/x axis) */
	cross_v3_v3v3(locy, normal, locx);

	/* Normalize vectors */
	normalize_v3(locx);
	normalize_v3(locy);

	/* Get all points in local space */
	for (int i = 0; i < totpoints; i++) {
		const bGPDspoint *pt = &points[i];
		float loc[3];

		/* Get local space using first point as origin */
		sub_v3_v3v3(loc, &pt->x, &pt0->x);

		points2d[i][0] = dot_v3v3(loc, locx);
		points2d[i][1] = dot_v3v3(loc, locy);
	}

	/* Concave (-1), Convex (1), or Autodetect (0)? */
	*r_direction = (int)locy[2];
}

/* Triangulate stroke for high quality fill (this is done only if cache is null or stroke was modified) */
static void gp_triangulate_stroke_fill(bGPDstroke *gps)
{
	BLI_assert(gps->totpoints >= 3);

	/* allocate memory for temporary areas */
	gps->tot_triangles = gps->totpoints - 2;
	uint(*tmp_triangles)[3] = MEM_mallocN(sizeof(*tmp_triangles) * gps->tot_triangles, "GP Stroke temp triangulation");
	float(*points2d)[2] = MEM_mallocN(sizeof(*points2d) * gps->totpoints, "GP Stroke temp 2d points");
	float(*uv)[2] = MEM_mallocN(sizeof(*uv) * gps->totpoints, "GP Stroke temp 2d uv data");

	int direction = 0;

	/* convert to 2d and triangulate */
	gpencil_stroke_2d_flat(gps->points, gps->totpoints, points2d, &direction);
	BLI_polyfill_calc(points2d, (uint)gps->totpoints, direction, tmp_triangles);

	/* calc texture coordinates automatically */
	float minv[2];
	float maxv[2];
	/* first needs bounding box data */
	gpencil_calc_2d_bounding_box(points2d, gps->totpoints, minv, maxv, false);
	/* calc uv data */
	gpencil_calc_stroke_fill_uv(points2d, gps->totpoints, minv, maxv, uv);

	/* Number of triangles */
	gps->tot_triangles = gps->totpoints - 2;
	/* save triangulation data in stroke cache */
	if (gps->tot_triangles > 0) {
		if (gps->triangles == NULL) {
			gps->triangles = MEM_callocN(sizeof(*gps->triangles) * gps->tot_triangles, "GP Stroke triangulation");
		}
		else {
			gps->triangles = MEM_recallocN(gps->triangles, sizeof(*gps->triangles) * gps->tot_triangles);
		}

		for (int i = 0; i < gps->tot_triangles; i++) {
			bGPDtriangle *stroke_triangle = &gps->triangles[i];
			memcpy(gps->triangles[i].verts, tmp_triangles[i], sizeof(uint[3]));
			/* copy texture coordinates */
			copy_v2_v2(stroke_triangle->uv[0], uv[tmp_triangles[i][0]]);
			copy_v2_v2(stroke_triangle->uv[1], uv[tmp_triangles[i][1]]);
			copy_v2_v2(stroke_triangle->uv[2], uv[tmp_triangles[i][2]]);
		}
	}
	else {
		/* No triangles needed - Free anything allocated previously */
		if (gps->triangles)
			MEM_freeN(gps->triangles);

		gps->triangles = NULL;
	}

	/* disable recalculation flag */
	if (gps->flag & GP_STROKE_RECALC_CACHES) {
		gps->flag &= ~GP_STROKE_RECALC_CACHES;
	}

	/* clear memory */
	MEM_SAFE_FREE(tmp_triangles);
	MEM_SAFE_FREE(points2d);
	MEM_SAFE_FREE(uv);
}

/* add a new fill point and texture coordinates to vertex buffer */
static void gpencil_set_fill_point(
        Gwn_VertBuf *vbo, int idx, bGPDspoint *pt, const float fcolor[4], float uv[2],
        uint pos_id, uint color_id, uint text_id)
{
	GWN_vertbuf_attr_set(vbo, pos_id, idx, &pt->x);
	GWN_vertbuf_attr_set(vbo, color_id, idx, fcolor);
	GWN_vertbuf_attr_set(vbo, text_id, idx, uv);
}

/* recalc the internal geometry caches for fill and uvs */
void DRW_gpencil_recalc_geometry_caches(bGPDstroke *gps) {
	if (gps->flag & GP_STROKE_RECALC_CACHES) {
		/* Calculate triangles cache for filling area (must be done only after changes) */
		if ((gps->tot_triangles == 0) || (gps->triangles == NULL)) {
			if ((gps->totpoints > 2) && 
				((gps->palcolor->fill[3] > GPENCIL_ALPHA_OPACITY_THRESH) || (gps->palcolor->fill_style > 0))) 
			{
				gp_triangulate_stroke_fill(gps);
			}
		}

		/* calc uv data along the stroke */
		ED_gpencil_calc_stroke_uv(gps);
		
		/* clear flag */
		gps->flag &= ~GP_STROKE_RECALC_CACHES;
	}
}

/* create batch geometry data for stroke shader */
Gwn_Batch *DRW_gpencil_get_fill_geom(bGPDstroke *gps, const float color[4])
{
	BLI_assert(gps->totpoints >= 3);

	/* Calculate triangles cache for filling area (must be done only after changes) */
	if ((gps->flag & GP_STROKE_RECALC_CACHES) || (gps->tot_triangles == 0) || (gps->triangles == NULL)) {
		gp_triangulate_stroke_fill(gps);
		ED_gpencil_calc_stroke_uv(gps);
	}

	BLI_assert(gps->tot_triangles >= 1);

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id, text_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
		text_id = GWN_vertformat_attr_add(&format, "texCoord", GWN_COMP_F32, 2, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, gps->tot_triangles * 3);

	/* Draw all triangles for filling the polygon (cache must be calculated before) */
	bGPDtriangle *stroke_triangle = gps->triangles;
	int idx = 0;
	for (int i = 0; i < gps->tot_triangles; i++, stroke_triangle++) {
		for (int j = 0; j < 3; j++) {
			gpencil_set_fill_point(
			        vbo, idx, &gps->points[stroke_triangle->verts[j]], color, stroke_triangle->uv[j],
			        pos_id, color_id, text_id);
			++idx;
		}
	}

	return GWN_batch_create_ex(GWN_PRIM_TRIS, vbo, NULL, GWN_BATCH_OWNS_VBO);
}

/* Draw selected verts for strokes being edited */
Gwn_Batch *DRW_gpencil_get_edit_geom(bGPDstroke *gps, float alpha, short dflag)
{
	const DRWContextState *draw_ctx = DRW_context_state_get();
	Object *ob = draw_ctx->obact;
	bGPdata *gpd = ob->data;
	bool is_weight_paint = (gpd) && (gpd->flag & GP_DATA_STROKE_WEIGHTMODE);

	int vgindex = ob->actdef - 1;
	if (!BLI_findlink(&ob->defbase, vgindex)) {
		vgindex = -1;
	}

	/* Get size of verts:
	* - The selected state needs to be larger than the unselected state so that
	*   they stand out more.
	* - We use the theme setting for size of the unselected verts
	*/
	float bsize = UI_GetThemeValuef(TH_GP_VERTEX_SIZE);
	float vsize;
	if ((int)bsize > 8) {
		vsize = 10.0f;
		bsize = 8.0f;
	}
	else {
		vsize = bsize + 2;
	}

	/* for now, we assume that the base color of the points is not too close to the real color */
	/* set color using palette */
	PaletteColor *palcolor = gps->palcolor;

	float selectColor[4];
	UI_GetThemeColor3fv(TH_GP_VERTEX_SELECT, selectColor);
	selectColor[3] = alpha;

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id, size_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
		size_id = GWN_vertformat_attr_add(&format, "size", GWN_COMP_F32, 1, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, gps->totpoints);

	/* Draw start and end point differently if enabled stroke direction hint */
	bool show_direction_hint = (dflag & GP_DATA_SHOW_DIRECTION) && (gps->totpoints > 1);

	/* Draw all the stroke points (selected or not) */
	bGPDspoint *pt = gps->points;
	int idx = 0;
	float fcolor[4];
	float fsize = 0;
	for (int i = 0; i < gps->totpoints; i++, pt++) {
		/* weight paint */
		if (is_weight_paint) {
			float weight = BKE_gpencil_vgroup_use_index(pt, vgindex);
			CLAMP(weight, 0.0f, 1.0f);
			float hue = 2.0f * (1.0f - weight) / 3.0f;
			hsv_to_rgb(hue, 1.0f, 1.0f, &selectColor[0], &selectColor[1], &selectColor[2]);
			selectColor[3] = 1.0f;
			copy_v4_v4(fcolor, selectColor);
			fsize = vsize;
		}
		else {
			if (show_direction_hint && i == 0) {
				/* start point in green bigger */
				ARRAY_SET_ITEMS(fcolor, 0.0f, 1.0f, 0.0f, 1.0f);
				fsize = vsize + 4;
			}
			else if (show_direction_hint && (i == gps->totpoints - 1)) {
				/* end point in red smaller */
				ARRAY_SET_ITEMS(fcolor, 1.0f, 0.0f, 0.0f, 1.0f);
				fsize = vsize + 1;
			}
			else if (pt->flag & GP_SPOINT_SELECT) {
				copy_v4_v4(fcolor, selectColor);
				fsize = vsize;
			}
			else {
				copy_v4_v4(fcolor, palcolor->rgb);
				fsize = bsize;
			}
		}

		GWN_vertbuf_attr_set(vbo, color_id, idx, fcolor);
		GWN_vertbuf_attr_set(vbo, size_id, idx, &fsize);
		GWN_vertbuf_attr_set(vbo, pos_id, idx, &pt->x);
		++idx;
	}

	return GWN_batch_create_ex(GWN_PRIM_POINTS, vbo, NULL, GWN_BATCH_OWNS_VBO);
}

/* Draw lines for strokes being edited */
Gwn_Batch *DRW_gpencil_get_edlin_geom(bGPDstroke *gps, float alpha, short UNUSED(dflag))
{
	const DRWContextState *draw_ctx = DRW_context_state_get();
	Object *ob = draw_ctx->obact;
	bGPdata *gpd = ob->data;
	bool is_weight_paint = (gpd) && (gpd->flag & GP_DATA_STROKE_WEIGHTMODE);

	int vgindex = ob->actdef - 1;
	if (!BLI_findlink(&ob->defbase, vgindex)) {
		vgindex = -1;
	}

	float selectColor[4];
	UI_GetThemeColor3fv(TH_GP_VERTEX_SELECT, selectColor);
	selectColor[3] = alpha;
	float linecolor[4];
	copy_v4_v4(linecolor, gpd->line_color);

	static Gwn_VertFormat format = { 0 };
	static uint pos_id, color_id;
	if (format.attrib_ct == 0) {
		pos_id = GWN_vertformat_attr_add(&format, "pos", GWN_COMP_F32, 3, GWN_FETCH_FLOAT);
		color_id = GWN_vertformat_attr_add(&format, "color", GWN_COMP_F32, 4, GWN_FETCH_FLOAT);
	}

	Gwn_VertBuf *vbo = GWN_vertbuf_create_with_format(&format);
	GWN_vertbuf_data_alloc(vbo, gps->totpoints);

	/* Draw all the stroke lines (selected or not) */
	bGPDspoint *pt = gps->points;
	int idx = 0;
	float fcolor[4];
	for (int i = 0; i < gps->totpoints; i++, pt++) {
		/* weight paint */
		if (is_weight_paint) {
			float weight = BKE_gpencil_vgroup_use_index(pt, vgindex);
			CLAMP(weight, 0.0f, 1.0f);
			float hue = 2.0f * (1.0f - weight) / 3.0f;
			hsv_to_rgb(hue, 1.0f, 1.0f, &selectColor[0], &selectColor[1], &selectColor[2]);
			selectColor[3] = 1.0f;
			copy_v4_v4(fcolor, selectColor);
		}
		else {
			if (pt->flag & GP_SPOINT_SELECT) {
				copy_v4_v4(fcolor, selectColor);
			}
			else {
				copy_v4_v4(fcolor, linecolor);
			}
		}

		GWN_vertbuf_attr_set(vbo, color_id, idx, fcolor);
		GWN_vertbuf_attr_set(vbo, pos_id, idx, &pt->x);
		++idx;
	}

	return GWN_batch_create_ex(GWN_PRIM_LINE_STRIP, vbo, NULL, GWN_BATCH_OWNS_VBO);
}
