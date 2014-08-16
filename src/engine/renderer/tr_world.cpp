/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2006-2008 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of Daemon source code.

Daemon source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Daemon source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_world.c

#include "tr_local.h"
#include "gl_shader.h"

/*
================
R_CullSurface

Tries to back face cull surfaces before they are lighted or
added to the sorting list.

This will also allow mirrors on both sides of a model without recursion.
================
*/
static qboolean R_CullSurface( surfaceType_t *surface, shader_t *shader, int *frontFace )
{
	srfGeneric_t *gen;
	int          cull;
	float        d;

	// force to non-front facing
	*frontFace = 0;

	// allow culling to be disabled
	if ( r_nocull->integer )
	{
		return qfalse;
	}

	// ydnar: made surface culling generic, inline with q3map2 surface classification
	if ( *surface == SF_GRID && r_nocurves->integer )
	{
		return qtrue;
	}

	if ( *surface != SF_FACE && *surface != SF_TRIANGLES && *surface != SF_VBO_MESH && *surface != SF_GRID )
	{
		return qtrue;
	}

	// get generic surface
	gen = ( srfGeneric_t * ) surface;

	// plane cull
	if ( *surface == SF_FACE && r_facePlaneCull->integer )
	{
		srfSurfaceFace_t *srf = ( srfSurfaceFace_t * )gen;
		d = DotProduct( tr.orientation.viewOrigin, srf->plane.normal ) - srf->plane.dist;

		if ( d > 0.0f )
		{
			*frontFace = 1;
		}

		// don't cull exactly on the plane, because there are levels of rounding
		// through the BSP, ICD, and hardware that may cause pixel gaps if an
		// epsilon isn't allowed here
		if ( shader->cullType == CT_FRONT_SIDED )
		{
			if ( d < -8.0f )
			{
				tr.pc.c_plane_cull_out++;
				return qtrue;
			}
		}
		else if ( shader->cullType == CT_BACK_SIDED )
		{
			if ( d > 8.0f )
			{
				tr.pc.c_plane_cull_out++;
				return qtrue;
			}
		}

		tr.pc.c_plane_cull_in++;
	}

	if ( *surface == SF_VBO_MESH )
	{
		if ( tr.currentEntity != &tr.worldEntity )
		{
			cull = R_CullLocalBox( gen->bounds );
		}
		else
		{
			cull = R_CullBox( gen->bounds );
		}

		if ( cull == CULL_OUT )
		{
			tr.pc.c_box_cull_out++;
			return qtrue;
		}

		tr.pc.c_box_cull_in++;
	}
	else
	{
		if ( tr.currentEntity != &tr.worldEntity )
		{
			cull = R_CullLocalPointAndRadius( gen->origin, gen->radius );
		}
		else
		{
			cull = R_CullPointAndRadius( gen->origin, gen->radius );
		}

		if ( cull == CULL_OUT )
		{
			tr.pc.c_sphere_cull_out++;
			return qtrue;
		}

		tr.pc.c_sphere_cull_in++;
	}

	// must be visible
	return qfalse;
}

static qboolean R_CullLightSurface( surfaceType_t *surface, shader_t *shader, trRefLight_t *light, byte *cubeSideBits )
{
	srfGeneric_t *gen;
	float        d;

	// allow culling to be disabled
	if ( r_nocull->integer )
	{
		return qfalse;
	}

	// ydnar: made surface culling generic, inline with q3map2 surface classification
	if ( *surface == SF_GRID && r_nocurves->integer )
	{
		return qtrue;
	}

	if ( *surface != SF_FACE && *surface != SF_TRIANGLES && *surface != SF_VBO_MESH && *surface != SF_GRID )
	{
		return qtrue;
	}

	gen = ( srfGeneric_t * ) surface;

	// do a quick AABB cull
	if ( !BoundsIntersect( light->worldBounds[ 0 ], light->worldBounds[ 1 ], gen->bounds[ 0 ], gen->bounds[ 1 ] ) )
	{
		return qtrue;
	}

	// do a more expensive and precise light frustum cull
	if ( !r_noLightFrustums->integer )
	{
		if ( R_CullLightWorldBounds( light, gen->bounds ) == CULL_OUT )
		{
			return qtrue;
		}
	}

	// plane cull
	if ( *surface == SF_FACE && r_facePlaneCull->integer )
	{
		srfSurfaceFace_t *srf = ( srfSurfaceFace_t * )gen;
		if ( light->l.rlType == RL_DIRECTIONAL )
		{
			d = DotProduct( tr.sunDirection, srf->plane.normal );
		}
		else
		{
			d = DotProduct( light->origin, srf->plane.normal ) - srf->plane.dist;
		}
		// don't cull exactly on the plane, because there are levels of rounding
		// through the BSP, ICD, and hardware that may cause pixel gaps if an
		// epsilon isn't allowed here
		if ( shader->cullType == CT_FRONT_SIDED )
		{
			if ( d < -8.0f )
			{
				return qtrue;
			}
		}
		else if ( shader->cullType == CT_BACK_SIDED )
		{
			if ( d > 8.0f )
			{
				return qtrue;
			}
		}
	}

	if ( r_cullShadowPyramidFaces->integer )
	{
		*cubeSideBits = R_CalcLightCubeSideBits( light, gen->bounds );
	}

	return qfalse;
}

/*
======================
R_AddInteractionSurface
======================
*/
static void R_AddInteractionSurface( bspSurface_t *surf, trRefLight_t *light, int interactionBits )
{
	byte              cubeSideBits = CUBESIDE_CLIPALL;
	qboolean          firstAddition = qfalse;
	int               bits;

	if ( surf->lightCount != tr.lightCount )
	{
		surf->interactionBits = 0;
		surf->lightCount = tr.lightCount;
		firstAddition = qtrue;
	}

	// only add interactions we haven't already added
	bits = interactionBits & ~surf->interactionBits;

	if ( !bits )
	{
		// already added these interactions
		return;
	}

	surf->interactionBits |= bits;

	//  skip all surfaces that don't matter for lighting only pass
	if ( surf->shader->isSky || ( !surf->shader->interactLight && surf->shader->noShadows ) )
	{
		return;
	}

	if ( R_CullLightSurface( surf->data, surf->shader, light, &cubeSideBits ) )
	{
		if ( !light->isStatic && firstAddition )
		{
			tr.pc.c_dlightSurfacesCulled++;
		}
		return;
	}

	R_AddLightInteraction( light, surf->data, surf->shader, cubeSideBits, ( interactionType_t ) bits );

	if ( firstAddition )
	{
		if ( light->isStatic )
		{
			tr.pc.c_slightSurfaces++;
		}
		else
		{
			tr.pc.c_dlightSurfaces++;
		}
	}
}

static void R_AddDecalSurface( bspSurface_t *surf, int decalBits )
{
	int i;

	// add decals
	if ( decalBits )
	{
		// ydnar: project any decals
		for ( i = 0; i < tr.refdef.numDecalProjectors; i++ )
		{
			if ( decalBits & ( 1 << i ) )
			{
				R_ProjectDecalOntoSurface( &tr.refdef.decalProjectors[ i ], surf, &tr.world->models[ 0 ] );
			}
		}
	}
}

/*
======================
R_AddWorldSurface
======================
*/
static qboolean R_AddWorldSurface( bspSurface_t *surf )
{
	int      frontFace;

	if ( surf->viewCount == tr.viewCountNoReset )
	{
		return qfalse;
	}

	surf->viewCount = tr.viewCountNoReset;

	// try to cull before lighting or adding
	if ( R_CullSurface( surf->data, surf->shader, &frontFace ) )
	{
		return qtrue;
	}

	R_AddDrawSurf( surf->data, surf->shader, surf->lightmapNum, surf->fogIndex );
	return qtrue;
}

/*
=============================================================

        BRUSH MODELS

=============================================================
*/

/*
======================
R_AddBrushModelSurface
======================
*/
static void R_AddBrushModelSurface( bspSurface_t *surf, int fogIndex )
{
	int frontFace;

	if ( surf->viewCount == tr.viewCountNoReset )
	{
		return; // already in this view
	}

	surf->viewCount = tr.viewCountNoReset;

	// try to cull before lighting or adding
	if ( R_CullSurface( surf->data, surf->shader, &frontFace ) )
	{
		return;
	}

	R_AddDrawSurf( surf->data, surf->shader, surf->lightmapNum, fogIndex );
}

/*
=================
R_AddBSPModelSurfaces
=================
*/
void R_AddBSPModelSurfaces( trRefEntity_t *ent )
{
	bspModel_t *bspModel;
	model_t    *pModel;
	unsigned int i;
	vec3_t     v;
	vec3_t     transformed;
	vec3_t     boundsCenter;
	int        fogNum;

	pModel = R_GetModelByHandle( ent->e.hModel );
	bspModel = pModel->bsp;

	// copy local bounds
	for ( i = 0; i < 3; i++ )
	{
		ent->localBounds[ 0 ][ i ] = bspModel->bounds[ 0 ][ i ];
		ent->localBounds[ 1 ][ i ] = bspModel->bounds[ 1 ][ i ];
	}

	ent->cull = R_CullLocalBox( bspModel->bounds );

	if ( ent->cull == CULL_OUT )
	{
		return;
	}

	// setup world bounds for intersection tests
	ClearBounds( ent->worldBounds[ 0 ], ent->worldBounds[ 1 ] );

	for ( i = 0; i < 8; i++ )
	{
		v[ 0 ] = ent->localBounds[ i & 1 ][ 0 ];
		v[ 1 ] = ent->localBounds[( i >> 1 ) & 1 ][ 1 ];
		v[ 2 ] = ent->localBounds[( i >> 2 ) & 1 ][ 2 ];

		// transform local bounds vertices into world space
		R_LocalPointToWorld( v, transformed );

		AddPointToBounds( transformed, ent->worldBounds[ 0 ], ent->worldBounds[ 1 ] );
	}

	VectorAdd( ent->worldBounds[ 0 ], ent->worldBounds[ 1 ], boundsCenter );
	VectorScale( boundsCenter, 0.5f, boundsCenter );

	// Tr3B: BSP inline models should always use vertex lighting
	R_SetupEntityLighting( &tr.refdef, ent, boundsCenter );

	fogNum = R_FogWorldBox( ent->worldBounds );

	for ( i = 0; i < bspModel->numSurfaces; i++ )
	{
		R_AddBrushModelSurface( bspModel->firstSurface + i, fogNum );
	}
}

/*
=============================================================

        WORLD MODEL

=============================================================
*/

static void R_AddLeafSurfaces( bspNode_t *node, int decalBits )
{
	int          c;
	bspSurface_t **mark;
	bspSurface_t **view;

	tr.pc.c_leafs++;

	// add to z buffer bounds
	if ( node->mins[ 0 ] < tr.viewParms.visBounds[ 0 ][ 0 ] )
	{
		tr.viewParms.visBounds[ 0 ][ 0 ] = node->mins[ 0 ];
	}

	if ( node->mins[ 1 ] < tr.viewParms.visBounds[ 0 ][ 1 ] )
	{
		tr.viewParms.visBounds[ 0 ][ 1 ] = node->mins[ 1 ];
	}

	if ( node->mins[ 2 ] < tr.viewParms.visBounds[ 0 ][ 2 ] )
	{
		tr.viewParms.visBounds[ 0 ][ 2 ] = node->mins[ 2 ];
	}

	if ( node->maxs[ 0 ] > tr.viewParms.visBounds[ 1 ][ 0 ] )
	{
		tr.viewParms.visBounds[ 1 ][ 0 ] = node->maxs[ 0 ];
	}

	if ( node->maxs[ 1 ] > tr.viewParms.visBounds[ 1 ][ 1 ] )
	{
		tr.viewParms.visBounds[ 1 ][ 1 ] = node->maxs[ 1 ];
	}

	if ( node->maxs[ 2 ] > tr.viewParms.visBounds[ 1 ][ 2 ] )
	{
		tr.viewParms.visBounds[ 1 ][ 2 ] = node->maxs[ 2 ];
	}

	// add the individual surfaces
	mark = node->markSurfaces;
	c = node->numMarkSurfaces;
	view = node->viewSurfaces;

	while ( c-- )
	{
		// the surface may have already been added if it
		// spans multiple leafs
		if ( R_AddWorldSurface( *view ) )
		{
			R_AddDecalSurface( *mark, decalBits );
		}

		( *mark )->viewCount = tr.viewCountNoReset;

		mark++;
		view++;
	}
}

/*
================
R_RecursiveWorldNode
================
*/
static void R_RecursiveWorldNode( bspNode_t *node, int planeBits, int decalBits )
{
	do
	{
		// if the node wasn't marked as potentially visible, exit
		if ( node->visCounts[ tr.visIndex ] != tr.visCounts[ tr.visIndex ] )
		{
			return;
		}

		if ( node->contents != -1 && !node->numMarkSurfaces )
		{
			// don't waste time dealing with this empty leaf
			return;
		}

		// if the bounding volume is outside the frustum, nothing
		// inside can be visible OPTIMIZE: don't do this all the way to leafs?
		if ( !r_nocull->integer )
		{
			int i;
			int r;

			for ( i = 0; i < FRUSTUM_PLANES; i++ )
			{
				if ( planeBits & ( 1 << i ) )
				{
					r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustums[ 0 ][ i ] );

					if ( r == 2 )
					{
						return; // culled
					}

					if ( r == 1 )
					{
						planeBits &= ~( 1 << i );  // all descendants will also be in front
					}
				}
			}
		}

		InsertLink( &node->visChain, &tr.traversalStack );

		// ydnar: cull decals
		if ( decalBits )
		{
			int i;

			for ( i = 0; i < tr.refdef.numDecalProjectors; i++ )
			{
				if ( decalBits & ( 1 << i ) )
				{
					// test decal bounds against node bounds
					if ( tr.refdef.decalProjectors[ i ].shader == NULL ||
					     !R_TestDecalBoundingBox( &tr.refdef.decalProjectors[ i ], node->mins, node->maxs ) )
					{
						decalBits &= ~( 1 << i );
					}
				}
			}
		}

		if ( node->contents != -1 )
		{
			break;
		}

		// recurse down the children, front side first
		R_RecursiveWorldNode( node->children[ 0 ], planeBits, decalBits );

		// tail recurse
		node = node->children[ 1 ];
	}
	while ( 1 );

	if ( node->numMarkSurfaces )
	{
		// ydnar: moved off to separate function
		R_AddLeafSurfaces( node, decalBits );
	}
}

/*
================
R_RecursiveInteractionNode
================
*/
static void R_RecursiveInteractionNode( bspNode_t *node, trRefLight_t *light, int planeBits, int interactionBits )
{
	int i;
	int r;

	do
	{
		// surfaces that arn't potentially visible may still cast shadows
		// but we don't bother lighting them since there will be no visible effect
		if ( node->visCounts[ tr.visIndex ] != tr.visCounts[ tr.visIndex ] )
		{
			interactionBits &= ~IA_LIGHT;
		}

		// if the bounding volume is outside the frustum, nothing
		// inside can be visible OPTIMIZE: don't do this all the way to leafs?

		// Tr3B - even surfaces that belong to nodes that are outside of the view frustum
		// can cast shadows into the view frustum
		if ( !r_nocull->integer )
		{
			for ( i = 0; i < FRUSTUM_PLANES; i++ )
			{
				if ( planeBits & ( 1 << i ) )
				{
					r = BoxOnPlaneSide( node->mins, node->maxs, &tr.viewParms.frustums[ 0 ][ i ] );

					if ( r == 2 )
					{
						// this node cannot be lighted, but may cast shadows
						interactionBits &= ~IA_LIGHT;
						break;
					}

					if ( r == 1 )
					{
						planeBits &= ~( 1 << i );  // all descendants will also be in front
					}
				}
			}
		}

		// don't waste time on nodes with no interactions
		if ( !interactionBits )
		{
			return;
		}

		if ( node->contents != -1 )
		{
			break;
		}

		// node is just a decision point, so go down both sides
		// since we don't care about sort orders, just go positive to negative
		r = BoxOnPlaneSide( light->worldBounds[ 0 ], light->worldBounds[ 1 ], node->plane );

		switch ( r )
		{
			case 1:
				node = node->children[ 0 ];
				break;

			case 2:
				node = node->children[ 1 ];
				break;

			case 3:
			default:
				// recurse down the children, front side first
				R_RecursiveInteractionNode( node->children[ 0 ], light, planeBits, interactionBits );

				// tail recurse
				node = node->children[ 1 ];
				break;
		}
	}
	while ( 1 );

	{
		// leaf node, so add mark surfaces
		int          c;
		bspSurface_t *surf, **mark;

		// add the individual surfaces
		mark = node->markSurfaces;
		c = node->numMarkSurfaces;

		while ( c-- )
		{
			// the surface may have already been added if it
			// spans multiple leafs
			surf = *mark;
			R_AddInteractionSurface( surf, light, interactionBits );
			mark++;
		}
	}
}

/*
===============
R_PointInLeaf
===============
*/
static bspNode_t *R_PointInLeaf( const vec3_t p )
{
	bspNode_t *node;
	float     d;
	cplane_t  *plane;

	if ( !tr.world )
	{
		ri.Error( ERR_DROP, "R_PointInLeaf: bad model" );
	}

	node = tr.world->nodes;

	while ( 1 )
	{
		if ( node->contents != -1 )
		{
			break;
		}

		plane = node->plane;
		d = DotProduct( p, plane->normal ) - plane->dist;

		if ( d > 0 )
		{
			node = node->children[ 0 ];
		}
		else
		{
			node = node->children[ 1 ];
		}
	}

	return node;
}

/*
==============
R_ClusterPVS
==============
*/
static const byte *R_ClusterPVS( int cluster )
{
	if ( !tr.world || !tr.world->vis || cluster < 0 || cluster >= tr.world->numClusters )
	{
		return tr.world->novis;
	}

	return tr.world->vis + cluster * tr.world->clusterBytes;
}

/*
==============
R_ClusterPVVS
==============
*/
static const byte *R_ClusterPVVS( int cluster )
{
	if ( !tr.world || !tr.world->vis || cluster < 0 || cluster >= tr.world->numClusters )
	{
		return tr.world->novis;
	}

	return tr.world->visvis + cluster * tr.world->clusterBytes;
}

/*
=================
R_inPVS
=================
*/
qboolean R_inPVS( const vec3_t p1, const vec3_t p2 )
{
	bspNode_t  *leaf;
	const byte *vis;

	leaf = R_PointInLeaf( p1 );
	vis = R_ClusterPVS( leaf->cluster );
	leaf = R_PointInLeaf( p2 );

	if ( !( vis[ leaf->cluster >> 3 ] & ( 1 << ( leaf->cluster & 7 ) ) ) )
	{
		return qfalse;
	}

	return qtrue;
}

/*
=================
R_inPVVS
=================
*/
qboolean R_inPVVS( const vec3_t p1, const vec3_t p2 )
{
	bspNode_t  *leaf;
	const byte *vis;

	leaf = R_PointInLeaf( p1 );
	vis = R_ClusterPVVS( leaf->cluster );
	leaf = R_PointInLeaf( p2 );

	if ( !( vis[ leaf->cluster >> 3 ] & ( 1 << ( leaf->cluster & 7 ) ) ) )
	{
		return qfalse;
	}

	return qtrue;
}

/*
===============
R_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current
cluster
===============
*/
static void R_MarkLeaves( void )
{
	const byte *vis;
	bspNode_t  *leaf, *parent;
	int        i;
	int        cluster;

	// lockpvs lets designers walk around to determine the
	// extent of the current pvs
	if ( r_lockpvs->integer )
	{
		return;
	}

	// current viewcluster
	leaf = R_PointInLeaf( tr.viewParms.pvsOrigin );
	cluster = leaf->cluster;

	// if the cluster is the same and the area visibility matrix
	// hasn't changed, we don't need to mark everything again
	if( tr.refdef.areamaskModified ) {
		// remark ALL cached visClusters
		for ( i = 0; i < MAX_VISCOUNTS; i++ ) {
			tr.visClusters[ i ] = -1;
		}
		tr.visIndex = 1;
	} else {
		for ( i = 0; i < MAX_VISCOUNTS; i++ ) {
			if ( tr.visClusters[ i ] == cluster ) {
				// if r_showcluster was just turned on, remark everything
				if ( !r_showcluster->modified )
				{
					if ( tr.visClusters[ i ] != tr.visClusters[ tr.visIndex ] && r_showcluster->integer )
					{
						ri.Printf( PRINT_ALL, "found cluster:%i  area:%i  index:%i\n", cluster, leaf->area, i );
					}

					tr.visIndex = i;
					return;
				}
			}
		}
		tr.visIndex = ( tr.visIndex + 1 ) % MAX_VISCOUNTS;
	}

	tr.visClusters[ tr.visIndex ] = cluster;
	tr.visCounts[ tr.visIndex ]++;

	if ( r_showcluster->modified || r_showcluster->integer )
	{
		r_showcluster->modified = qfalse;

		if ( r_showcluster->integer )
		{
			ri.Printf( PRINT_ALL, "update cluster:%i  area:%i  index:%i\n", cluster, leaf->area, tr.visIndex );
		}
	}

	if ( r_novis->integer || tr.visClusters[ tr.visIndex ] == -1 )
	{
		for ( i = 0; i < tr.world->numnodes; i++ )
		{
			if ( tr.world->nodes[ i ].contents != CONTENTS_SOLID )
			{
				tr.world->nodes[ i ].visCounts[ tr.visIndex ] = tr.visCounts[ tr.visIndex ];
			}
		}

		return;
	}

	vis = R_ClusterPVS( tr.visClusters[ tr.visIndex ] );

	for ( i = 0, leaf = tr.world->nodes; i < tr.world->numnodes; i++, leaf++ )
	{
		if ( tr.world->vis )
		{
			cluster = leaf->cluster;

			if ( cluster >= 0 && cluster < tr.world->numClusters )
			{
				// check general pvs
				if ( !( vis[ cluster >> 3 ] & ( 1 << ( cluster & 7 ) ) ) )
				{
					continue;
				}
			}
		}

		// check for door connection
		if ( ( tr.refdef.areamask[ leaf->area >> 3 ] & ( 1 << ( leaf->area & 7 ) ) ) )
		{
			// not visible
			continue;
		}

		// ydnar: don't want to walk the entire bsp to add skybox surfaces
		if ( tr.refdef.rdflags & RDF_SKYBOXPORTAL )
		{
			// this only happens once, as game/cgame know the origin of the skybox
			// this also means the skybox portal cannot move, as this list is calculated once and never again
			if ( tr.world->numSkyNodes < WORLD_MAX_SKY_NODES )
			{
				tr.world->skyNodes[ tr.world->numSkyNodes++ ] = leaf;
			}

			R_AddLeafSurfaces( leaf, 0 );
			continue;
		}

		parent = leaf;

		do
		{
			if ( parent->visCounts[ tr.visIndex ] == tr.visCounts[ tr.visIndex ] )
			{
				break;
			}

			parent->visCounts[ tr.visIndex ] = tr.visCounts[ tr.visIndex ];
			parent = parent->parent;
		}
		while ( parent );
	}
}

static void DrawLeaf( bspNode_t *node )
{
	// leaf node, so add mark surfaces
	int          c;
	bspSurface_t *surf, **mark;

	tr.pc.c_leafs++;

	// add to z buffer bounds
	if ( node->mins[ 0 ] < tr.viewParms.visBounds[ 0 ][ 0 ] )
	{
		tr.viewParms.visBounds[ 0 ][ 0 ] = node->mins[ 0 ];
	}

	if ( node->mins[ 1 ] < tr.viewParms.visBounds[ 0 ][ 1 ] )
	{
		tr.viewParms.visBounds[ 0 ][ 1 ] = node->mins[ 1 ];
	}

	if ( node->mins[ 2 ] < tr.viewParms.visBounds[ 0 ][ 2 ] )
	{
		tr.viewParms.visBounds[ 0 ][ 2 ] = node->mins[ 2 ];
	}

	if ( node->maxs[ 0 ] > tr.viewParms.visBounds[ 1 ][ 0 ] )
	{
		tr.viewParms.visBounds[ 1 ][ 0 ] = node->maxs[ 0 ];
	}

	if ( node->maxs[ 1 ] > tr.viewParms.visBounds[ 1 ][ 1 ] )
	{
		tr.viewParms.visBounds[ 1 ][ 1 ] = node->maxs[ 1 ];
	}

	if ( node->maxs[ 2 ] > tr.viewParms.visBounds[ 1 ][ 2 ] )
	{
		tr.viewParms.visBounds[ 1 ][ 2 ] = node->maxs[ 2 ];
	}

	// add the individual surfaces
	mark = node->viewSurfaces;
	c = node->numMarkSurfaces;

	while ( c-- )
	{
		// the surface may have already been added if it
		// spans multiple leafs
		surf = *mark;
		R_AddWorldSurface( surf );
		mark++;
	}
}

/*
=============
R_AddWorldSurfaces
=============
*/
void R_AddWorldSurfaces( void )
{
	if ( !r_drawworld->integer )
	{
		return;
	}

	if ( tr.refdef.rdflags & RDF_NOWORLDMODEL )
	{
		return;
	}

	tr.currentEntity = &tr.worldEntity;

	// clear out the visible min/max
	ClearBounds( tr.viewParms.visBounds[ 0 ], tr.viewParms.visBounds[ 1 ] );

	// render sky or world?
	if ( tr.refdef.rdflags & RDF_SKYBOXPORTAL && tr.world->numSkyNodes > 0 )
	{
		int       i;
		bspNode_t **node;

		for ( i = 0, node = tr.world->skyNodes; i < tr.world->numSkyNodes; i++, node++ )
		{
			R_AddLeafSurfaces( *node, 0 );  // no decals on skybox nodes
		}
	}
	else
	{
		// determine which leaves are in the PVS / areamask
		R_MarkLeaves();

		ClearLink( &tr.traversalStack );

		// update visbounds and add surfaces that weren't cached with VBOs
		R_RecursiveWorldNode( tr.world->nodes, FRUSTUM_CLIPALL, tr.refdef.decalBits );

		// ydnar: add decal surfaces
		R_AddDecalSurfaces( tr.world->models );
	}
}

/*
=============
R_AddWorldInteractions
=============
*/
void R_AddWorldInteractions( trRefLight_t *light )
{
	int interactionBits;

	if ( !r_drawworld->integer )
	{
		return;
	}

	if ( tr.refdef.rdflags & RDF_NOWORLDMODEL )
	{
		return;
	}

	tr.currentEntity = &tr.worldEntity;

	// perform frustum culling and add all the potentially visible surfaces
	tr.lightCount++;

	interactionBits = IA_DEFAULT;

	if ( light->restrictInteractionFirst >= 0 )
	{
		interactionBits = IA_DEFAULTCLIP;
	}

	if ( r_shadows->integer <= SHADOWING_BLOB || light->l.noShadows )
	{
		interactionBits = interactionBits & IA_LIGHT;
	}

	R_RecursiveInteractionNode( tr.world->nodes, light, FRUSTUM_CLIPALL, interactionBits );
}

/*
=============
R_AddPrecachedWorldInteractions
=============
*/
void R_AddPrecachedWorldInteractions( trRefLight_t *light )
{
	interactionType_t iaType = IA_DEFAULT;

	if ( !r_drawworld->integer )
	{
		return;
	}

	if ( tr.refdef.rdflags & RDF_NOWORLDMODEL )
	{
		return;
	}

	if ( !light->firstInteractionCache )
	{
		// this light has no interactions precached
		return;
	}

	tr.currentEntity = &tr.worldEntity;

	if ( ( r_vboShadows->integer || r_vboLighting->integer ) )
	{
		interactionCache_t *iaCache;
		interactionVBO_t   *iaVBO;
		srfVBOMesh_t       *srf;
		shader_t           *shader;
		bspSurface_t       *surface;

		// this can be shadow mapping or shadowless lighting
		for ( iaVBO = light->firstInteractionVBO; iaVBO; iaVBO = iaVBO->next )
		{
			if ( !iaVBO->vboLightMesh )
			{
				continue;
			}

			srf = iaVBO->vboLightMesh;
			shader = iaVBO->shader;

			switch ( light->l.rlType )
			{
				case RL_OMNI:
					R_AddLightInteraction( light, ( surfaceType_t * ) srf, shader, CUBESIDE_CLIPALL, IA_LIGHT );
					break;

				case RL_DIRECTIONAL:
				case RL_PROJ:
					R_AddLightInteraction( light, ( surfaceType_t * ) srf, shader, CUBESIDE_CLIPALL, IA_LIGHT );
					break;

				default:
					R_AddLightInteraction( light, ( surfaceType_t * ) srf, shader, CUBESIDE_CLIPALL, IA_DEFAULT );
					break;
			}
		}

		// add meshes for shadowmap generation if any
		for ( iaVBO = light->firstInteractionVBO; iaVBO; iaVBO = iaVBO->next )
		{
			if ( !iaVBO->vboShadowMesh )
			{
				continue;
			}

			srf = iaVBO->vboShadowMesh;
			shader = iaVBO->shader;

			R_AddLightInteraction( light, ( surfaceType_t * ) srf, shader, iaVBO->cubeSideBits, IA_SHADOW );
		}

		// add interactions that couldn't be merged into VBOs
		for ( iaCache = light->firstInteractionCache; iaCache; iaCache = iaCache->next )
		{
			if ( iaCache->redundant )
			{
				continue;
			}

			if ( iaCache->mergedIntoVBO )
			{
				continue;
			}

			surface = iaCache->surface;

			// Tr3B - this surface is maybe not in this view but it may still cast a shadow
			// into this view
			if ( surface->viewCount != tr.viewCountNoReset )
			{
				if ( r_shadows->integer < SHADOWING_ESM16 || light->l.noShadows )
				{
					continue;
				}
				else
				{
					iaType = IA_SHADOW;
				}
			}
			else
			{
				iaType = iaCache->type;
			}

			R_AddLightInteraction( light, surface->data, surface->shader, iaCache->cubeSideBits, iaType );
		}
	}
	else
	{
		interactionCache_t *iaCache;
		bspSurface_t       *surface;

		for ( iaCache = light->firstInteractionCache; iaCache; iaCache = iaCache->next )
		{
			if ( iaCache->redundant )
			{
				continue;
			}

			surface = iaCache->surface;

			// Tr3B - this surface is maybe not in this view but it may still cast a shadow
			// into this view
			if ( surface->viewCount != tr.viewCountNoReset )
			{
				if ( r_shadows->integer < SHADOWING_ESM16 || light->l.noShadows )
				{
					continue;
				}
				else
				{
					iaType = IA_SHADOW;
				}
			}
			else
			{
				iaType = iaCache->type;
			}

			R_AddLightInteraction( light, surface->data, surface->shader, iaCache->cubeSideBits, iaType );
		}
	}
}
