// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2018 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_drawinfo.cpp
** Basic scene draw info management class
**
*/

#include "a_sharedglobal.h"
#include "r_utility.h"
#include "r_sky.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "hw_fakeflat.h"
#include "hw_portal.h"
#include "hw_renderstate.h"
#include "hw_drawinfo.h"
#include "hw_drawstructs.h"
#include "po_man.h"
#include "models.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_vrmodes.h"
#include "hw_clipper.h"
#include "v_draw.h"
#include "a_corona.h"
#include "texturemanager.h"
#include "actorinlines.h"
#include "g_levellocals.h"

EXTERN_CVAR(Float, r_visibility)
CVAR(Bool, gl_bandedswlight, false, CVAR_ARCHIVE)
CVAR(Bool, gl_sort_textures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_no_skyclear, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_enhanced_nv_stealth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_texture, true, 0)
CVAR(Float, gl_mask_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_mask_sprite_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_coronas, true, CVAR_ARCHIVE);

// [Nakara] Through-wall redraw test path.
// This intentionally does not modify or reuse FocusHighlight shader data.
CVAR(Bool, nk_vis_thru_wall_hidden_redraw, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_vis_thru_wall_sprite_occlusion, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nk_vis_thru_wall_max_candidates, 16, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] Category filter for +VISTHRUWALL targets. Uses ENakaraVisThruWallMask bits:
// VIS_NONE=0, VIS_MONST=1, VIS_ITEM=2, VIS_OBJ=4, VIS_SECRET=8, VIS_NPC=16, VIS_ALL=31.
// With the default VIS_ALL filter, actors are revealed whenever their own
// VisThruWallMask is not VIS_NONE. Set this CVar to 0 to globally hide them.
CVAR(Int, nk_vis_thru_wall_filter_mask, VIS_ALL, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] Separate limit for ordinary sprite mask candidates. Normal sprite
// masks are much more numerous than +VISTHRUWALL targets, so sharing the target
// limit can make sprite-behind-sprite reveal fail depending on thinker order.
CVAR(Int, nk_vis_thru_wall_sprite_mask_max_candidates, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] Experimental throttle for the ordinary sprite mask pass. 1 means
// every frame. Higher values skip rebuilding the sprite mask on intermediate
// frames; wall/geometry through-wall redraw still runs normally.
CVAR(Int, nk_vis_thru_wall_sprite_mask_update_interval, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] Optional smart filter for the ordinary sprite mask. Instead of
// rebuilding a mask for every sprite thinker, only sprites close to or screen-
// aligned with a +VISTHRUWALL target are copied into the extra mask list.
CVAR(Bool, nk_vis_thru_wall_sprite_mask_smart_filter, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_vis_thru_wall_sprite_mask_target_radius, 512.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_vis_thru_wall_sprite_mask_angle, 8.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_vis_thru_wall_sprite_mask_depth_margin, 128.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] When a full +VISTHRUWALL scan produces no redraw items, skip the
// expensive first thinker scan for a few frames. 0 disables the cooldown.
CVAR(Int, nk_vis_thru_wall_empty_scan_interval, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] Global fade used when nk_vis_thru_wall_hidden_redraw is toggled by
// gameplay items. The feature remains rendered while fading out instead of
// disappearing instantly when the CVar becomes false.
CVAR(Int, nk_vis_thru_wall_global_fade_in_tics, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nk_vis_thru_wall_global_fade_out_tics, 6, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [Nakara] Console stats for tuning VisThruWall performance. Printed at a
// throttled interval to avoid flooding the console while profiling.
CVAR(Bool, nk_vis_thru_wall_debug_stats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nk_vis_thru_wall_debug_stats_interval, 35, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static uint32_t NkVisThruWallFilterMask()
{
	return uint32_t(max<int>(nk_vis_thru_wall_filter_mask, 0));
}

static bool NkVisThruWallFilterEnabled()
{
	return NkVisThruWallFilterMask() != VIS_NONE;
}

static bool NkVisThruWallMatchesFilter(AActor *thing)
{
	return thing != nullptr && thing->VisThruWallMask != VIS_NONE && (thing->VisThruWallMask & NkVisThruWallFilterMask()) != 0;
}

sector_t * hw_FakeFlat(sector_t * sec, sector_t * dest, area_t in_area, bool back);

//==========================================================================
//
//
//
//==========================================================================

class FDrawInfoList
{
public:
	TDeletingArray<HWDrawInfo *> mList;

	HWDrawInfo * GetNew();
	void Release(HWDrawInfo *);
};


FDrawInfoList di_list;

//==========================================================================
//
// Try to reuse the lists as often as possible as they contain resources that
// are expensive to create and delete.
//
// Note: If multithreading gets used, this class needs synchronization.
//
//==========================================================================

HWDrawInfo *FDrawInfoList::GetNew()
{
	if (mList.Size() > 0)
	{
		HWDrawInfo *di;
		mList.Pop(di);
		return di;
	}
	return new HWDrawInfo();
}

void FDrawInfoList::Release(HWDrawInfo * di)
{
	di->ClearBuffers();
	di->Level = nullptr;
	mList.Push(di);
}

//==========================================================================
//
// Sets up a new drawinfo struct
//
//==========================================================================

HWDrawInfo *HWDrawInfo::StartDrawInfo(FLevelLocals *lev, HWDrawInfo *parent, FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	HWDrawInfo *di = di_list.GetNew();
	di->Level = lev;
	di->StartScene(parentvp, uniforms);
	return di;
}


//==========================================================================
//
//
//
//==========================================================================

static Clipper staticClipper;		// Since all scenes are processed sequentially we only need one clipper.
static Clipper staticVClipper;		// Another clipper to clip vertically (used if (VPSF_ALLOWOUTOFBOUNDS & camera->viewpos->Flags)).
static Clipper staticRClipper;		// Another clipper for radar (doesn't actually clip. Changes SSECMF_DRAWN setting).
static HWDrawInfo * gl_drawinfo;	// This is a linked list of all active DrawInfos and needed to free the memory arena after the last one goes out of scope.

void HWDrawInfo::StartScene(FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	staticClipper.Clear();
	staticVClipper.Clear();
	staticRClipper.Clear();
	mClipper = &staticClipper;
	vClipper = &staticVClipper;
	rClipper = &staticRClipper;
	rClipper->amRadar = true;

	Viewpoint = parentvp;
	lightmode = getRealLightmode(Level, true);

	if (uniforms)
	{
		VPUniforms = *uniforms;
		// The clip planes will never be inherited from the parent drawinfo.
		VPUniforms.mClipLine.X = -1000001.f;
		VPUniforms.mClipHeight = 0;
	}
	else
	{
		VPUniforms.mProjectionMatrix.loadIdentity();
		VPUniforms.mViewMatrix.loadIdentity();
		VPUniforms.mNormalViewMatrix.loadIdentity();
		ProjectionMatrix2.loadIdentity();
		VPUniforms.mViewHeight = viewheight;
		if (lightmode == ELightMode::Build)
		{
			VPUniforms.mGlobVis = 1 / 64.f;
			VPUniforms.mPalLightLevels = 32 | (static_cast<int>(gl_fogmode) << 8) | ((int)lightmode << 16);
		}
		else
		{
			VPUniforms.mGlobVis = (float)R_GetGlobVis(r_viewwindow, r_visibility) / 32.f;
			VPUniforms.mPalLightLevels = static_cast<int>(gl_bandedswlight) | (static_cast<int>(gl_fogmode) << 8) | ((int)lightmode << 16);
		}
		VPUniforms.mClipLine.X = -10000000.0f;
		VPUniforms.mShadowmapFilter = gl_shadowmap_filter;
		VPUniforms.mLightBlendMode = (level.info ? (int)level.info->lightblendmode : 0);
	}
	mClipper->SetViewpoint(Viewpoint);
	vClipper->SetViewpoint(Viewpoint);
	rClipper->SetViewpoint(Viewpoint);

	ClearBuffers();

	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	hudsprites.Clear();
//	Coronas.Clear();
	vpIndex = 0;

	// Fullbright information needs to be propagated from the main view.
	if (outer != nullptr) FullbrightFlags = outer->FullbrightFlags;
	else FullbrightFlags = 0;

	outer = gl_drawinfo;
	gl_drawinfo = this;

}

//==========================================================================
//
//
//
//==========================================================================

HWDrawInfo *HWDrawInfo::EndDrawInfo()
{
	assert(this == gl_drawinfo);
	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	gl_drawinfo = outer;
	di_list.Release(this);
	if (gl_drawinfo == nullptr)
		ResetRenderDataAllocator();
	return gl_drawinfo;
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ClearBuffers()
{
    otherFloorPlanes.Clear();
    otherCeilingPlanes.Clear();
    floodFloorSegs.Clear();
    floodCeilingSegs.Clear();

	// clear all the lists that might not have been cleared already
	MissingUpperTextures.Clear();
	MissingLowerTextures.Clear();
	MissingUpperSegs.Clear();
	MissingLowerSegs.Clear();
	SubsectorHacks.Clear();
	//CeilingStacks.Clear();
	//FloorStacks.Clear();
	HandledSubsectors.Clear();
	spriteindex = 0;

	if (Level)
	{
		CurrentMapSections.Resize(Level->NumMapSections);
		CurrentMapSections.Zero();

		section_renderflags.Resize(Level->sections.allSections.Size());
		ss_renderflags.Resize(Level->subsectors.Size());
		no_renderflags.Resize(Level->subsectors.Size());

		memset(&section_renderflags[0], 0, Level->sections.allSections.Size() * sizeof(section_renderflags[0]));
		memset(&ss_renderflags[0], 0, Level->subsectors.Size() * sizeof(ss_renderflags[0]));
		memset(&no_renderflags[0], 0, Level->nodes.Size() * sizeof(no_renderflags[0]));
	}

	Decals[0].Clear();
	Decals[1].Clear();

	mClipPortal = nullptr;
	mCurrentPortal = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::UpdateCurrentMapSection()
{
	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.IsAllowedOoB() || Viewpoint.IsOrtho())
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
}


//-----------------------------------------------------------------------------
//
// Sets the area the camera is in
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewArea()
{
	auto &vp = Viewpoint;
	// The render_sector is better suited to represent the current position in GL
	vp.sector = Level->PointInRenderSubsector(vp.Pos)->render_sector;
	if (Viewpoint.IsAllowedOoB())
		vp.sector = Level->PointInRenderSubsector(vp.camera->Pos())->render_sector;

	// Get the heightsec state from the render sector, not the current one!
	if (vp.sector->GetHeightSec())
	{
		in_area = vp.Pos.Z <= vp.sector->heightsec->floorplane.ZatPoint(vp.Pos) ? area_below :
			(vp.Pos.Z > vp.sector->heightsec->ceilingplane.ZatPoint(vp.Pos) &&
				!(vp.sector->heightsec->MoreFlags&SECMF_FAKEFLOORONLY)) ? area_above : area_normal;
	}
	else
	{
		in_area = Level->HasHeightSecs ? area_default : area_normal;	// depends on exposed lower sectors, if map contains heightsecs.
	}
}

//-----------------------------------------------------------------------------
//
// 
//
//-----------------------------------------------------------------------------

int HWDrawInfo::SetFullbrightFlags(player_t *player)
{
	FullbrightFlags = 0;

	// check for special colormaps
	player_t * cplayer = player? player->camera->player : nullptr;
	if (cplayer)
	{
		int cm = CM_DEFAULT;
		if (cplayer->extralight == INT_MIN)
		{
			cm = CM_FIRSTSPECIALCOLORMAP + REALINVERSECOLORMAP;
			Viewpoint.extralight = 0;
			FullbrightFlags = Fullbright;
			// This does never set stealth vision.
		}
		else if (cplayer->fixedcolormap != NOFIXEDCOLORMAP)
		{
			cm = CM_FIRSTSPECIALCOLORMAP + cplayer->fixedcolormap;
			FullbrightFlags = Fullbright;
			if (gl_enhanced_nv_stealth > 2) FullbrightFlags |= StealthVision;
		}
		else if (cplayer->fixedlightlevel != -1)
		{
			auto torchtype = PClass::FindActor(NAME_PowerTorch);
			auto litetype = PClass::FindActor(NAME_PowerLightAmp);
			for (AActor *in = cplayer->mo->Inventory; in; in = in->Inventory)
			{
				// Need special handling for light amplifiers 
				if (in->IsKindOf(torchtype))
				{
					FullbrightFlags = Fullbright;
					if (gl_enhanced_nv_stealth > 1) FullbrightFlags |= StealthVision;
				}
				else if (in->IsKindOf(litetype))
				{
					FullbrightFlags = Fullbright;
					if (gl_enhanced_nightvision) FullbrightFlags |= Nightvision;
					if (gl_enhanced_nv_stealth > 0) FullbrightFlags |= StealthVision;
				}
			}
		}
		return cm;
	}
	else
	{
		return CM_DEFAULT;
	}
}

//-----------------------------------------------------------------------------
//
// R_FrustumAngle
//
//-----------------------------------------------------------------------------

angle_t OoBFrustumAngle(FRenderViewpoint* Viewpoint)
{
	// If pitch is larger than this you can look all around at an FOV of 90 degrees
	if (fabs(Viewpoint->HWAngles.Pitch.Degrees()) > 89.0)  return 0xffffffff;
	int aspMult = AspectMultiplier(r_viewwindow.WidescreenRatio); // 48 == square window
	double absPitch = fabs(Viewpoint->HWAngles.Pitch.Degrees());
	 // Smaller aspect ratios still clip too much. Need a better solution
	if (aspMult > 36 && absPitch > 30.0)  return 0xffffffff;
	else if (aspMult > 40 && absPitch > 25.0)  return 0xffffffff;
	else if (aspMult > 45 && absPitch > 20.0)  return 0xffffffff;
	else if (aspMult > 47 && absPitch > 10.0) return 0xffffffff;

	double xratio = r_viewwindow.FocalTangent / Viewpoint->PitchCos;
	double floatangle = 0.05 + atan ( xratio ) * 48.0 / aspMult; // this is radians
	angle_t a1 = DAngle::fromRad(floatangle).BAMs();

	if (a1 >= ANGLE_90) return 0xffffffff;
	return a1;
}

angle_t HWDrawInfo::FrustumAngle()
{
	if (Viewpoint.IsAllowedOoB())
	{
		return OoBFrustumAngle(&Viewpoint);
	}
	else
	{
		float tilt = fabs(Viewpoint.HWAngles.Pitch.Degrees());

		// If the pitch is larger than this you can look all around at a FOV of 90°
		if (tilt > 46.0f) return 0xffffffff;

		// ok, this is a gross hack that barely works...
		// but at least it doesn't overestimate too much...
		double floatangle = 2.0 + (45.0 + ((tilt / 1.9)))*Viewpoint.FieldOfView.Degrees() * 48.0 / AspectMultiplier(r_viewwindow.WidescreenRatio) / 90.0;
		angle_t a1 = DAngle::fromDeg(floatangle).BAMs();
		if (a1 >= ANGLE_180) return 0xffffffff;
		return a1;
	}
}

//-----------------------------------------------------------------------------
//
// Setup the modelview matrix
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewMatrix(const FRotator &angles, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	float mult = mirror ? -1.f : 1.f;
	float planemult = planemirror ? -Level->info->pixelstretch : Level->info->pixelstretch;

	VPUniforms.mViewMatrix.loadIdentity();
	VPUniforms.mViewMatrix.rotate(angles.Roll.Degrees(), 0.0f, 0.0f, 1.0f);
	VPUniforms.mViewMatrix.rotate(angles.Pitch.Degrees(), 1.0f, 0.0f, 0.0f);
	VPUniforms.mViewMatrix.rotate(angles.Yaw.Degrees(), 0.0f, mult, 0.0f);
	VPUniforms.mViewMatrix.translate(vx * mult, -vz * planemult, -vy);
	VPUniforms.mViewMatrix.scale(-mult, planemult, 1);
}


//-----------------------------------------------------------------------------
//
// SetupView
// Setup the view rotation matrix for the given viewpoint
//
//-----------------------------------------------------------------------------
void HWDrawInfo::SetupView(FRenderState &state, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	auto &vp = Viewpoint;
	vp.SetViewAngle(r_viewwindow);
	SetViewMatrix(vp.HWAngles, vx, vy, vz, mirror, planemirror);
	SetCameraPos(vp.Pos);
	VPUniforms.CalcDependencies();
	vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWPortal * HWDrawInfo::FindPortal(const void * src)
{
	int i = Portals.Size() - 1;

	while (i >= 0 && Portals[i] && Portals[i]->GetSource() != src) i--;
	return i >= 0 ? Portals[i] : nullptr;
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWDecal *HWDrawInfo::AddDecal(bool onmirror)
{
	auto decal = (HWDecal*)RenderDataAllocator.Alloc(sizeof(HWDecal));
	Decals[onmirror ? 1 : 0].Push(decal);
	return decal;
}

//-----------------------------------------------------------------------------
//
// CreateScene
//
// creates the draw lists for the current scene
//
//-----------------------------------------------------------------------------

void HWDrawInfo::CreateScene(bool drawpsprites)
{
	const auto &vp = Viewpoint;
	angle_t a1 = FrustumAngle(); // horizontally clip the back of the viewport
	mClipper->SafeAddClipRangeRealAngles(vp.Angles.Yaw.BAMs() + a1, vp.Angles.Yaw.BAMs() - a1);
	Viewpoint.FrustAngle = a1;
	if (Viewpoint.IsAllowedOoB()) // No need for vertical clipper if viewpoint not allowed out of bounds
	{
		double a2 = 20.0 + 0.5*Viewpoint.FieldOfView.Degrees(); // FrustumPitch for vertical clipping
		if (a2 > 179.0) a2 = 179.0;
		double pitchmult = !!(portalState.PlaneMirrorFlag & 1) ? -1.0 : 1.0;
		vClipper->SafeAddClipRangeDegPitches(pitchmult * vp.HWAngles.Pitch.Degrees() - a2, pitchmult * vp.HWAngles.Pitch.Degrees() + a2); // clip the suplex range
		Viewpoint.PitchSin *= pitchmult;
	}

	// reset the portal manager
	portalState.StartFrame();

	ProcessAll.Clock();

	// clip the scene and fill the drawlists
	screen->mVertexData->Map();
	screen->mLights->Map();
	screen->mBones->Map();

	RenderBSP(Level->HeadNode(), drawpsprites);

	// [Nakara] Reset per-frame VisThruWall debug counters before building the
	// extra draw lists. These counters are only printed when
	// nk_vis_thru_wall_debug_stats is true.
	VisThruWallDebugTargetsSeen = 0;
	VisThruWallDebugTargetsAccepted = 0;
	VisThruWallDebugTargetsFadeSkipped = 0;
	VisThruWallDebugOccludersSeen = 0;
	VisThruWallDebugOccludersAccepted = 0;
	VisThruWallDebugSmartTargets = 0;
	VisThruWallDebugSpriteMaskSeen = 0;
	VisThruWallDebugSpriteMaskAccepted = 0;
	VisThruWallDebugSpriteMaskSmartRejected = 0;
	VisThruWallDebugSpriteMaskFlagSkipped = 0;
	VisThruWallDebugSpriteMaskViewSkipped = 0;
	VisThruWallDebugSpriteMaskUpdateSkipped = false;
	VisThruWallDebugSkippedByEmptyCooldown = false;

	// [Nakara] Build +VISTHRUWALL candidates while render buffers are mapped.
	// Do not build this inside RenderTranslucent: HWSprite::Process may allocate
	// vertices/lights, and doing that during the draw phase can stall the renderer.
	//
	// The modern gameplay path is actor-mask driven: +VISTHRUWALL actors are
	// revealed when their VisThruWallMask is not VIS_NONE and it matches
	// nk_vis_thru_wall_filter_mask. nk_vis_thru_wall_hidden_redraw is kept as a
	// legacy/global force-on switch, but it is no longer required for masked actors.
	int currentVisThruFadeTic = Level->maptime;
	if (VisThruWallLastGlobalFadeTic != currentVisThruFadeTic)
	{
		int ticDelta = VisThruWallLastGlobalFadeTic <= 0 ? 1 : max<int>(currentVisThruFadeTic - VisThruWallLastGlobalFadeTic, 1);
		VisThruWallLastGlobalFadeTic = currentVisThruFadeTic;

		bool visThruWallGlobalShouldShow = nk_vis_thru_wall_hidden_redraw || NkVisThruWallFilterEnabled();

		if (visThruWallGlobalShouldShow)
		{
			int fadeInTics = max<int>(nk_vis_thru_wall_global_fade_in_tics, 0);
			if (fadeInTics <= 0)
			{
				VisThruWallGlobalFade = 1.0;
			}
			else
			{
				VisThruWallGlobalFade = min<double>(VisThruWallGlobalFade + double(ticDelta) / double(fadeInTics), 1.0);
			}
		}
		else
		{
			int fadeOutTics = max<int>(nk_vis_thru_wall_global_fade_out_tics, 0);
			if (fadeOutTics <= 0)
			{
				VisThruWallGlobalFade = 0.0;
			}
			else
			{
				VisThruWallGlobalFade = max<double>(VisThruWallGlobalFade - double(ticDelta) / double(fadeOutTics), 0.0);
			}
		}
	}

	bool visThruWallRuntimeActive = nk_vis_thru_wall_hidden_redraw || NkVisThruWallFilterEnabled() || VisThruWallGlobalFade > 0.0;

	if (visThruWallRuntimeActive)
	{
		if (VisThruWallEmptyScanCooldown > 0)
		{
			VisThruWallDebugSkippedByEmptyCooldown = true;
			// [Nakara] The previous full scan produced no redraw items. Avoid repeating
			// the first full thinker scan every single frame when the feature is enabled
			// but no through-wall target is currently active/visible.
			VisThruWallEmptyScanCooldown--;
			VisThruWallDrawList.Reset();
			VisThruWallOccluderDrawList.Reset();
			VisThruWallSpriteMaskDrawList.Reset();
		}
		else
		{
			BuildVisThruWallDrawList();

			// [Nakara] Optimization: if no +VISTHRUWALL actor produced a redraw item,
			// the sprite mask and +NOVISTHRU occluder passes cannot affect the frame.
			// Skip their thinker scans and draw-list builds entirely. This keeps the
			// feature cheap when it is enabled but no valid through-wall target is active.
			if (VisThruWallDrawList.Size() > 0)
			{
				VisThruWallEmptyScanCooldown = 0;
				BuildVisThruWallOccluderDrawList();

				// [Nakara] Optional sprite-mask update throttle. The sprite mask draw list
				// is backed by the current frame's vertex buffers, so it cannot be
				// safely cached across frames. On skipped frames, only the sprite-occlusion
				// part is omitted; wall/geometry through-wall redraw still works.
				int spriteMaskInterval = clamp<int>(nk_vis_thru_wall_sprite_mask_update_interval, 1, 60);
				if (!nk_vis_thru_wall_sprite_occlusion || spriteMaskInterval <= 1)
				{
					VisThruWallSpriteMaskUpdateCountdown = 0;
					BuildVisThruWallSpriteMaskDrawList();
				}
				else if (VisThruWallSpriteMaskUpdateCountdown > 0)
				{
					VisThruWallSpriteMaskUpdateCountdown--;
					VisThruWallSpriteMaskDrawList.Reset();
					VisThruWallDebugSpriteMaskUpdateSkipped = true;
				}
				else
				{
					VisThruWallSpriteMaskUpdateCountdown = spriteMaskInterval - 1;
					BuildVisThruWallSpriteMaskDrawList();
				}
			}
			else
			{
				VisThruWallOccluderDrawList.Reset();
				VisThruWallSpriteMaskDrawList.Reset();
				VisThruWallSpriteMaskUpdateCountdown = 0;
				VisThruWallEmptyScanCooldown = clamp<int>(nk_vis_thru_wall_empty_scan_interval, 0, 60);
			}
		}
	}
	else
	{
		VisThruWallEmptyScanCooldown = 0;
		VisThruWallDrawList.Reset();
		VisThruWallOccluderDrawList.Reset();
		VisThruWallSpriteMaskDrawList.Reset();
		VisThruWallSpriteMaskUpdateCountdown = 0;
	}

	if (nk_vis_thru_wall_debug_stats && Level != nullptr)
	{
		int debugInterval = clamp<int>(nk_vis_thru_wall_debug_stats_interval, 1, 350);
		int debugTic = Level->maptime;
		if (VisThruWallDebugLastPrintTic < 0 || debugTic - VisThruWallDebugLastPrintTic >= debugInterval)
		{
			VisThruWallDebugLastPrintTic = debugTic;
			Printf("[Nakara VisThruWall] active=%d filtermask=%u globalfade=%.2f emptycooldown=%d skipped=%d maskinterval=%d maskcountdown=%d maskskip=%d targets=%d accepted=%d fadeskip=%d occluders=%d accepted=%d smarttargets=%d sprseen=%d spraccepted=%d smartreject=%d flagskip=%d viewskip=%d\n",
				visThruWallRuntimeActive ? 1 : 0,
				NkVisThruWallFilterMask(),
				VisThruWallGlobalFade,
				VisThruWallEmptyScanCooldown,
				VisThruWallDebugSkippedByEmptyCooldown ? 1 : 0,
				clamp<int>(nk_vis_thru_wall_sprite_mask_update_interval, 1, 60),
				VisThruWallSpriteMaskUpdateCountdown,
				VisThruWallDebugSpriteMaskUpdateSkipped ? 1 : 0,
				VisThruWallDebugTargetsSeen,
				VisThruWallDebugTargetsAccepted,
				VisThruWallDebugTargetsFadeSkipped,
				VisThruWallDebugOccludersSeen,
				VisThruWallDebugOccludersAccepted,
				VisThruWallDebugSmartTargets,
				VisThruWallDebugSpriteMaskSeen,
				VisThruWallDebugSpriteMaskAccepted,
				VisThruWallDebugSpriteMaskSmartRejected,
				VisThruWallDebugSpriteMaskFlagSkipped,
				VisThruWallDebugSpriteMaskViewSkipped);
		}
	}

	// And now the crappy hacks that have to be done to avoid rendering anomalies.
	// These cannot be multithreaded when the time comes because all these depend
	// on the global 'validcount' variable.

	HandleMissingTextures(in_area);	// Missing upper/lower textures
	HandleHackedSubsectors();	// open sector hacks for deep water
	PrepareUnhandledMissingTextures();
	DispatchRenderHacks();
	screen->mLights->Unmap();
	screen->mBones->Unmap();
	screen->mVertexData->Unmap();

	ProcessAll.Unclock();

}

//-----------------------------------------------------------------------------
//
// RenderScene
//
// Draws the current draw lists for the non GLSL renderer
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderScene(FRenderState &state)
{
	const auto &vp = Viewpoint;
	RenderAll.Clock();

	state.SetDepthMask(true);

	state.EnableFog(true);
	state.SetRenderStyle(STYLE_Source);

	if (gl_sort_textures)
	{
		drawlists[GLDL_PLAINWALLS].SortWalls();
		drawlists[GLDL_PLAINFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLS].SortWalls();
		drawlists[GLDL_MASKEDFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLSOFS].SortWalls();
	}

	// Part 1: solid geometry. This is set up so that there are no transparent parts
	state.SetDepthFunc(DF_Less);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.ClearDepthBias();

	state.EnableTexture(gl_texture);
	state.EnableBrightmap(true);
	drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);


	// Part 2: masked geometry. This is set up so that only pixels with alpha>gl_mask_threshold will show
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
	drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

	// Part 3: masked geometry with polygon offset. This list is empty most of the time so only waste time on it when in use.
	if (drawlists[GLDL_MASKEDWALLSOFS].Size() > 0)
	{
		state.SetDepthBias(-1, -128);
		drawlists[GLDL_MASKEDWALLSOFS].DrawWalls(this, state, false);
		state.ClearDepthBias();
	}

	drawlists[GLDL_MODELS].Draw(this, state, false);

	state.SetRenderStyle(STYLE_Translucent);

	// Part 4: Draw decals (not a real pass)
	state.SetDepthFunc(DF_LEqual);
	DrawDecals(state, Decals[0]);

	RenderAll.Unclock();
}

//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
//
// BuildVisThruWallDrawList
//
// Nakara: collect +VISTHRUWALL actors directly from the thinker list instead of
// relying on the normal BSP/sector traversal. This solves the case where a wall-
// hidden actor never reaches the ordinary sprite draw lists because its sector was
// culled earlier.
//
// This pass is deliberately kept separate from FocusHighlight.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::BuildVisThruWallDrawList()
{
	VisThruWallDrawList.Reset();

	if (Level == nullptr)
	{
		return;
	}

	int maxCandidates = clamp<int>(nk_vis_thru_wall_max_candidates, 1, 1024);
	int addedCandidates = 0;

	VisThruWallCollectPass = true;

	auto it = Level->GetThinkerIterator<AActor>();
	AActor *thing = nullptr;

	while ((thing = it.Next()) != nullptr)
	{
		if (!(thing->flags9 & MF9_VISTHRUWALL))
		{
			continue;
		}

		VisThruWallDebugTargetsSeen++;

		if (thing->Sector == nullptr || thing->subsector == nullptr)
		{
			continue;
		}

		// [Nakara] Runtime fade for the separate through-wall redraw pass.
		// This smooths out the hard pop when an actor enters/leaves the valid VisThruWall
		// range or when the category filter changes. It is updated once per game tic so
		// uncapped framerates do not change the fade speed.
		bool visThruShouldShow = NkVisThruWallMatchesFilter(thing);
		if (visThruShouldShow && thing->VisThruWallMaxDistance > 0.0)
		{
			double maxDistance = max<double>(thing->VisThruWallMaxDistance, 0.0);
			double maxDistanceSq = maxDistance * maxDistance;
			DVector3 thingpos = thing->InterpolatedPosition(Viewpoint.TicFrac);

			if ((thingpos - Viewpoint.Pos).LengthSquared() > maxDistanceSq)
			{
				visThruShouldShow = false;
			}
		}

		int currentTic = Level->maptime;
		if (thing->VisThruWallLastFadeTic != currentTic)
		{
			int ticDelta = thing->VisThruWallLastFadeTic <= 0 ? 1 : max<int>(currentTic - thing->VisThruWallLastFadeTic, 1);
			thing->VisThruWallLastFadeTic = currentTic;

			if (visThruShouldShow)
			{
				if (thing->VisThruWallFadeInTics <= 0.0)
				{
					thing->VisThruWallCurrentFade = 1.0;
				}
				else
				{
					double fadeInTics = max<double>(thing->VisThruWallFadeInTics, 1.0);
					thing->VisThruWallCurrentFade = min<double>(thing->VisThruWallCurrentFade + double(ticDelta) / fadeInTics, 1.0);
				}
			}
			else
			{
				if (thing->VisThruWallFadeOutTics <= 0.0)
				{
					thing->VisThruWallCurrentFade = 0.0;
				}
				else
				{
					double fadeOutTics = max<double>(thing->VisThruWallFadeOutTics, 1.0);
					thing->VisThruWallCurrentFade = max<double>(thing->VisThruWallCurrentFade - double(ticDelta) / fadeOutTics, 0.0);
				}
			}
		}

		if (thing->VisThruWallCurrentFade <= 0.0)
		{
			VisThruWallDebugTargetsFadeSkipped++;
			continue;
		}

		HWSprite sprite;
		sprite.Process(this, thing, thing->Sector, in_area, false);

		addedCandidates++;
		VisThruWallDebugTargetsAccepted++;
		if (addedCandidates >= maxCandidates)
		{
			break;
		}
	}

	VisThruWallCollectPass = false;
}

//-----------------------------------------------------------------------------
//
// BuildVisThruWallOccluderDrawList
//
// Nakara: collect +NOVISTHRU actors into a separate stencil-occluder list.
// These sprites do not become through-wall targets. They only mark sprite-shaped
// pixels that should block the +VISTHRUWALL hidden redraw.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::BuildVisThruWallOccluderDrawList()
{
	VisThruWallOccluderDrawList.Reset();

	if (Level == nullptr)
	{
		return;
	}

	int maxCandidates = clamp<int>(nk_vis_thru_wall_max_candidates, 1, 1024);
	int addedCandidates = 0;

	VisThruWallOccluderCollectPass = true;

	auto it = Level->GetThinkerIterator<AActor>();
	AActor *thing = nullptr;

	while ((thing = it.Next()) != nullptr)
	{
		if (!(thing->flags9 & MF9_NOVISTHRU))
		{
			continue;
		}

		VisThruWallDebugOccludersSeen++;

		if (thing->Sector == nullptr || thing->subsector == nullptr)
		{
			continue;
		}

		HWSprite sprite;
		sprite.Process(this, thing, thing->Sector, in_area, false);

		addedCandidates++;
		VisThruWallDebugOccludersAccepted++;
		if (addedCandidates >= maxCandidates)
		{
			break;
		}
	}

	VisThruWallOccluderCollectPass = false;
}

//-----------------------------------------------------------------------------
//
// BuildVisThruWallSpriteMaskDrawList
//
// Nakara: collect ordinary sprite actors into a depth-only mask list. These
// sprites are not blockers; instead, they add their sprite-shaped depth just
// before the through-wall redraw so +VISTHRUWALL actors can be revealed when
// they are hidden behind normal actor sprites as well as behind walls.
//
// +NOVISTHRU actors are deliberately excluded here because they should block the
// through-wall redraw via the stencil occluder list. +VISTHRUWALL actors are
// also excluded so they do not mask themselves.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::BuildVisThruWallSpriteMaskDrawList()
{
	VisThruWallSpriteMaskDrawList.Reset();

	if (!nk_vis_thru_wall_sprite_occlusion || Level == nullptr)
	{
		return;
	}

	// If there is no +VISTHRUWALL sprite to redraw this frame, the ordinary
	// sprite mask cannot contribute anything. Avoid the extra thinker scan.
	if (VisThruWallDrawList.Size() == 0)
	{
		return;
	}

	int maxCandidates = clamp<int>(nk_vis_thru_wall_sprite_mask_max_candidates, 1, 4096);
	int addedCandidates = 0;

	struct FVisThruWallMaskTarget
	{
		DVector3 Pos;
		DVector3 Dir;
		double Distance;
	};

	TArray<FVisThruWallMaskTarget> maskTargets;

	if (nk_vis_thru_wall_sprite_mask_smart_filter)
	{
		int maxTargetCandidates = clamp<int>(nk_vis_thru_wall_max_candidates, 1, 1024);
		int addedTargets = 0;

		auto targetIt = Level->GetThinkerIterator<AActor>();
		AActor *target = nullptr;

		while ((target = targetIt.Next()) != nullptr)
		{
			if (!(target->flags9 & MF9_VISTHRUWALL))
			{
				continue;
			}

			// [Nakara] Only active or fading VisThruWall categories should drive the
			// ordinary sprite mask smart filter. This keeps item/monster/object filters
			// from expanding the mask around categories that are currently hidden.
			if (target->VisThruWallCurrentFade <= 0.0)
			{
				continue;
			}

			if (target->Sector == nullptr || target->subsector == nullptr)
			{
				continue;
			}

			DVector3 targetPos = target->InterpolatedPosition(Viewpoint.TicFrac);
			DVector3 targetDelta = targetPos - Viewpoint.Pos;
			double targetDistSq = targetDelta.LengthSquared();

			if (target->VisThruWallMaxDistance > 0.0)
			{
				double maxDistance = max<double>(target->VisThruWallMaxDistance, 0.0);
				if (targetDistSq > maxDistance * maxDistance)
				{
					continue;
				}
			}

			if (targetDistSq <= 1.0)
			{
				continue;
			}

			double targetDistance = sqrt(targetDistSq);
			FVisThruWallMaskTarget &entry = maskTargets[maskTargets.Reserve(1)];
			entry.Pos = targetPos;
			entry.Dir = targetDelta / targetDistance;
			entry.Distance = targetDistance;

			addedTargets++;
			VisThruWallDebugSmartTargets++;
			if (addedTargets >= maxTargetCandidates)
			{
				break;
			}
		}

		if (maskTargets.Size() == 0)
		{
			return;
		}
	}

	double smartRadius = max<double>((double)nk_vis_thru_wall_sprite_mask_target_radius, 0.0);
	double smartRadiusSq = smartRadius * smartRadius;
	double smartAngle = clamp<double>((double)nk_vis_thru_wall_sprite_mask_angle, 0.0, 89.0);
	double smartAngleCos = cos(smartAngle * (3.14159265358979323846 / 180.0));
	double smartDepthMargin = max<double>((double)nk_vis_thru_wall_sprite_mask_depth_margin, 0.0);

	VisThruWallSpriteMaskCollectPass = true;

	auto it = Level->GetThinkerIterator<AActor>();
	AActor *thing = nullptr;

	while ((thing = it.Next()) != nullptr)
	{
		// Normal sprites should reveal the through-wall silhouette. +NOVISTHRU is
		// the explicit exception, +VISNOTHRUMASK is ignored by the mask system,
		// and +VISTHRUWALL targets must not self-mask.
		if (thing->flags9 & (MF9_NOVISTHRU | MF9_VISNOTHRUMASK | MF9_VISTHRUWALL))
		{
			VisThruWallDebugSpriteMaskFlagSkipped++;
			continue;
		}

		VisThruWallDebugSpriteMaskSeen++;

		// Keep first-person/HUD-related sprites out of the ordinary world-sprite
		// mask. PSprites are not thinkers, but this also protects the current
		// camera/view actor from entering the mask through the normal actor path.
		if (thing == Viewpoint.camera || thing == Viewpoint.ViewActor)
		{
			VisThruWallDebugSpriteMaskViewSkipped++;
			continue;
		}

		if (thing->Sector == nullptr || thing->subsector == nullptr)
		{
			continue;
		}

		if (nk_vis_thru_wall_sprite_mask_smart_filter)
		{
			DVector3 thingPos = thing->InterpolatedPosition(Viewpoint.TicFrac);
			DVector3 thingDelta = thingPos - Viewpoint.Pos;
			double thingDistSq = thingDelta.LengthSquared();
			bool keepSpriteMaskCandidate = false;

			if (thingDistSq > 1.0)
			{
				double thingDistance = sqrt(thingDistSq);
				DVector3 thingDir = thingDelta / thingDistance;

				for (unsigned int i = 0; i < maskTargets.Size(); i++)
				{
					DVector3 targetDelta = thingPos - maskTargets[i].Pos;
					if (smartRadius > 0.0 && targetDelta.LengthSquared() <= smartRadiusSq)
					{
						keepSpriteMaskCandidate = true;
						break;
					}

					double dot = thingDir.X * maskTargets[i].Dir.X + thingDir.Y * maskTargets[i].Dir.Y + thingDir.Z * maskTargets[i].Dir.Z;
					if (dot >= smartAngleCos && thingDistance <= maskTargets[i].Distance + smartDepthMargin)
					{
						keepSpriteMaskCandidate = true;
						break;
					}
				}
			}

			if (!keepSpriteMaskCandidate)
			{
				VisThruWallDebugSpriteMaskSmartRejected++;
				continue;
			}
		}

		unsigned int beforeSize = VisThruWallSpriteMaskDrawList.Size();

		HWSprite sprite;
		sprite.Process(this, thing, thing->Sector, in_area, false);

		// Only count actors that actually produced a mask sprite. This avoids
		// wasting the candidate budget on invisible/no-sprite helper actors.
		if (VisThruWallSpriteMaskDrawList.Size() > beforeSize)
		{
			addedCandidates++;
			VisThruWallDebugSpriteMaskAccepted++;
			if (addedCandidates >= maxCandidates)
			{
				break;
			}
		}
	}

	VisThruWallSpriteMaskCollectPass = false;
}

void HWDrawInfo::RenderTranslucent(FRenderState &state)
{
	RenderAll.Clock();

	// final pass: translucent stuff
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetRenderStyle(STYLE_Translucent);

	state.EnableBrightmap(true);
	drawlists[GLDL_TRANSLUCENTBORDER].Draw(this, state, true);
	state.SetDepthMask(false);

	drawlists[GLDL_TRANSLUCENT].DrawSorted(this, state);
	state.EnableBrightmap(false);


	// [Nakara] +VISTHRUWALL hidden redraw.
	// Draw this after the normal translucent lists so ordinary world sprites do not
	// cover the through-wall silhouette. Only +NOVISTHRU actors are allowed to mask it.
	if (VisThruWallDrawList.Size() > 0)
	{
		state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);

		bool useVisThruWallOccluders = VisThruWallOccluderDrawList.Size() > 0;
		bool useVisThruWallSpriteMask = nk_vis_thru_wall_sprite_occlusion && VisThruWallSpriteMaskDrawList.Size() > 0;

		if (useVisThruWallOccluders)
		{
			// [Nakara] Mark only +NOVISTHRU sprite pixels in stencil.
			// Color/depth writes are disabled; this pass exists only so selected
			// sprite-shaped UI/world objects can block the through-wall redraw.
			state.SetNkVisThruWallTint(0, 0.0f);
			state.SetNkVisThruWallSpriteAlpha(1.0f);
			state.SetNkVisThruWallOutlineThickness(0.0f);
			state.SetNkVisThruWallOutlineAlpha(1.0f);
			state.SetRenderStyle(STYLE_Source);
			state.SetDepthFunc(DF_LEqual);
			state.SetDepthMask(false);
			state.EnableStencil(true);
			state.SetStencil(0, SOP_Increment, SF_ColorMaskOff | SF_DepthMaskOff);
			VisThruWallOccluderDrawList.DrawSorted(this, state);

			state.SetColorMask(true);
			state.SetDepthMask(false);
			state.SetStencil(0, SOP_Keep);
		}

		if (useVisThruWallSpriteMask)
		{
			// [Nakara] Ordinary sprites should not block +VISTHRUWALL by draw order.
			// Write their alpha-tested sprite shape into depth only, after the normal
			// translucent pass. Then the existing DF_Greater through-wall redraw also
			// triggers behind sprite pixels, not only behind map geometry.
			state.SetNkVisThruWallTint(0, 0.0f);
			state.SetNkVisThruWallSpriteAlpha(1.0f);
			state.SetNkVisThruWallOutlineThickness(0.0f);
			state.SetNkVisThruWallOutlineAlpha(1.0f);
			state.SetRenderStyle(STYLE_Translucent);
			state.SetDepthFunc(DF_LEqual);
			state.SetDepthMask(true);
			state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
			state.SetColorMask(false, false, false, false);
			if (useVisThruWallOccluders)
			{
				state.SetStencil(0, SOP_Keep);
			}
			VisThruWallSpriteMaskDrawList.DrawSorted(this, state);
			VisThruWallSpriteMaskDrawList.Reset();
			state.SetColorMask(true, true, true, true);
			state.SetDepthMask(false);
		}

		state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetDepthFunc(DF_Greater);
		state.SetDepthMask(false);
		if (useVisThruWallOccluders)
		{
			state.SetStencil(0, SOP_Keep);
		}

		VisThruWallRenderPass = true;
		VisThruWallDrawList.DrawSorted(this, state);
		VisThruWallRenderPass = false;
		VisThruWallDrawList.Reset();

		state.SetNkVisThruWallTint(0, 0.0f);
		state.SetNkVisThruWallSpriteAlpha(1.0f);
		state.SetNkVisThruWallOutlineThickness(0.0f);
		state.SetNkVisThruWallOutlineAlpha(1.0f);

		if (useVisThruWallOccluders)
		{
			// [Nakara] Restore the temporary stencil increments.
			state.SetRenderStyle(STYLE_Source);
			state.SetDepthFunc(DF_LEqual);
			state.SetStencil(1, SOP_Decrement, SF_ColorMaskOff | SF_DepthMaskOff);
			VisThruWallOccluderDrawList.DrawSorted(this, state);
			state.SetColorMask(true);
			state.EnableStencil(false);
			VisThruWallOccluderDrawList.Reset();
		}

		state.SetDepthFunc(DF_LEqual);
	}


	state.AlphaFunc(Alpha_GEqual, 0.5f);
	state.SetDepthMask(true);

	RenderAll.Unclock();
}


//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderPortal(HWPortal *p, FRenderState &state, bool usestencil)
{
	auto gp = static_cast<HWPortal *>(p);
	gp->SetupStencil(this, state, usestencil);
	auto new_di = StartDrawInfo(this->Level, this, Viewpoint, &VPUniforms);
	new_di->mCurrentPortal = gp;
	state.SetLightIndex(-1);
	gp->DrawContents(new_di, state);
	new_di->EndDrawInfo();
	state.SetVertexBuffer(screen->mVertexData);
	screen->mViewpoints->Bind(state, vpIndex);
	gp->RemoveStencil(this, state, usestencil);

}

void HWDrawInfo::DrawCorona(FRenderState& state, ACorona* corona, double dist)
{
#if 0
	spriteframe_t* sprframe = &SpriteFrames[sprites[corona->sprite].spriteframes + (size_t)corona->SpawnState->GetFrame()];
	FTextureID patch = sprframe->Texture[0];
	if (!patch.isValid()) return;
	auto tex = TexMan.GetGameTexture(patch, false);
	if (!tex || !tex->isValid()) return;

	// Project the corona sprite center
	FVector4 worldPos((float)corona->X(), (float)corona->Z(), (float)corona->Y(), 1.0f);
	FVector4 viewPos, clipPos;
	VPUniforms.mViewMatrix.multMatrixPoint(&worldPos[0], &viewPos[0]);
	VPUniforms.mProjectionMatrix.multMatrixPoint(&viewPos[0], &clipPos[0]);
	if (clipPos.W < -1.0f) return; // clip z nearest
	float halfViewportWidth = screen->GetWidth() * 0.5f;
	float halfViewportHeight = screen->GetHeight() * 0.5f;
	float invW = 1.0f / clipPos.W;
	float screenX = halfViewportWidth + clipPos.X * invW * halfViewportWidth;
	float screenY = halfViewportHeight - clipPos.Y * invW * halfViewportHeight;

	float alpha = corona->CoronaFade * float(corona->Alpha);

	// distance-based fade - looks better IMO
	float distNearFadeStart = float(corona->RenderRadius()) * 0.1f;
	float distFarFadeStart = float(corona->RenderRadius()) * 0.5f;
	float distFade = 1.0f;

	if (float(dist) < distNearFadeStart)
		distFade -= abs(((float(dist) - distNearFadeStart) / distNearFadeStart));
	else if (float(dist) >= distFarFadeStart)
		distFade -= (float(dist) - distFarFadeStart) / distFarFadeStart;

	alpha *= distFade;

	state.SetColorAlpha(0xffffff, alpha, 0);
	if (isSoftwareLighting()) state.SetSoftLightLevel(255);
	else state.SetNoSoftLightLevel();

	state.SetLightIndex(-1);
	state.SetRenderStyle(corona->RenderStyle);
	state.SetTextureMode(corona->RenderStyle);

	state.SetMaterial(tex, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, 0, 0);

	float scale = screen->GetHeight() / 1000.0f;
	float tileWidth = corona->Scale.X * tex->GetDisplayWidth() * scale;
	float tileHeight = corona->Scale.Y * tex->GetDisplayHeight() * scale;
	float x0 = screenX - tileWidth, y0 = screenY - tileHeight;
	float x1 = screenX + tileWidth, y1 = screenY + tileHeight;

	float u0 = 0.0f, v0 = 0.0f;
	float u1 = 1.0f, v1 = 1.0f;

	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	unsigned int vertexindex = vert.second;

	vp[0].Set(x0, y0, 1.0f, u0, v0);
	vp[1].Set(x1, y0, 1.0f, u1, v0);
	vp[2].Set(x0, y1, 1.0f, u0, v1);
	vp[3].Set(x1, y1, 1.0f, u1, v1);

	state.Draw(DT_TriangleStrip, vertexindex, 4);
#endif
}

//==========================================================================
//
// TraceCallbackForDitherTransparency
// Toggles dither flag on anything that occludes the actor's
// position from viewpoint.
//
//==========================================================================

static ETraceStatus TraceCallbackForDitherTransparency(FTraceResults& res, void* userdata)
{
	BitArray* CurMapSections = (BitArray*)userdata;
	double bf, bc;

	switch(res.HitType)
	{
	case TRACE_HitWall:
		{
			sector_t* linesec = res.Line->sidedef[res.Side]->sector;
			if (linesec->subsectorcount > 0 && (*CurMapSections)[linesec->subsectors[0]->mapsection])
			{
				bf = res.Line->sidedef[res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY());
				bc = res.Line->sidedef[res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY());
				if (res.Line->sidedef[!res.Side])
				{
					// Two sided line! So let's find out if mid, top, or bottom texture needs dithered transparency
					bf = max(bf, res.Line->sidedef[!res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY()));
					bc = min(bc, res.Line->sidedef[!res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY()));
					if (res.HitPos.Z <= bf) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_BOTTOM;
					else if (res.HitPos.Z < bc) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					else res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_TOP;

					res.Line->sidedef[res.Side]->dithertranscount = max<int>(1, res.Line->sidedef[!res.Side]->sector->e->XFloor.ffloors.Size());
				}
				else if ((res.HitPos.Z <= bc) && (res.HitPos.Z >= bf))
				{
					res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					res.Line->sidedef[res.Side]->dithertranscount = 1;
				}
			}
		}
		break;
	case TRACE_HitFloor:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->floorplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->floorplane.ZatPoint(res.HitPos))
			{
				res.Sector->floorplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->top.plane->ZatPoint(res.HitPos))
						{
							rover->top.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitCeiling:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->ceilingplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->ceilingplane.ZatPoint(res.HitPos))
			{
				res.Sector->ceilingplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->bottom.plane->ZatPoint(res.HitPos))
						{
							rover->bottom.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitActor:
	default:
		break;
	}

	return TRACE_ContinueOutOfBounds;
}


void HWDrawInfo::SetDitherTransFlags(AActor* actor)
{
	// This should really be moved to a shader and have the GPU do some shape-tracing.
	if (actor && actor->Sector)
	{
		FTraceResults results;
		double horix = Viewpoint.Sin * actor->radius;
		double horiy = Viewpoint.Cos * actor->radius;
		DVector3 actorpos = actor->Pos();
		DVector3 vvec = actorpos - Viewpoint.Pos;
		if (Viewpoint.IsOrtho())
		{
			vvec = 5.0 * Viewpoint.camera->ViewPos->Offset.Length() * Viewpoint.ViewVector3D; // Should be 4.0? (since zNear is behind screen by 3*dist in VREyeInfo::GetProjection())
		}
		double distance = vvec.Length() - actor->radius;
		DVector3 campos = actorpos - vvec;
		sector_t* startsec;

		vvec = vvec.Unit();
		campos.X -= horix; campos.Y += horiy; campos.Z += actor->Height * 0.25;
		for (int iter = 0; iter < 3; iter++)
		{
			startsec = Level->PointInRenderSubsector(campos)->sector;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z += actor->Height * 0.5;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z -= actor->Height * 0.5;
			campos.X += horix; campos.Y -= horiy;
		}

		// Tracers don't work on 3D floors when you are starting in the same sector (standing under them, for example)
		if (actor->Sector->e->XFloor.ffloors.Size()) // 3D floor
		{
			F3DFloor *rover;
			for (int kk = 0; kk < (int)actor->Sector->e->XFloor.ffloors.Size(); kk++)
			{
				rover = actor->Sector->e->XFloor.ffloors[kk];
				rover->top.plane->dithertransflag = true;
				rover->bottom.plane->dithertransflag = true;
			}
		}
	}
}

static ETraceStatus CheckForViewpointActor(FTraceResults& res, void* userdata)
{
	FRenderViewpoint* data = (FRenderViewpoint*)userdata;
	if (res.HitType == TRACE_HitActor && res.Actor && res.Actor == data->ViewActor)
	{
		return TRACE_Skip;
	}

	return TRACE_Stop;
}


void HWDrawInfo::DrawCoronas(FRenderState& state)
{
	state.EnableDepthTest(false);
	state.SetDepthMask(false);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = VRMode::GetVRMode(true)->GetHUDSpriteProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);

	float timeElapsed = (screen->FrameTime - LastFrameTime) / 1000.0f;
	LastFrameTime = screen->FrameTime;

#if 0
	for (ACorona* corona : Coronas)
	{
		auto cPos = corona->Vec3Offset(0., 0., corona->Height * 0.5);
		DVector3 direction = Viewpoint.Pos - cPos;
		double dist = direction.Length();

		// skip coronas that are too far
		if (dist > corona->RenderRadius())
			continue;

		static const float fadeSpeed = 9.0f;

		direction.MakeUnit();
		FTraceResults results;
		if (!Trace(cPos, corona->Sector, direction, dist, MF_SOLID, ML_BLOCKEVERYTHING, corona, results, 0, CheckForViewpointActor, &Viewpoint))
		{
			corona->CoronaFade = std::min(corona->CoronaFade + timeElapsed * fadeSpeed, 1.0f);
		}
		else
		{
			corona->CoronaFade = std::max(corona->CoronaFade - timeElapsed * fadeSpeed, 0.0f);
		}

		if (corona->CoronaFade > 0.0f)
			DrawCorona(state, corona, dist);
	}
#endif

	state.SetTextureMode(TM_NORMAL);
	screen->mViewpoints->Bind(state, vpIndex);
	state.EnableDepthTest(true);
	state.SetDepthMask(true);
}


//-----------------------------------------------------------------------------
//
// Draws player sprites and color blend
//
//-----------------------------------------------------------------------------


void HWDrawInfo::EndDrawScene(sector_t * viewsector, FRenderState &state)
{
	state.EnableFog(false);

	/*if (gl_coronas && Coronas.Size() > 0)
	{
		DrawCoronas(state);
	}*/

	// [BB] HUD models need to be rendered here. 
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	if (renderHUDModel)
	{
		// [BB] The HUD model should be drawn over everything else already drawn.
		state.Clear(CT_Depth);
		DrawPlayerSprites(true, state);
	}

	state.EnableStencil(false);
	state.SetViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

void HWDrawInfo::DrawEndScene2D(sector_t * viewsector, FRenderState &state)
{
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	auto vrmode = VRMode::GetVRMode(true);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = vrmode->GetHUDSpriteProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);
	state.EnableDepthTest(false);
	state.EnableMultisampling(false);

	DrawPlayerSprites(false, state);

	state.SetNoSoftLightLevel();

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

//-----------------------------------------------------------------------------
//
// sets 3D viewport and initial state
//
//-----------------------------------------------------------------------------

void HWDrawInfo::Set3DViewport(FRenderState &state)
{
	// Always clear all buffers with scissor test disabled.
	// This is faster on newer hardware because it allows the GPU to skip
	// reading from slower memory where the full buffers are stored.
	state.SetScissor(0, 0, -1, -1);
	state.Clear(CT_Color | CT_Depth | CT_Stencil);

	const auto &bounds = screen->mSceneViewport;
	state.SetViewport(bounds.left, bounds.top, bounds.width, bounds.height);
	state.SetScissor(bounds.left, bounds.top, bounds.width, bounds.height);
	state.EnableMultisampling(true);
	state.EnableDepthTest(true);
	state.EnableStencil(true);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
}

//-----------------------------------------------------------------------------
//
// gl_drawscene - this function renders the scene from the current
// viewpoint, including mirrors and skyboxes and other portals
// It is assumed that the HWPortal::EndFrame returns with the 
// stencil, z-buffer and the projection matrix intact!
//
//-----------------------------------------------------------------------------

void HWDrawInfo::DrawScene(int drawmode)
{
	static int recursion = 0;
	static int ssao_portals_available = 0;
	auto& vp = Viewpoint;

	bool applySSAO = false;
	if (drawmode == DM_MAINVIEW)
	{
		ssao_portals_available = gl_ssao_portals;
		applySSAO = true;
		if (r_dithertransparency && vp.IsAllowedOoB())
		{
			vp.camera->tracer ? SetDitherTransFlags(vp.camera->tracer) : SetDitherTransFlags(players[consoleplayer].mo);
		}
	}
	else if (drawmode == DM_OFFSCREEN)
	{
		ssao_portals_available = 0;
	}
	else if (drawmode == DM_PORTAL && ssao_portals_available > 0)
	{
		applySSAO = (mCurrentPortal->AllowSSAO() || Level->flags3&LEVEL3_SKYBOXAO);
		ssao_portals_available--;
	}

	if (vp.camera != nullptr)
	{
		ActorRenderFlags savedflags = vp.camera->renderflags;
		CreateScene(drawmode == DM_MAINVIEW);
		vp.camera->renderflags = savedflags;
	}
	else
	{
		CreateScene(false);
	}
	auto& RenderState = *screen->RenderState();

	RenderState.SetDepthMask(true);
	if (!gl_no_skyclear) portalState.RenderFirstSkyPortal(recursion, this, RenderState);

	RenderScene(RenderState);

	if (applySSAO && RenderState.GetPassType() == GBUFFER_PASS)
	{
		screen->AmbientOccludeScene(VPUniforms.mProjectionMatrix.get()[5]);
		screen->mViewpoints->Bind(RenderState, vpIndex);
	}

	// Handle all portals after rendering the opaque objects but before
	// doing all translucent stuff
	recursion++;
	portalState.EndFrame(this, RenderState);
	recursion--;
	RenderTranslucent(RenderState);
}


//-----------------------------------------------------------------------------
//
// R_RenderView - renders one view - either the screen or a camera texture
//
//-----------------------------------------------------------------------------

void HWDrawInfo::ProcessScene(bool toscreen)
{
	portalState.BeginScene();

	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.IsAllowedOoB() || Viewpoint.IsOrtho())
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
	DrawScene(toscreen ? DM_MAINVIEW : DM_OFFSCREEN);

}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddSubsectorToPortal(FSectorPortalGroup *ptg, subsector_t *sub)
{
	auto portal = FindPortal(ptg);
	if (!portal)
	{
        portal = new HWSectorStackPortal(&portalState, ptg);
		Portals.Push(portal);
	}
    auto ptl = static_cast<HWSectorStackPortal*>(portal);
	ptl->AddSubsector(sub);
}

