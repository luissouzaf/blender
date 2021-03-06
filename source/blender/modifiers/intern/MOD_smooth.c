/*
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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup modifiers
 */


#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_editmesh.h"
#include "BKE_library.h"
#include "BKE_mesh.h"
#include "BKE_particle.h"
#include "BKE_deform.h"

#include "MOD_modifiertypes.h"
#include "MOD_util.h"


static void initData(ModifierData *md)
{
	SmoothModifierData *smd = (SmoothModifierData *) md;

	smd->fac = 0.5f;
	smd->repeat = 1;
	smd->flag = MOD_SMOOTH_X | MOD_SMOOTH_Y | MOD_SMOOTH_Z;
	smd->defgrp_name[0] = '\0';
}

static bool isDisabled(const struct Scene *UNUSED(scene), ModifierData *md, bool UNUSED(useRenderParams))
{
	SmoothModifierData *smd = (SmoothModifierData *) md;
	short flag;

	flag = smd->flag & (MOD_SMOOTH_X | MOD_SMOOTH_Y | MOD_SMOOTH_Z);

	/* disable if modifier is off for X, Y and Z or if factor is 0 */
	if ((smd->fac == 0.0f) || flag == 0) return 1;

	return 0;
}

static void requiredDataMask(Object *UNUSED(ob), ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
	SmoothModifierData *smd = (SmoothModifierData *)md;

	/* ask for vertexgroups if we need them */
	if (smd->defgrp_name[0] != '\0') {
		r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
	}
}

static void smoothModifier_do(
        SmoothModifierData *smd, Object *ob, Mesh *mesh,
        float (*vertexCos)[3], int numVerts)
{
	MDeformVert *dvert = NULL;
	MEdge *medges = NULL;

	int i, j, numDMEdges, defgrp_index;
	unsigned char *uctmp;
	float *ftmp, fac, facm;

	ftmp = (float *)MEM_calloc_arrayN(numVerts, 3 * sizeof(float),
	                            "smoothmodifier_f");
	if (!ftmp) return;
	uctmp = (unsigned char *)MEM_calloc_arrayN(numVerts, sizeof(unsigned char),
	                                     "smoothmodifier_uc");
	if (!uctmp) {
		if (ftmp) MEM_freeN(ftmp);
		return;
	}

	fac = smd->fac;
	facm = 1 - fac;

	if (mesh != NULL) {
		medges = mesh->medge;
		numDMEdges = mesh->totedge;
	}
	else {
		medges = NULL;
		numDMEdges = 0;
	}

	MOD_get_vgroup(ob, mesh, smd->defgrp_name, &dvert, &defgrp_index);

	/* NOTICE: this can be optimized a little bit by moving the
	 * if (dvert) out of the loop, if needed */
	for (j = 0; j < smd->repeat; j++) {
		for (i = 0; i < numDMEdges; i++) {
			float fvec[3];
			float *v1, *v2;
			unsigned int idx1, idx2;

			idx1 = medges[i].v1;
			idx2 = medges[i].v2;

			v1 = vertexCos[idx1];
			v2 = vertexCos[idx2];

			mid_v3_v3v3(fvec, v1, v2);

			v1 = &ftmp[idx1 * 3];
			v2 = &ftmp[idx2 * 3];

			if (uctmp[idx1] < 255) {
				uctmp[idx1]++;
				add_v3_v3(v1, fvec);
			}
			if (uctmp[idx2] < 255) {
				uctmp[idx2]++;
				add_v3_v3(v2, fvec);
			}
		}

		if (dvert) {
			MDeformVert *dv = dvert;
			for (i = 0; i < numVerts; i++, dv++) {
				float f, fm, facw, *fp, *v;
				short flag = smd->flag;

				v = vertexCos[i];
				fp = &ftmp[i * 3];


				f = defvert_find_weight(dv, defgrp_index);
				if (f <= 0.0f) continue;

				f *= fac;
				fm = 1.0f - f;

				/* fp is the sum of uctmp[i] verts, so must be averaged */
				facw = 0.0f;
				if (uctmp[i])
					facw = f / (float)uctmp[i];

				if (flag & MOD_SMOOTH_X)
					v[0] = fm * v[0] + facw * fp[0];
				if (flag & MOD_SMOOTH_Y)
					v[1] = fm * v[1] + facw * fp[1];
				if (flag & MOD_SMOOTH_Z)
					v[2] = fm * v[2] + facw * fp[2];
			}
		}
		else { /* no vertex group */
			for (i = 0; i < numVerts; i++) {
				float facw, *fp, *v;
				short flag = smd->flag;

				v = vertexCos[i];
				fp = &ftmp[i * 3];

				/* fp is the sum of uctmp[i] verts, so must be averaged */
				facw = 0.0f;
				if (uctmp[i])
					facw = fac / (float)uctmp[i];

				if (flag & MOD_SMOOTH_X)
					v[0] = facm * v[0] + facw * fp[0];
				if (flag & MOD_SMOOTH_Y)
					v[1] = facm * v[1] + facw * fp[1];
				if (flag & MOD_SMOOTH_Z)
					v[2] = facm * v[2] + facw * fp[2];
			}

		}

		memset(ftmp, 0, 3 * sizeof(float) * numVerts);
		memset(uctmp, 0, sizeof(unsigned char) * numVerts);
	}

	MEM_freeN(ftmp);
	MEM_freeN(uctmp);
}

static void deformVerts(
        ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh,
        float (*vertexCos)[3], int numVerts)
{
	SmoothModifierData *smd = (SmoothModifierData *)md;
	Mesh *mesh_src = NULL;

	/* mesh_src is needed for vgroups, and taking edges into account. */
	mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, numVerts, false, false);

	smoothModifier_do(smd, ctx->object, mesh_src, vertexCos, numVerts);

	if (!ELEM(mesh_src, NULL, mesh)) {
		BKE_id_free(NULL, mesh_src);
	}
}

static void deformVertsEM(
        ModifierData *md, const ModifierEvalContext *ctx, struct BMEditMesh *editData,
        Mesh *mesh, float (*vertexCos)[3], int numVerts)
{
	SmoothModifierData *smd = (SmoothModifierData *)md;
	Mesh *mesh_src = NULL;

	/* mesh_src is needed for vgroups, and taking edges into account. */
	mesh_src = MOD_deform_mesh_eval_get(ctx->object, editData, mesh, NULL, numVerts, false, false);

	smoothModifier_do(smd, ctx->object, mesh_src, vertexCos, numVerts);

	if (!ELEM(mesh_src, NULL, mesh)) {
		BKE_id_free(NULL, mesh_src);
	}
}


ModifierTypeInfo modifierType_Smooth = {
	/* name */              "Smooth",
	/* structName */        "SmoothModifierData",
	/* structSize */        sizeof(SmoothModifierData),
	/* type */              eModifierTypeType_OnlyDeform,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_AcceptsCVs |
	                        eModifierTypeFlag_SupportsEditmode,

	/* copyData */          modifier_copyData_generic,

	/* deformVerts_DM */    NULL,
	/* deformMatrices_DM */ NULL,
	/* deformVertsEM_DM */  NULL,
	/* deformMatricesEM_DM*/NULL,
	/* applyModifier_DM */  NULL,

	/* deformVerts */       deformVerts,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     deformVertsEM,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     NULL,

	/* initData */          initData,
	/* requiredDataMask */  requiredDataMask,
	/* freeData */          NULL,
	/* isDisabled */        isDisabled,
	/* updateDepsgraph */   NULL,
	/* dependsOnTime */     NULL,
	/* dependsOnNormals */	NULL,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
	/* freeRuntimeData */   NULL,
};
