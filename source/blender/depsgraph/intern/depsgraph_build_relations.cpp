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
 * The Original Code is Copyright (C) 2013 Blender Foundation.
 * All rights reserved.
 *
 * Original Author: Joshua Leung
 * Contributor(s): Based on original depsgraph.c code - Blender Foundation (2005-2013)
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Methods for constructing depsgraph
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

extern "C" {
#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_camera_types.h"
#include "DNA_constraint_types.h"
#include "DNA_curve_types.h"
#include "DNA_effect_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_group_types.h"
#include "DNA_key_types.h"
#include "DNA_lamp_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_node_types.h"
#include "DNA_particle_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"
#include "DNA_world_types.h"

#include "BKE_action.h"
#include "BKE_armature.h"
#include "BKE_animsys.h"
#include "BKE_constraint.h"
#include "BKE_curve.h"
#include "BKE_effect.h"
#include "BKE_fcurve.h"
#include "BKE_group.h"
#include "BKE_key.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_mball.h"
#include "BKE_modifier.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_rigidbody.h"
#include "BKE_sound.h"
#include "BKE_texture.h"
#include "BKE_tracking.h"
#include "BKE_world.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

#include "RNA_access.h"
#include "RNA_types.h"
} /* extern "C" */

#include "depsnode.h"
#include "depsnode_component.h"
#include "depsnode_operation.h"
#include "depsgraph_build.h"
#include "depsgraph_debug.h"
#include "depsgraph_eval.h"
#include "depsgraph_intern.h"
#include "depsgraph_types.h"

#include "stubs.h" // XXX: REMOVE THIS INCLUDE ONCE DEPSGRAPH REFACTOR PROJECT IS DONE!!!

namespace {

/* TODO(sergey): This is a stupid copy of function from depsgraph.c/ */
bool modifier_check_depends_on_time(Object *ob, ModifierData *md)
{
	if (modifier_dependsOnTime(md)) {
		return true;
	}

	/* Check whether modifier is animated. */
	// TODO: this should be handled as part of build_animdata()  -- Aligorith
	if (ob->adt) {
		AnimData *adt = ob->adt;
		FCurve *fcu;

		char pattern[MAX_NAME + 10];
		/* TODO(sergey): Escape modifier name. */
		BLI_snprintf(pattern, sizeof(pattern), "modifiers[%s", md->name);

		/* action - check for F-Curves with paths containing 'modifiers[' */
		if (adt->action) {
			for (fcu = (FCurve *)adt->action->curves.first;
			     fcu != NULL;
			     fcu = (FCurve *)fcu->next)
			{
				if (fcu->rna_path && strstr(fcu->rna_path, pattern))
					return true;
			}
		}

		/* This here allows modifier properties to get driven and still update properly
		 *
		 * Workaround to get [#26764] (e.g. subsurf levels not updating when animated/driven)
		 * working, without the updating problems ([#28525] [#28690] [#28774] [#28777]) caused
		 * by the RNA updates cache introduced in r.38649
		 */
		for (fcu = (FCurve *)adt->drivers.first;
		     fcu != NULL;
		     fcu = (FCurve *)fcu->next)
		{
			if (fcu->rna_path && strstr(fcu->rna_path, pattern))
				return true;
		}

		/* XXX: also, should check NLA strips, though for now assume that nobody uses
		 * that and we can omit that for performance reasons... */
	}

	return false;
}

}  /* namespace */

/* ***************** */
/* Pose Channels "Root" Map */

DepsgraphRelationBuilder::RootPChanMap::RootPChanMap()
{
	/* just create empty map */
	m_map = BLI_ghash_str_new("RootPChanMap");
}

static void free_rootpchanmap_valueset(void *val)
{
	/* just need to free the set itself - the names stored are all references */
	GSet *values = (GSet *)val;
	BLI_gset_free(values, NULL);
}

DepsgraphRelationBuilder::RootPChanMap::~RootPChanMap()
{
	/* free the map, and all the value sets */
	BLI_ghash_free(m_map, NULL, free_rootpchanmap_valueset);
}

/* Debug contents of map */
void DepsgraphRelationBuilder::RootPChanMap::print_debug()
{
	GHashIterator it1;
	GSetIterator it2;
	
	printf("Root PChan Map:\n");
	GHASH_ITER(it1, m_map) {
		const char *item = (const char *)BLI_ghashIterator_getKey(&it1);
		GSet *values = (GSet *)BLI_ghashIterator_getValue(&it1);

		printf("  %s : { ", item);
		GSET_ITER(it2, values) {
			const char *val = (const char *)BLI_gsetIterator_getKey(&it2);
			printf("%s, ", val);
		}
		printf("}\n");
	}
}

/* Add a mapping */
void DepsgraphRelationBuilder::RootPChanMap::add_bone(const char *bone, const char *root)
{
	if (BLI_ghash_haskey(m_map, bone)) {
		/* add new entry */
		GSet *values = (GSet *)BLI_ghash_lookup(m_map, bone);
		BLI_gset_insert(values, (void *)root);
	}
	else {
		/* create new set and mapping */
		GSet *values = BLI_gset_new(BLI_ghashutil_strhash_p, BLI_ghashutil_strcmp, "RootPChanMap Value Set");
		BLI_ghash_insert(m_map, (void *)bone, (void *)values);

		/* add new entry now */
		BLI_gset_insert(values, (void *)root);
	}
}

/* Check if there's a common root bone between two bones */
bool DepsgraphRelationBuilder::RootPChanMap::has_common_root(const char *bone1, const char *bone2)
{
	/* Ensure that both are in the map... */
	if (BLI_ghash_haskey(m_map, bone1) == false) {
		//fprintf("RootPChanMap: bone1 '%s' not found (%s => %s)\n", bone1, bone1, bone2);
		print_debug();
		return false;
	}

	if (BLI_ghash_haskey(m_map, bone2) == false) {
		//fprintf("RootPChanMap: bone2 '%s' not found (%s => %s)\n", bone2, bone1, bone2);
		print_debug();
		return false;
	}

	GSet *bone1_roots = (GSet *)BLI_ghash_lookup(m_map, (void *)bone1);
	GSet *bone2_roots = (GSet *)BLI_ghash_lookup(m_map, (void *)bone2);

	GSetIterator it1, it2;
	GSET_ITER(it1, bone1_roots) {
		GSET_ITER(it2, bone2_roots) {
			const char *v1 = (const char *)BLI_gsetIterator_getKey(&it1);
			const char *v2 = (const char *)BLI_gsetIterator_getKey(&it2);

			if (strcmp(v1, v2) == 0) {
				//fprintf("RootPchanMap: %s in common for %s => %s\n", v1, bone1, bone2);
				return true;
			}
		}
	}

	//fprintf("RootPChanMap: No common root found (%s => %s)\n", bone1, bone2);
	return false;
}


/* ***************** */
/* Relations Builder */

void DepsgraphRelationBuilder::build_scene(Scene *scene)
{
	if (scene->set) {
		// TODO: link set to scene, especially our timesource...
	}
	
	/* scene objects */
	for (Base *base = (Base *)scene->base.first; base; base = base->next) {
		Object *ob = base->object;
		
		/* object itself */
		build_object(scene, ob);
		
#if 0
		/* object that this is a proxy for */
		// XXX: the way that proxies work needs to be completely reviewed!
		if (ob->proxy) {
			build_object(scene, ob->proxy);
		}
#endif
		
#if 0
		/* handled in next loop... 
		 * NOTE: in most cases, setting dupli-group means that we may want
		 *       to instance existing data and/or reuse it with very few
		 *       modifications...
		 */
		if (ob->dup_group) {
			id_tag_set(ob->dup_group);
		}
#endif
	}
	
#if 0
	/* tagged groups */
	for (Group *group = (Group *)m_bmain->group.first; group; group = (Group *)group->id.next) {
		if (is_id_tagged(group)) {
			// TODO: we need to make this group reliant on the object that spawned it...
			build_subgraph_nodes(group);
			
			id_tag_clear(group);
		}
	}
#endif

	/* rigidbody */
	if (scene->rigidbody_world) {
		build_rigidbody(scene);
	}
	
	/* scene's animation and drivers */
	if (scene->adt) {
		build_animdata(&scene->id);
	}
	
	/* world */
	if (scene->world) {
		build_world(scene, scene->world);
	}
	
	/* compo nodes */
	if (scene->nodetree) {
		build_compositor(scene);
	}
	
	/* grease pencil */
	if (scene->gpd) {
		build_gpencil(&scene->id, scene->gpd);
	}
}

void DepsgraphRelationBuilder::build_object(Scene *scene, Object *ob)
{
	/* Object Transforms */
	eDepsOperation_Code base_op = (ob->parent) ? DEG_OPCODE_TRANSFORM_PARENT : DEG_OPCODE_TRANSFORM_LOCAL;
	OperationKey base_op_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, base_op);
	
	OperationKey local_transform_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_LOCAL);
	OperationKey parent_transform_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_PARENT);
	OperationKey final_transform_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_FINAL);
	
	OperationKey ob_ubereval_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_OBJECT_UBEREVAL);
	
	/* parenting */
	if (ob->parent) {
		/* parent relationship */
		build_object_parent(ob);
		
		/* local -> parent */
		add_relation(local_transform_key, parent_transform_key, DEPSREL_TYPE_COMPONENT_ORDER, "[ObLocal -> ObParent]");
	}
	
	/* object constraints */
	if (ob->constraints.first) {
		OperationKey constraint_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_CONSTRAINTS);
		
		/* constraint relations */
		// TODO: provide base op
		// XXX: this is broken
		build_constraints(scene, &ob->id, DEPSNODE_TYPE_TRANSFORM, "", &ob->constraints, NULL);
		
		/* operation order */
		add_relation(base_op_key, constraint_key, DEPSREL_TYPE_COMPONENT_ORDER, "[ObBase-> Constraint Stack]");
		add_relation(constraint_key, final_transform_key, DEPSREL_TYPE_COMPONENT_ORDER, "[ObConstraints -> Done]");
		
		// XXX
		add_relation(constraint_key, ob_ubereval_key, DEPSREL_TYPE_COMPONENT_ORDER, "Temp Ubereval");
		add_relation(ob_ubereval_key, final_transform_key, DEPSREL_TYPE_COMPONENT_ORDER, "Temp Ubereval");
	}
	else {
		/* operation order */
		add_relation(base_op_key, final_transform_key, DEPSREL_TYPE_COMPONENT_ORDER, "Object Transform");
		
		// XXX
		add_relation(base_op_key, ob_ubereval_key, DEPSREL_TYPE_COMPONENT_ORDER, "Temp Ubereval");
		add_relation(ob_ubereval_key, final_transform_key, DEPSREL_TYPE_COMPONENT_ORDER, "Temp Ubereval");
	}
	
	
	/* AnimData */
	build_animdata(&ob->id);
	
	// XXX: This should be hooked up by the build_animdata code
	if (ob->adt && (ob->adt->action || ob->adt->nla_tracks.first)) {
		ComponentKey adt_key(&ob->id, DEPSNODE_TYPE_ANIMATION);
		add_relation(adt_key, local_transform_key, DEPSREL_TYPE_OPERATION, "Object Animation");
	}
	
	
	/* object data */
	if (ob->data) {
		ID *obdata_id = (ID *)ob->data;
		
		/* ob data animation */
		build_animdata(obdata_id);
		
		/* type-specific data... */
		switch (ob->type) {
			case OB_MESH:     /* Geometry */
			case OB_CURVE:
			case OB_FONT:
			case OB_SURF:
			case OB_MBALL:
			case OB_LATTICE:
			{
				build_obdata_geom(scene, ob);
			}
			break;
			
			
			case OB_ARMATURE: /* Pose */
				build_rig(scene, ob);
				break;
			
			case OB_LAMP:   /* Lamp */
				build_lamp(ob);
				break;
				
			case OB_CAMERA: /* Camera */
				build_camera(ob);
				break;
		}
	}
	
	/* particle systems */
	if (ob->particlesystem.first) {
		build_particles(scene, ob);
	}
	
	/* grease pencil */
	if (ob->gpd) {
		build_gpencil(&ob->id, ob->gpd);
	}
}

void DepsgraphRelationBuilder::build_object_parent(Object *ob)
{
	/* XXX: for now, need to use the component key (not just direct to the parent op), or else the matrix doesn't get reset */
	// XXX: @sergey - it would be good if we got that backwards flushing working when tagging for updates 
	//OperationKey ob_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_PARENT);
	ComponentKey ob_key(&ob->id, DEPSNODE_TYPE_TRANSFORM);
	
	/* type-specific links */
	switch (ob->partype) {
		case PARSKEL:  /* Armature Deform (Virtual Modifier) */
		{
			ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_TRANSFORM);
			add_relation(parent_key, ob_key, DEPSREL_TYPE_STANDARD, "Armature Deform Parent");
		}
		break;
			
		case PARVERT1: /* Vertex Parent */
		case PARVERT3:
		{
			ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_GEOMETRY);
			add_relation(parent_key, ob_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Vertex Parent");
			/* XXX not sure what this is for or how you could be done properly - lukas */
			//parent_node->customdata_mask |= CD_MASK_ORIGINDEX;
			
			ComponentKey transform_key(&ob->parent->id, DEPSNODE_TYPE_TRANSFORM);
			add_relation(transform_key, ob_key, DEPSREL_TYPE_TRANSFORM, "Vertex Parent TFM");
		}
		break;
			
		case PARBONE: /* Bone Parent */
		{
			ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_BONE, ob->parsubstr);
			add_relation(parent_key, ob_key, DEPSREL_TYPE_TRANSFORM, "Bone Parent");
		}
		break;
			
		default:
		{
			if (ob->parent->type == OB_LATTICE) {
				/* Lattice Deform Parent - Virtual Modifier */
				// XXX: no virtual modifiers should be left!
				ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_TRANSFORM);
				ComponentKey geom_key(&ob->parent->id, DEPSNODE_TYPE_GEOMETRY);
				
				add_relation(parent_key, ob_key, DEPSREL_TYPE_STANDARD, "Lattice Deform Parent");
				add_relation(geom_key, ob_key, DEPSREL_TYPE_STANDARD, "Lattice Deform Parent Geom");
			}
			else if (ob->parent->type == OB_CURVE) {
				Curve *cu = (Curve *)ob->parent->data;
				
				if (cu->flag & CU_PATH) {
					/* Follow Path */
					ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_GEOMETRY);
					add_relation(parent_key, ob_key, DEPSREL_TYPE_TRANSFORM, "Curve Follow Parent");
					
					ComponentKey transform_key(&ob->parent->id, DEPSNODE_TYPE_TRANSFORM);
					add_relation(transform_key, ob_key, DEPSREL_TYPE_TRANSFORM, "Curve Follow TFM");
				}
				else {
					/* Standard Parent */
					ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_TRANSFORM);
					add_relation(parent_key, ob_key, DEPSREL_TYPE_TRANSFORM, "Curve Parent");
				}
			}
			else {
				/* Standard Parent */
				ComponentKey parent_key(&ob->parent->id, DEPSNODE_TYPE_TRANSFORM);
				add_relation(parent_key, ob_key, DEPSREL_TYPE_TRANSFORM, "Parent");
			}
		}
		break;
	}
	
	/* exception case: parent is duplivert */
	if ((ob->type == OB_MBALL) && (ob->parent->transflag & OB_DUPLIVERTS)) {
		//dag_add_relation(dag, node2, node, DAG_RL_DATA_DATA | DAG_RL_OB_OB, "Duplivert");
	}
}

void DepsgraphRelationBuilder::build_constraints(Scene *scene, ID *id, eDepsNode_Type component_type, const char *component_subdata,
                                                 ListBase *constraints, RootPChanMap *root_map)
{
	OperationKey constraint_op_key(id, component_type, component_subdata,
	                               (component_type == DEPSNODE_TYPE_BONE) ? DEG_OPCODE_BONE_CONSTRAINTS : DEG_OPCODE_TRANSFORM_CONSTRAINTS);

	/* add dependencies for each constraint in turn */
	for (bConstraint *con = (bConstraint *)constraints->first; con; con = con->next) {
		bConstraintTypeInfo *cti = BKE_constraint_typeinfo_get(con);

		/* invalid constraint type... */
		if (cti == NULL)
			continue;

		/* special case for camera tracking -- it doesn't use targets to define relations */
		// TODO: we can now represent dependencies in a much richer manner, so review how this is done...
		if (ELEM(cti->type, CONSTRAINT_TYPE_FOLLOWTRACK, CONSTRAINT_TYPE_CAMERASOLVER, CONSTRAINT_TYPE_OBJECTSOLVER)) {
			bool depends_on_camera = false;
			
			if (cti->type == CONSTRAINT_TYPE_FOLLOWTRACK) {
				bFollowTrackConstraint *data = (bFollowTrackConstraint *)con->data;

				if (((data->clip) || (data->flag & FOLLOWTRACK_ACTIVECLIP)) && data->track[0])
					depends_on_camera = true;
				
				if (data->depth_ob) {
					// DAG_RL_DATA_OB | DAG_RL_OB_OB
					ComponentKey depth_key(&data->depth_ob->id, DEPSNODE_TYPE_TRANSFORM);
					add_relation(depth_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
				}
			}
			else if (cti->type == CONSTRAINT_TYPE_OBJECTSOLVER) {
				depends_on_camera = true;
			}

			if (depends_on_camera && scene->camera) {
				// DAG_RL_DATA_OB | DAG_RL_OB_OB
				ComponentKey camera_key(&scene->camera->id, DEPSNODE_TYPE_TRANSFORM);
				add_relation(camera_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
			}
			
			/* tracker <-> constraints */
			// FIXME: actually motionclip dependency on results of motionclip block here...
			//dag_add_relation(dag, scenenode, node, DAG_RL_SCENE, "Scene Relation");
		}
		else if (cti->get_constraint_targets) {
			ListBase targets = {NULL, NULL};
			cti->get_constraint_targets(con, &targets);
			
			for (bConstraintTarget *ct = (bConstraintTarget *)targets.first; ct; ct = ct->next) {
				if (!ct->tar)
					continue;
				
				if (ELEM(con->type, CONSTRAINT_TYPE_KINEMATIC, CONSTRAINT_TYPE_SPLINEIK)) {
					/* ignore IK constraints - these are handled separately (on pose level) */
					// XXX: this is bad - it precludes using geometry targets -- aligorith
				}
				else if (ELEM(con->type, CONSTRAINT_TYPE_FOLLOWPATH, CONSTRAINT_TYPE_CLAMPTO)) {
					/* these constraints require path geometry data... */
					ComponentKey target_key(&ct->tar->id, DEPSNODE_TYPE_GEOMETRY);
					add_relation(target_key, constraint_op_key, DEPSREL_TYPE_GEOMETRY_EVAL, cti->name); // XXX: type = geom_transform
					// TODO: path dependency
				}
				else if ((ct->tar->type == OB_ARMATURE) && (ct->subtarget[0])) {
					/* bone */
					if (&ct->tar->id == id) {
						/* same armature  */
						eDepsOperation_Code target_key_opcode;
						
						/* Using "done" here breaks in-chain deps, while using "ready" here breaks most production rigs instead...
						 * So, we do a compromise here, and only do this when an IK chain conflict may occur
						 */
						if (root_map->has_common_root(component_subdata, ct->subtarget)) {
							target_key_opcode = DEG_OPCODE_BONE_READY;
						}
						else {
							target_key_opcode = DEG_OPCODE_BONE_DONE;
						}

						OperationKey target_key(&ct->tar->id, DEPSNODE_TYPE_BONE, ct->subtarget, target_key_opcode);
						add_relation(target_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
					}
					else {
						/* different armature - we can safely use the result of that */
						OperationKey target_key(&ct->tar->id, DEPSNODE_TYPE_BONE, ct->subtarget, DEG_OPCODE_BONE_DONE);
						add_relation(target_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
					}
				}
				else if (ELEM(ct->tar->type, OB_MESH, OB_LATTICE) && (ct->subtarget[0])) {
					/* vertex group */
					/* NOTE: for now, we don't need to represent vertex groups separately... */
					ComponentKey target_key(&ct->tar->id, DEPSNODE_TYPE_GEOMETRY);
					add_relation(target_key, constraint_op_key, DEPSREL_TYPE_GEOMETRY_EVAL, cti->name);
					
					if (ct->tar->type == OB_MESH) {
						//node2->customdata_mask |= CD_MASK_MDEFORMVERT;
					}
				}
				else if (con->type == CONSTRAINT_TYPE_SHRINKWRAP) {
					/* Constraints which requires the target object surface. */
					ComponentKey target_key(&ct->tar->id, DEPSNODE_TYPE_GEOMETRY);
					add_relation(target_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
					
					/* NOTE: obdata eval now doesn't necessarily depend on the object's transform... */
					ComponentKey target_transform_key(&ct->tar->id, DEPSNODE_TYPE_TRANSFORM);
					add_relation(target_transform_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
				}
				else {
					/* standard object relation */
					// TODO: loc vs rot vs scale?
					if (&ct->tar->id == id) {
						/* Constraint targetting own object:
						 * - This case is fine IFF we're dealing with a bone constraint pointing to
						 *   its own armature. In that case, it's just transform -> bone.
						 * - If however it is a real self targetting case, just make it depend on the
						 *   previous constraint (or the pre-constraint state)...
						 */
						if ((ct->tar->type == OB_ARMATURE) && (component_type == DEPSNODE_TYPE_BONE)) {
							OperationKey target_key(&ct->tar->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_FINAL);
							add_relation(target_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
						}
						else {
							OperationKey target_key(&ct->tar->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_LOCAL);
							add_relation(target_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
						}
					}
					else {
						/* normal object dependency */
						OperationKey target_key(&ct->tar->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_FINAL);
						add_relation(target_key, constraint_op_key, DEPSREL_TYPE_TRANSFORM, cti->name);
					}
				}
			}
			
			if (cti->flush_constraint_targets)
				cti->flush_constraint_targets(con, &targets, 1);
		}
	}
}

void DepsgraphRelationBuilder::build_animdata(ID *id)
{
	AnimData *adt = BKE_animdata_from_id(id);
	
	if (adt == NULL)
		return;
	
	ComponentKey adt_key(id, DEPSNODE_TYPE_ANIMATION);
	
	/* animation */
	if (adt->action || adt->nla_tracks.first) {
		/* wire up dependency to time source */
		TimeSourceKey time_src_key;
		add_relation(time_src_key, adt_key, DEPSREL_TYPE_TIME, "[TimeSrc -> Animation]");
		
		// XXX: Hook up specific update callbacks for special properties which may need it...
		
		// XXX: animdata "hierarchy" - top-level overrides need to go after lower-down
	}
	
	/* drivers */
	for (FCurve *fcu = (FCurve *)adt->drivers.first; fcu; fcu = fcu->next) {
		OperationKey driver_key(id, DEPSNODE_TYPE_PARAMETERS, DEG_OPCODE_DRIVER, fcu->rna_path);
		
		/* create the driver's relations to targets */
		build_driver(id, fcu);
		
		/* prevent driver from occurring before own animation... */
		if (adt->action || adt->nla_tracks.first) {
			add_relation(adt_key, driver_key, DEPSREL_TYPE_OPERATION, 
						 "[AnimData Before Drivers]");
		}
	}
}

void DepsgraphRelationBuilder::build_driver(ID *id, FCurve *fcu)
{
	ChannelDriver *driver = fcu->driver;
	OperationKey driver_key(id, DEPSNODE_TYPE_PARAMETERS, DEG_OPCODE_DRIVER, deg_fcurve_id_name(fcu));
	
	/* create dependency between driver and data affected by it */
	/* - direct property relationship... */
	//RNAPathKey affected_key(id, fcu->rna_path);
	//add_relation(driver_key, affected_key, DEPSREL_TYPE_DRIVER, "[Driver -> Data] DepsRel");
	
	/* driver -> data components (for interleaved evaluation - bones/constraints/modifiers) */
	// XXX: this probably should probably be moved out into a separate function
	if (strstr(fcu->rna_path, "pose.bones[") != NULL) {
		/* interleaved drivers during bone eval */
		// TODO: ideally, if this is for a constraint, it goes to said constraint
		Object *ob = (Object *)id;
		bPoseChannel *pchan;
		char *bone_name;
		
		bone_name = BLI_str_quoted_substrN(fcu->rna_path, "pose.bones[");
		pchan = BKE_pose_channel_find_name(ob->pose, bone_name);
		
		if (bone_name) {
			MEM_freeN(bone_name);
			bone_name = NULL;
		}
		
		if (pchan) {
			OperationKey bone_key(id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_LOCAL);
			add_relation(driver_key, bone_key, DEPSREL_TYPE_DRIVER, "[Driver -> Bone]");
		}
		else {
			fprintf(stderr,
			        "Couldn't find bone name for driver path - '%s'\n",
			        fcu->rna_path);
		}
	}
	else if (GS(id->name) == ID_AR && strstr(fcu->rna_path, "bones[")) {
		/* drivers on armature-level bone settings (i.e. bbone stuff),
		 * which will affect the evaluation of corresponding pose bones
		 */
		IDDepsNode *arm_node = m_graph->find_id_node(id);
		char *bone_name = BLI_str_quoted_substrN(fcu->rna_path, "bones[");
		
		if (arm_node && bone_name) {
			/* find objects which use this, and make their eval callbacks depend on this */
			DEPSNODE_RELATIONS_ITER_BEGIN(arm_node->outlinks, rel)
			{
				IDDepsNode *to_node = (IDDepsNode *)rel->to;
				
				/* we only care about objects with pose data which use this... */
				if (GS(to_node->id->name) == ID_OB) {
					Object *ob = (Object *)to_node->id;
					bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, bone_name); // NOTE: ob->pose may be NULL
					
					if (pchan) {
						OperationKey bone_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_LOCAL);
						add_relation(driver_key, bone_key, DEPSREL_TYPE_DRIVER, "[Arm Bone -> Driver -> Bone]");
					}
				}
			}
			DEPSNODE_RELATIONS_ITER_END;
			
			/* free temp data */
			MEM_freeN(bone_name);
			bone_name = NULL;
		}
		else {
			fprintf(stderr,
			        "Couldn't find armature bone name for driver path - '%s'\n",
			        fcu->rna_path);
		}
	}
	else if (GS(id->name) == ID_OB && strstr(fcu->rna_path, "modifiers[")) {
		/* modifier driver - connect directly to the modifier */
		char *modifier_name = BLI_str_quoted_substrN(fcu->rna_path, "modifiers[");
		if (modifier_name) {
			OperationKey modifier_key(id, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_GEOMETRY_MODIFIER, modifier_name);
			add_relation(driver_key, modifier_key, DEPSREL_TYPE_DRIVER, "[Driver -> Modifier]");
			
			MEM_freeN(modifier_name);
		}
	}
	else if (GS(id->name) == ID_KE && strstr(fcu->rna_path, "key_blocks[")) {
		/* shape key driver - hook into the base geometry operation */
		// XXX: double check where this points
		Key *shape_key = (Key *)id;
		
		ComponentKey geometry_key(shape_key->from, DEPSNODE_TYPE_GEOMETRY);
		add_relation(driver_key, geometry_key, DEPSREL_TYPE_DRIVER, "[Driver -> ShapeKey Geom]");
	}
	else {
		if (GS(id->name) == ID_OB) {
			/* assume that driver affects a transform... */
			OperationKey local_transform_key(id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_LOCAL);
			add_relation(driver_key, local_transform_key, DEPSREL_TYPE_OPERATION, "[Driver -> Transform]");
		}
	}
	
	/* ensure that affected prop's update callbacks will be triggered once done */
	// TODO: implement this once the functionality to add these links exists in RNA
	// XXX: the data itself could also set this, if it were to be truly initialised later?
	
	/* loop over variables to get the target relationships */
	for (DriverVar *dvar = (DriverVar *)driver->variables.first; dvar; dvar = dvar->next) {
		/* only used targets */
		DRIVER_TARGETS_USED_LOOPER(dvar) 
		{
			if (dtar->id == NULL)
				continue;
			
			/* special handling for directly-named bones */
			if ((dtar->flag & DTAR_FLAG_STRUCT_REF) && (dtar->pchan_name[0])) {
				Object *ob = (Object *)dtar->id;
				bPoseChannel *pchan = BKE_pose_channel_find_name(ob->pose, dtar->pchan_name);
				
				if (pchan != NULL) {
					/* get node associated with bone */
					// XXX: watch the space!
					OperationKey target_key(dtar->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_DONE);
					add_relation(target_key, driver_key, DEPSREL_TYPE_DRIVER_TARGET, "[Bone Target -> Driver]");
				}
			}
			else if (dtar->flag & DTAR_FLAG_STRUCT_REF) {
				/* get node associated with the object's transforms */
				OperationKey target_key(dtar->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_FINAL);
				add_relation(target_key, driver_key, DEPSREL_TYPE_DRIVER_TARGET, "[Target -> Driver]");
			}
			else if (dtar->rna_path && strstr(dtar->rna_path, "pose.bones[")) {
				/* workaround for ensuring that local bone transforms don't end up
				 * having to wait for pose eval to finish (to prevent cycles)
				 */
				Object *ob = (Object *)dtar->id;
				bPoseChannel *pchan;
				char *bone_name;
				
				bone_name = BLI_str_quoted_substrN(dtar->rna_path, "pose.bones[");
				pchan = BKE_pose_channel_find_name(ob->pose, bone_name);
				
				if (bone_name) {
					MEM_freeN(bone_name);
					bone_name = NULL;
				}
				
				if (pchan) {
					OperationKey bone_key(dtar->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_LOCAL);
					add_relation(bone_key, driver_key, DEPSREL_TYPE_DRIVER, "[RNA Bone -> Driver]");
				}
			}
			else {
				/* resolve path to get node */
				RNAPathKey target_key(dtar->id, dtar->rna_path ? dtar->rna_path : "");
				add_relation(target_key, driver_key, DEPSREL_TYPE_DRIVER_TARGET, "[RNA Target -> Driver]");
			}
		}
		DRIVER_TARGETS_LOOPER_END
	}
}

void DepsgraphRelationBuilder::build_world(Scene *scene, World *world)
{
	/* Prevent infinite recursion by checking (and tagging the world) as having been visited 
	 * already. This assumes wo->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	ID *world_id = &world->id;
	if (id_is_tagged(world_id))
		return;
	id_tag_set(world_id);
	
	build_animdata(world_id);
	
	/* TODO: other settings? */
	
	/* textures */
	build_texture_stack(world_id, world->mtex);
	
	/* world's nodetree */
	build_nodetree(world_id, world->nodetree);

	id_tag_clear(world_id);
}

void DepsgraphRelationBuilder::build_rigidbody(Scene *scene)
{
	RigidBodyWorld *rbw = scene->rigidbody_world;
	
	OperationKey init_key(&scene->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_RIGIDBODY_REBUILD);
	OperationKey sim_key(&scene->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_RIGIDBODY_SIM);
	
	/* rel between the two sim-nodes */
	add_relation(init_key, sim_key, DEPSREL_TYPE_OPERATION, "Rigidbody [Init -> SimStep]");
	
	/* set up dependencies between these operations and other builtin nodes --------------- */	
	
	/* time dependency */
	TimeSourceKey time_src_key;
	add_relation(time_src_key, init_key, DEPSREL_TYPE_TIME, "TimeSrc -> Rigidbody Reset/Rebuild (Optional)");
	add_relation(time_src_key, sim_key, DEPSREL_TYPE_TIME, "TimeSrc -> Rigidbody Sim Step");
	
	/* objects - simulation participants */
	if (rbw->group) {
		for (GroupObject *go = (GroupObject *)rbw->group->gobject.first; go; go = go->next) {
			Object *ob = go->ob;
			if (!ob || ob->type != OB_MESH)
				continue;
			
			/* hook up evaluation order... 
			 * 1) flushing rigidbody results follows base transforms being applied
			 * 2) rigidbody flushing can only be performed after simulation has been run
			 *
			 * 3) simulation needs to know base transforms to figure out what to do
			 *    XXX: there's probably a difference between passive and active 
			 *         - passive don't change, so may need to know full transform...
			 */
			OperationKey rbo_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_RIGIDBODY);
			
			eDepsOperation_Code trans_opcode = ob->parent ? DEG_OPCODE_TRANSFORM_PARENT : DEG_OPCODE_TRANSFORM_LOCAL;
			OperationKey trans_op(&ob->id, DEPSNODE_TYPE_TRANSFORM, trans_opcode);
			
			add_relation(trans_op, rbo_key, DEPSREL_TYPE_OPERATION, "Base Ob Transform -> RBO Sync");
			add_relation(sim_key, rbo_key, DEPSREL_TYPE_COMPONENT_ORDER, "Rigidbody Sim Eval -> RBO Sync");
			
			/* if constraints exist, those depend on the result of the rigidbody sim
			 * - This allows constraints to modify the result of the sim (i.e. clamping)
			 *   while still allowing the sim to depend on some changes to the objects.
			 *   Also, since constraints are hooked up to the final nodes, this link
			 *   means that we can also fit in there too...
			 * - Later, it might be good to include a constraint in the stack allowing us
			 *   to control whether rigidbody eval gets interleaved into the constraint stack
			 */
			if (ob->constraints.first) {
				OperationKey constraint_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_CONSTRAINTS);
				add_relation(rbo_key, constraint_key, DEPSREL_TYPE_COMPONENT_ORDER, "RBO Sync -> Ob Constraints");
			}
			else {
				/* final object transform depends on rigidbody */
				OperationKey done_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_FINAL);
				add_relation(rbo_key, done_key, DEPSREL_TYPE_COMPONENT_ORDER, "RBO Sync -> Done");
				
				// XXX: ubereval will be removed eventually, but we still need it in the meantime
				OperationKey uber_key(&ob->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_OBJECT_UBEREVAL);
				add_relation(rbo_key, uber_key, DEPSREL_TYPE_COMPONENT_ORDER, "RBO Sync -> Uber (Temp)");
			}
			
			
			/* needed to get correct base values */
			add_relation(trans_op, sim_key, DEPSREL_TYPE_OPERATION, "Base Ob Transform -> Rigidbody Sim Eval");
		}
	}
	
	/* constraints */
	if (rbw->constraints) {
		for (GroupObject *go = (GroupObject *)rbw->constraints->gobject.first; go; go = go->next) {
			Object *ob = go->ob;
			if (!ob || !ob->rigidbody_constraint)
				continue;
			
			RigidBodyCon *rbc = ob->rigidbody_constraint;
			
			/* final result of the constraint object's transform controls how the
			 * constraint affects the physics sim for these objects 
			 */
			ComponentKey trans_key(&ob->id, DEPSNODE_TYPE_TRANSFORM);
			OperationKey ob1_key(&rbc->ob1->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_RIGIDBODY);
			OperationKey ob2_key(&rbc->ob2->id, DEPSNODE_TYPE_TRANSFORM, DEG_OPCODE_TRANSFORM_RIGIDBODY);
			
			/* - constrained-objects sync depends on the constraint-holder */
			add_relation(trans_key, ob1_key, DEPSREL_TYPE_TRANSFORM, "RigidBodyConstraint -> RBC.Object_1");
			add_relation(trans_key, ob2_key, DEPSREL_TYPE_TRANSFORM, "RigidBodyConstraint -> RBC.Object_2");
			
			/* - ensure that sim depends on this constraint's transform */
			add_relation(trans_key, sim_key, DEPSREL_TYPE_TRANSFORM, "RigidBodyConstraint Transform -> RB Simulation");
		}
	}
}

void DepsgraphRelationBuilder::build_particles(Scene *scene, Object *ob)
{
	/* particle systems */
	for (ParticleSystem *psys = (ParticleSystem *)ob->particlesystem.first; psys; psys = psys->next) {
		ParticleSettings *part = psys->part;
		
		/* particle settings */
		build_animdata(&part->id);
		
		/* this particle system */
		OperationKey psys_key(&ob->id, DEPSNODE_TYPE_EVAL_PARTICLES, DEG_OPCODE_PSYS_EVAL);
		
		/* XXX: if particle system is later re-enabled, we must do full rebuild? */
		if (!psys_check_enabled(ob, psys))
			continue;
		
#if 0
		if (ELEM(part->phystype, PART_PHYS_KEYED, PART_PHYS_BOIDS)) {
			ParticleTarget *pt;

			for (pt = psys->targets.first; pt; pt = pt->next) {
				if (pt->ob && BLI_findlink(&pt->ob->particlesystem, pt->psys - 1)) {
					node2 = dag_get_node(dag, pt->ob);
					dag_add_relation(dag, node2, node, DAG_RL_DATA_DATA | DAG_RL_OB_DATA, "Particle Targets");
				}
			}
		}
		
		if (part->ren_as == PART_DRAW_OB && part->dup_ob) {
			node2 = dag_get_node(dag, part->dup_ob);
			/* note that this relation actually runs in the wrong direction, the problem
			 * is that dupli system all have this (due to parenting), and the render
			 * engine instancing assumes particular ordering of objects in list */
			dag_add_relation(dag, node, node2, DAG_RL_OB_OB, "Particle Object Visualization");
			if (part->dup_ob->type == OB_MBALL)
				dag_add_relation(dag, node, node2, DAG_RL_DATA_DATA, "Particle Object Visualization");
		}
		
		if (part->ren_as == PART_DRAW_GR && part->dup_group) {
			for (go = part->dup_group->gobject.first; go; go = go->next) {
				node2 = dag_get_node(dag, go->ob);
				dag_add_relation(dag, node2, node, DAG_RL_OB_OB, "Particle Group Visualization");
			}
		}
#endif
		
		/* effectors */
		ListBase *effectors = pdInitEffectors(scene, ob, psys, part->effector_weights, false);
		
		if (effectors) {
			for (EffectorCache *eff = (EffectorCache *)effectors->first; eff; eff = eff->next) {
				if (eff->psys) {
					// XXX: DAG_RL_DATA_DATA | DAG_RL_OB_DATA
					ComponentKey eff_key(&eff->ob->id, DEPSNODE_TYPE_GEOMETRY); // xxx: particles instead?
					add_relation(eff_key, psys_key, DEPSREL_TYPE_STANDARD, "Particle Field");
				}
			}
		}
		
		pdEndEffectors(&effectors);
		
		/* boids */
		if (part->boids) {
			BoidRule *rule = NULL;
			BoidState *state = NULL;
			
			for (state = (BoidState *)part->boids->states.first; state; state = state->next) {
				for (rule = (BoidRule *)state->rules.first; rule; rule = rule->next) {
					Object *ruleob = NULL;
					if (rule->type == eBoidRuleType_Avoid)
						ruleob = ((BoidRuleGoalAvoid *)rule)->ob;
					else if (rule->type == eBoidRuleType_FollowLeader)
						ruleob = ((BoidRuleFollowLeader *)rule)->ob;

					if (ruleob) {
						ComponentKey ruleob_key(&ruleob->id, DEPSNODE_TYPE_TRANSFORM);
						add_relation(ruleob_key, psys_key, DEPSREL_TYPE_TRANSFORM, "Boid Rule");
					}
				}
			}
		}
	}
	
	/* pointcache */
	// TODO...
}

/* IK Solver Eval Steps */
void DepsgraphRelationBuilder::build_ik_pose(Object *ob,
                                             bPoseChannel *pchan,
                                             bConstraint *con,
                                             RootPChanMap *root_map)
{
	bKinematicConstraint *data = (bKinematicConstraint *)con->data;
	
	/* attach owner to IK Solver too 
	 * - assume that owner is always part of chain 
	 * - see notes on direction of rel below...
	 */
	bPoseChannel *rootchan = BKE_armature_ik_solver_find_root(pchan, data);
	OperationKey solver_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE, rootchan->name, DEG_OPCODE_POSE_IK_SOLVER);
	
	OperationKey transforms_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_READY);
	add_relation(transforms_key, solver_key, DEPSREL_TYPE_TRANSFORM, "IK Solver Owner");

	/* IK target */
	// XXX: this should get handled as part of the constraint code
	if (data->tar != NULL) {
		/* TODO(sergey): For until we'll store partial matricies in the depsgraph,
		 * we create dependency bewteen target object and pose eval component.
		 *
		 * This way we ensuring the whole subtree is updated from sctratch without
		 * need of intermediate matricies. This is an overkill, but good enough for
		 * testing IK solver.
		 */
		// FIXME: geometry targets...
		ComponentKey pose_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE);
		if ((data->tar->type == OB_ARMATURE) && (data->subtarget[0])) {
			/* TODO(sergey): This is only for until granular update stores intermediate result. */
			if (data->tar != ob) {
				/* different armature - can just read the results */
				ComponentKey target_key(&data->tar->id, DEPSNODE_TYPE_BONE, data->subtarget);
				add_relation(target_key, pose_key, DEPSREL_TYPE_TRANSFORM, con->name);
			}
			else {
				/* same armature - we'll use the ready state only, just in case this bone is in the chain we're solving */
				//OperationKey target_key(&data->tar->id, DEPSNODE_TYPE_BONE, data->subtarget, DEG_OPCODE_BONE_READY);
				OperationKey target_key(&data->tar->id, DEPSNODE_TYPE_BONE, data->subtarget, DEG_OPCODE_BONE_DONE);
				add_relation(target_key, solver_key, DEPSREL_TYPE_TRANSFORM, con->name);
			}
		}
		else {
			ComponentKey target_key(&data->tar->id, DEPSNODE_TYPE_TRANSFORM);
			add_relation(target_key, pose_key, DEPSREL_TYPE_TRANSFORM, con->name);
		}
	}
	root_map->add_bone(pchan->name, rootchan->name);

	/* Pole Target */
	// XXX: this should get handled as part of the constraint code
	if (data->poletar != NULL) {
		if ((data->tar->type == OB_ARMATURE) && (data->subtarget[0])) {
			// XXX: same armature issues - ready vs done?
			ComponentKey target_key(&data->poletar->id, DEPSNODE_TYPE_BONE, data->subtarget);
			add_relation(target_key, solver_key, DEPSREL_TYPE_TRANSFORM, con->name);
		}
		else {
			ComponentKey target_key(&data->poletar->id, DEPSNODE_TYPE_TRANSFORM);
			add_relation(target_key, solver_key, DEPSREL_TYPE_TRANSFORM, con->name);
		}
	}
	
	
	bPoseChannel *parchan = pchan;
	/* exclude tip from chain? */
	if (!(data->flag & CONSTRAINT_IK_TIP))
		parchan = pchan->parent;
	
	/* Walk to the chain's root */
	//size_t segcount = 0;
	int segcount = 0;
	
	while (parchan) {
		/* Make IK-solver dependent on this bone's result,
		 * since it can only run after the standard results 
		 * of the bone are know. Validate links step on the 
		 * bone will ensure that users of this bone only
		 * grab the result with IK solver results...
		 */
		if (parchan != pchan) {
			OperationKey parent_key(&ob->id, DEPSNODE_TYPE_BONE, parchan->name, DEG_OPCODE_BONE_READY);
			add_relation(parent_key, solver_key, DEPSREL_TYPE_TRANSFORM, "IK Chain Parent");
		}
		parchan->flag |= POSE_DONE;

		OperationKey final_transforms_key(&ob->id, DEPSNODE_TYPE_BONE, parchan->name, DEG_OPCODE_BONE_DONE);
		add_relation(solver_key, final_transforms_key, DEPSREL_TYPE_TRANSFORM, "IK Solver Result");

		root_map->add_bone(parchan->name, rootchan->name);

		/* continue up chain, until we reach target number of items... */
		segcount++;
		if ((segcount == data->rootbone) || (segcount > 255)) break;  /* 255 is weak */
		
		parchan  = parchan->parent;
	}

	OperationKey flush_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE, DEG_OPCODE_POSE_DONE);
	add_relation(solver_key, flush_key, DEPSREL_TYPE_OPERATION, "PoseEval Result-Bone Link");
}

/* Spline IK Eval Steps */
void DepsgraphRelationBuilder::build_splineik_pose(Object *ob,
                                                   bPoseChannel *pchan,
                                                   bConstraint *con,
                                                   RootPChanMap *root_map)
{
	bSplineIKConstraint *data = (bSplineIKConstraint *)con->data;
	bPoseChannel *rootchan = BKE_armature_splineik_solver_find_root(pchan, data);
	OperationKey transforms_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_READY);
	OperationKey solver_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE, rootchan->name, DEG_OPCODE_POSE_SPLINE_IK_SOLVER);
	
	/* attach owner to IK Solver too 
	 * - assume that owner is always part of chain 
	 * - see notes on direction of rel below...
	 */
	add_relation(transforms_key, solver_key, DEPSREL_TYPE_TRANSFORM, "Spline IK Solver Owner");
	
	/* attach path dependency to solver */
	if (data->tar) {
		/* TODO(sergey): For until we'll store partial matricies in the depsgraph,
		 * we create dependency bewteen target object and pose eval component.
		 * See IK pose for a bit more information.
		 */
		// TODO: the bigggest point here is that we need the curve PATH and not just the general geometry...
		ComponentKey target_key(&data->tar->id, DEPSNODE_TYPE_GEOMETRY);
		ComponentKey pose_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE);
		add_relation(target_key, pose_key, DEPSREL_TYPE_TRANSFORM,"[Curve.Path -> Spline IK] DepsRel");
	}

	pchan->flag |= POSE_DONE;
	OperationKey final_transforms_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_DONE);
	add_relation(solver_key, final_transforms_key, DEPSREL_TYPE_TRANSFORM, "Spline IK Result");

	root_map->add_bone(pchan->name, rootchan->name);

	/* Walk to the chain's root */
	//size_t segcount = 0;
	int segcount = 0;
	
	for (bPoseChannel *parchan = pchan->parent; parchan; parchan = parchan->parent) {
		/* Make Spline IK solver dependent on this bone's result,
		 * since it can only run after the standard results 
		 * of the bone are know. Validate links step on the 
		 * bone will ensure that users of this bone only
		 * grab the result with IK solver results...
		 */
		if (parchan != pchan) {
			OperationKey parent_key(&ob->id, DEPSNODE_TYPE_BONE, parchan->name, DEG_OPCODE_BONE_READY);
			add_relation(parent_key, solver_key, DEPSREL_TYPE_TRANSFORM, "Spline IK Solver Update");
		}
		parchan->flag |= POSE_DONE;

		OperationKey final_transforms_key(&ob->id, DEPSNODE_TYPE_BONE, parchan->name, DEG_OPCODE_BONE_DONE);
		add_relation(solver_key, final_transforms_key, DEPSREL_TYPE_TRANSFORM, "Spline IK Solver Result");

		root_map->add_bone(parchan->name, rootchan->name);

		/* continue up chain, until we reach target number of items... */
		segcount++;
		if ((segcount == data->chainlen) || (segcount > 255)) break;  /* 255 is weak */
	}

	OperationKey flush_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE, DEG_OPCODE_POSE_DONE);
	add_relation(solver_key, flush_key, DEPSREL_TYPE_OPERATION, "PoseEval Result-Bone Link");
}

/* Pose/Armature Bones Graph */
void DepsgraphRelationBuilder::build_rig(Scene *scene, Object *ob)
{
	/* Armature-Data */
	// TODO: selection status?
	
	/* attach links between pose operations */
	OperationKey init_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE, DEG_OPCODE_POSE_INIT);
	OperationKey flush_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE, DEG_OPCODE_POSE_DONE);
	
	add_relation(init_key, flush_key, DEPSREL_TYPE_COMPONENT_ORDER, "[Pose Init -> Pose Cleanup]");

	if (ob->adt != NULL) {
		ComponentKey animation_key(&ob->id, DEPSNODE_TYPE_ANIMATION);
		add_relation(animation_key, init_key, DEPSREL_TYPE_OPERATION, "Object Animation");
	}

	/* IK Solvers...
	* - These require separate processing steps are pose-level
	*   to be executed between chains of bones (i.e. once the
	*   base transforms of a bunch of bones is done)
	*
	* - We build relations for these before the dependencies
	*   between ops in the same component as it is necessary
	*   to check whether such bones are in the same IK chain
	*   (or else we get weird issues with either in-chain
	*   references, or with bones being parented to IK'd bones)
	*
	* Unsolved Issues:
	* - Care is needed to ensure that multi-headed trees work out the same as in ik-tree building
	* - Animated chain-lengths are a problem...
	*/
	RootPChanMap root_map;
	bool have_ik_solver = false;
	
	for (bPoseChannel *pchan = (bPoseChannel *)ob->pose->chanbase.first; pchan; pchan = pchan->next) {
		for (bConstraint *con = (bConstraint *)pchan->constraints.first; con; con = con->next) {
			switch (con->type) {
				case CONSTRAINT_TYPE_KINEMATIC:
					build_ik_pose(ob, pchan, con, &root_map);
					have_ik_solver = true;
					break;

				case CONSTRAINT_TYPE_SPLINEIK:
					build_splineik_pose(ob, pchan, con, &root_map);
					have_ik_solver = true;
					break;

				default:
					break;
			}
		}
	}
	//root_map.print_debug();
	
	if (have_ik_solver) {
		/* TODO(sergey): Once partial updates are possible use relation between
		 * object transform and solver itself in it's build function.
		 */
		ComponentKey pose_key(&ob->id, DEPSNODE_TYPE_EVAL_POSE);
		ComponentKey local_transform_key(&ob->id, DEPSNODE_TYPE_TRANSFORM);
		add_relation(local_transform_key, pose_key, DEPSREL_TYPE_TRANSFORM, "Local Transforms");
	}
	

	/* links between operations for each bone */
	for (bPoseChannel *pchan = (bPoseChannel *)ob->pose->chanbase.first; pchan; pchan = pchan->next) {
		OperationKey bone_local_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_LOCAL);
		OperationKey bone_pose_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_POSE_PARENT);
		OperationKey bone_ready_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_READY);
		OperationKey bone_done_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_DONE);
		
		pchan->flag &= ~POSE_DONE;
		
		/* pose init to bone local */
		add_relation(init_key, bone_local_key, DEPSREL_TYPE_OPERATION, "PoseEval Source-Bone Link");
		
		/* local to pose parenting operation */
		add_relation(bone_local_key, bone_pose_key, DEPSREL_TYPE_OPERATION, "Bone Local - PoseSpace Link");
		
		/* parent relation */
		if (pchan->parent != NULL) {
			eDepsOperation_Code parent_key_opcode;
			
			/* NOTE: this difference in handling allows us to prevent lockups while ensuring correct poses for separate chains */
			if (root_map.has_common_root(pchan->name, pchan->parent->name)) {
				parent_key_opcode = DEG_OPCODE_BONE_READY;
			}
			else {
				parent_key_opcode = DEG_OPCODE_BONE_DONE;
			}
			
			OperationKey parent_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->parent->name, parent_key_opcode);
			add_relation(parent_key, bone_pose_key, DEPSREL_TYPE_TRANSFORM, "[Parent Bone -> Child Bone]");
		}
		
		/* constraints */
		if (pchan->constraints.first != NULL) {
			/* constraints stack and constraint dependencies */
			build_constraints(scene, &ob->id, DEPSNODE_TYPE_BONE, pchan->name, &pchan->constraints, &root_map);
			
			/* pose -> constraints */
			OperationKey constraints_key(&ob->id, DEPSNODE_TYPE_BONE, pchan->name, DEG_OPCODE_BONE_CONSTRAINTS);
			add_relation(bone_pose_key, constraints_key, DEPSREL_TYPE_OPERATION, "Constraints Stack");
			
			/* constraints -> ready */
			// TODO: when constraint stack is exploded, this step should occur before the first IK solver
			add_relation(constraints_key, bone_ready_key, DEPSREL_TYPE_OPERATION, "Constraints -> Ready");
		}
		else {
			/* pose -> ready */
			add_relation(bone_pose_key, bone_ready_key, DEPSREL_TYPE_OPERATION, "Pose -> Ready");
		}
		
		/* bone ready -> done 
		 * NOTE: For bones without IK, this is all that's needed.
		 *       For IK chains however, an additional rel is created from IK to done,
		 *       with transitive reduction removing this one...
		 */
		add_relation(bone_ready_key, bone_done_key, DEPSREL_TYPE_OPERATION, "Ready -> Done");
		
		/* assume that all bones must be done for the pose to be ready (for deformers) */
		add_relation(bone_done_key, flush_key, DEPSREL_TYPE_OPERATION, "PoseEval Result-Bone Link");
	}
}

/* Shapekeys */
void DepsgraphRelationBuilder::build_shapekeys(ID *obdata, Key *key)
{
	ComponentKey obdata_key(obdata, DEPSNODE_TYPE_GEOMETRY);
	
	/* attach animdata to geometry */
	build_animdata(&key->id);
	
	if (key->adt) {
		// TODO: this should really be handled in build_animdata, since many of these cases will need it
		if (key->adt->action || key->adt->nla_tracks.first) {
			ComponentKey adt_key(&key->id, DEPSNODE_TYPE_ANIMATION);
			add_relation(adt_key, obdata_key, DEPSREL_TYPE_OPERATION, "Animation");
		}
		
		/* NOTE: individual shapekey drivers are handled above already */
	}
	
	/* attach to geometry */
	// XXX: aren't shapekeys now done as a pseudo-modifier on object?
	//ComponentKey key_key(&key->id, DEPSNODE_TYPE_GEOMETRY); // FIXME: this doesn't exist
	//add_relation(key_key, obdata_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Shapekeys");
}

/* ObData Geometry Evaluation
 * ==========================
 * The evaluation of geometry on objects is as follows:
 * - The actual evaluated of the derived geometry (e.g. DerivedMesh, DispList, etc.)
 *   occurs in the Geometry component of the object which references this. This includes
 *   modifiers, and the temporary "ubereval" for geometry.
 * - Therefore, each user of a piece of shared geometry data ends up evaluating its own
 *   version of the stuff, complete with whatever modifiers it may use.
 *
 * - The datablocks for the geometry data - "obdata" (e.g. ID_ME, ID_CU, ID_LT, etc.) are used for
 *     1) calculating the bounding boxes of the geometry data,
 *     2) aggregating inward links from other objects (e.g. for text on curve, etc.)
 *        and also for the links coming from the shapekey datablocks
 * - Animation/Drivers affecting the parameters of the geometry are made to trigger
 *   updates on the obdata geometry component, which then trigger downstream
 *   re-evaluation of the individual instances of this geometry.
 */
// TODO: Materials and lighting should probably get their own component, instead of being lumped under geometry?
void DepsgraphRelationBuilder::build_obdata_geom(Scene *scene, Object *ob)
{
	ID *obdata = (ID *)ob->data;
	
	/* get nodes for result of obdata's evaluation, and geometry evaluation on object */
	ComponentKey geom_key(&ob->id, DEPSNODE_TYPE_GEOMETRY);
	ComponentKey obdata_geom_key(obdata, DEPSNODE_TYPE_GEOMETRY);

	/* Link object data evaluation node to exit operation. */
	OperationKey obdata_geom_eval_key(obdata, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_PLACEHOLDER, "Geometry Eval");
	OperationKey obdata_geom_done_key(obdata, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_PLACEHOLDER, "Eval Done");
	add_relation(obdata_geom_eval_key, obdata_geom_done_key, DEPSREL_TYPE_DATABLOCK, "ObData Geom Eval Done");

	/* link components to each other */
	add_relation(obdata_geom_key, geom_key, DEPSREL_TYPE_DATABLOCK, "Object Geometry Base Data");

	/* Init operation of object-level geometry evaluation. */
	OperationKey geom_init_key(&ob->id, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_PLACEHOLDER, "Eval Init");

	/* type-specific node/links */
	switch (ob->type) {
		case OB_MESH:
			break;
		
		case OB_MBALL: 
		{
			Object *mom = BKE_mball_basis_find(scene, ob);
			
			/* motherball - mom depends on children! */
			if (mom != ob) {
				/* non-motherball -> cannot be directly evaluated! */
				ComponentKey mom_key(&mom->id, DEPSNODE_TYPE_GEOMETRY);
				add_relation(geom_key, mom_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Metaball Motherball");
			}
		}
		break;
		
		case OB_CURVE:
		case OB_FONT:
		{
			Curve *cu = (Curve *)obdata;
			
			/* curve's dependencies */
			// XXX: these needs geom data, but where is geom stored?
			if (cu->bevobj) {
				ComponentKey bevob_key(&cu->bevobj->id, DEPSNODE_TYPE_GEOMETRY);
				add_relation(bevob_key, geom_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Curve Bevel");
			}
			if (cu->taperobj) {
				ComponentKey taperob_key(&cu->taperobj->id, DEPSNODE_TYPE_GEOMETRY);
				add_relation(taperob_key, geom_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Curve Taper");
			}
			if (ob->type == OB_FONT) {
				if (cu->textoncurve) {
					ComponentKey textoncurve_key(&cu->taperobj->id, DEPSNODE_TYPE_GEOMETRY);
					add_relation(textoncurve_key, geom_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Text on Curve");
				}
			}
		}
		break;
		
		case OB_SURF: /* Nurbs Surface */
		{
		}
		break;
		
		case OB_LATTICE: /* Lattice */
		{
		}
		break;
	}
	
	/* ShapeKeys */
	Key *key = BKE_key_from_object(ob);
	if (key) {
		build_shapekeys(obdata, key);
	}
	
	/* Modifiers */
	if (ob->modifiers.first) {
		ModifierData *md;
		OperationKey prev_mod_key;
		
		for (md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
			ModifierTypeInfo *mti = modifierType_getInfo((ModifierType)md->type);
			OperationKey mod_key(&ob->id, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_GEOMETRY_MODIFIER, md->name);

			if (md->prev) {
				/* Stack relation: modifier depends on previous modifier in the stack */
				add_relation(prev_mod_key, mod_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Modifier Stack");
			}
			else {
				/* Stack relation: first modifier depends on the geometry. */
				add_relation(geom_init_key, mod_key, DEPSREL_TYPE_GEOMETRY_EVAL, "Modifier Stack");
			}

			if (mti->updateDepsgraph) {
				DepsNodeHandle handle = create_node_handle(mod_key);
				mti->updateDepsgraph(md, scene, ob, &handle);
			}

			if (modifier_check_depends_on_time(ob, md)) {
				TimeSourceKey time_src_key;
				add_relation(time_src_key, mod_key, DEPSREL_TYPE_TIME, "Time Source");
			}

			prev_mod_key = mod_key;
		}
	}
	
	/* materials */
	if (ob->totcol) {
		int a;
		
		for (a = 1; a <= ob->totcol; a++) {
			Material *ma = give_current_material(ob, a);
			
			if (ma)
				build_material(&ob->id, ma);
		}
	}
	
	/* geometry collision */
	if (ELEM(ob->type, OB_MESH, OB_CURVE, OB_LATTICE)) {
		// add geometry collider relations
	}

	/* Make sure uber update is the last in the dependencies.
	 *
	 * TODO(sergey): Get rid of this node.
	 */
	if (ob->type != OB_ARMATURE) {
		/* Armatures does no longer require uber node. */
		OperationKey obdata_ubereval_key(&ob->id, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_GEOMETRY_UBEREVAL);
		if (ob->modifiers.last) {
			ModifierData *md = (ModifierData *)ob->modifiers.last;
			OperationKey mod_key(&ob->id, DEPSNODE_TYPE_GEOMETRY, DEG_OPCODE_GEOMETRY_MODIFIER, md->name);
			add_relation(mod_key, obdata_ubereval_key, DEPSREL_TYPE_OPERATION, "Object Geometry UberEval");
		}
		else {
			add_relation(geom_init_key, obdata_ubereval_key, DEPSREL_TYPE_OPERATION, "Object Geometry UberEval");
		}
	}
}

/* Cameras */
// TODO: Link scene-camera links in somehow...
void DepsgraphRelationBuilder::build_camera(Object *ob)
{
	Camera *cam = (Camera *)ob->data;
	ComponentKey param_key(&cam->id, DEPSNODE_TYPE_PARAMETERS);
	
	/* DOF */
	if (cam->dof_ob) {
		ComponentKey dof_ob_key(&cam->dof_ob->id, DEPSNODE_TYPE_TRANSFORM);
		add_relation(dof_ob_key, param_key, DEPSREL_TYPE_TRANSFORM, "Camera DOF");
	}
}

/* Lamps */
void DepsgraphRelationBuilder::build_lamp(Object *ob)
{
	Lamp *la = (Lamp *)ob->data;
	ID *lamp_id = &la->id;

	/* Prevent infinite recursion by checking (and tagging the lamp) as having been visited 
	 * already. This assumes la->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	if (id_is_tagged(lamp_id))
		return;
	id_tag_set(lamp_id);
	
	/* lamp's nodetree */
	if (la->nodetree) {
		build_nodetree(lamp_id, la->nodetree);
	}
	
	/* textures */
	build_texture_stack(lamp_id, la->mtex);
	
	id_tag_clear(lamp_id);
}

void DepsgraphRelationBuilder::build_nodetree(ID *owner, bNodeTree *ntree)
{
	if (!ntree)
		return;
	
	build_animdata(&ntree->id);
	
	/* nodetree's nodes... */
	for (bNode *bnode = (bNode *)ntree->nodes.first; bnode; bnode = bnode->next) {
		if (bnode->id) {
			if (GS(bnode->id->name) == ID_MA) {
				build_material(owner, (Material *)bnode->id);
			}
			else if (bnode->type == ID_TE) {
				build_texture(owner, (Tex *)bnode->id);
			}
			else if (bnode->type == NODE_GROUP) {
				build_nodetree(owner, (bNodeTree *)bnode->id);
			}
		}
	}
	
	// TODO: link from nodetree to owner_component?
}

/* Recursively build graph for material */
void DepsgraphRelationBuilder::build_material(ID *owner, Material *ma)
{
	/* Prevent infinite recursion by checking (and tagging the material) as having been visited 
	 * already. This assumes ma->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	ID *ma_id = &ma->id;
	if (id_is_tagged(ma_id))
		return;
	id_tag_set(ma_id);
	
	/* animation */
	build_animdata(ma_id);
	
	/* textures */
	build_texture_stack(owner, ma->mtex);
	
	/* material's nodetree */
	build_nodetree(owner, ma->nodetree);
	
	id_tag_clear(ma_id);
}

/* Recursively build graph for texture */
void DepsgraphRelationBuilder::build_texture(ID *owner, Tex *tex)
{
	/* Prevent infinite recursion by checking (and tagging the texture) as having been visited 
	 * already. This assumes tex->id.flag & LIB_DOIT isn't set by anything else
	 * in the meantime... [#32017]
	 */
	ID *tex_id = &tex->id;
	if (id_is_tagged(tex_id))
		return;
	id_tag_set(tex_id);
	
	/* texture itself */
	build_animdata(tex_id);
	
	/* texture's nodetree */
	build_nodetree(owner, tex->nodetree);
	
	id_tag_clear(tex_id);
}

/* Texture-stack attached to some shading datablock */
void DepsgraphRelationBuilder::build_texture_stack(ID *owner, MTex **texture_stack)
{
	int i;
	
	/* for now assume that all texture-stacks have same number of max items */
	for (i = 0; i < MAX_MTEX; i++) {
		MTex *mtex = texture_stack[i];
		if (mtex && mtex->tex)
			build_texture(owner, mtex->tex);
	}
}

void DepsgraphRelationBuilder::build_compositor(Scene *scene)
{
	/* For now, just a plain wrapper? */
	build_nodetree(&scene->id, scene->nodetree);
}

void DepsgraphRelationBuilder::build_gpencil(ID *UNUSED(owner), bGPdata *gpd)
{
	/* animation */
	build_animdata(&gpd->id);
	
	// TODO: parent object (when that feature is implemented)
}
