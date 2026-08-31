// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
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
** gl_sprite.cpp
** Sprite/Particle rendering
**
*/

#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "doomstat.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "a_pickups.h"
#include "a_corona.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "r_data/r_vanillatrans.h"
#include "matrix.h"
#include "models.h"
#include "vectors.h"
#include "texturemanager.h"
#include "basics.h"
#include "printf.h"

#include "hw_models.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_portal.h"
#include "flatvertices.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hw_material.h"
#include "hw_dynlightdata.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "quaternion.h"

#include "p_visualthinker.h"
#include "a_dynlight.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

extern TArray<spritedef_t> sprites;
extern TArray<spriteframe_t> SpriteFrames;
extern uint32_t r_renderercaps;

const float LARGE_VALUE = 1e19f;
const float MY_SQRT2    = 1.41421356237309504880; // sqrt(2)

EXTERN_CVAR(Bool, r_debug_disable_vis_filter)
EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Float, r_actorspriteshadowalpha)
EXTERN_CVAR(Float, r_actorspriteshadowfadeheight)

static constexpr uint8_t PARTICLETRAIL_TAIL_ALPHA_BUCKETS = 16; // [Nakara V22.2] Smooth age-based tail alpha bands.
EXTERN_CVAR(Bool, nk_ribbon_debug)

//==========================================================================
//
// Sprite CVARs
//
//==========================================================================

CVAR(Bool, gl_usecolorblending, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_sprite_blend, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CUSTOM_CVAR(Int, gl_spriteclip, -1, CVAR_NOSET | CVAR_NOSAVE)
{
	if (self != -1)
	{
		self = -1;
	}
}
CVAR(Bool, r_debug_nolimitanamorphoses, false, 0)
CVAR(Float, r_spriteclipanamorphicminbias, 0.6, CVAR_ARCHIVE)
CVAR(Float, gl_sclipthreshold, 10.0, CVAR_ARCHIVE)
CVAR(Float, gl_sclipfactor, 1.8f, CVAR_ARCHIVE)
CVAR(Int, gl_particles_style, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // 0 = square, 1 = round, 2 = smooth
CVAR(Int, gl_billboard_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_faces_camera, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, hw_force_cambbpref, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, gl_fuzztype, 0, CVAR_ARCHIVE)
{
	if (self < 0 || self > 8) self = 0;
}

//==========================================================================
//
// [Nakara] ParticleTrail center-line ribbon helpers.
//
// 2D sprite trails use the already-prepared HWSprite afterimage path below.
// 3D model trails intentionally do not inspect model geometry at all: only the
// actor center-line history is rendered as one narrow camera-facing ribbon.
// This keeps the effect cheap and independent from model Roll/Pitch/Yaw.
//

static bool ParticleTrailPointFinite(const FVector3& point)
{
	return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
}

static double ParticleTrailViewDepth(HWDrawInfo *di, const FVector3& point, const DVector3& viewDir)
{
	return (DVector3(point.X, point.Y, point.Z) - di->Viewpoint.Pos) | viewDir;
}

// [Nakara] Do not synthesize new trail vertices on the camera near plane.
// Intersecting a long translucent streak with the near plane can turn a tiny
// world-space trail into a huge screen-space sheet. Only the triangle that
// actually touches/crosses the near plane is omitted; fully visible triangles
// are kept unchanged. This is deliberately much less aggressive than dropping
// an entire trail segment from its center points.
static bool ParticleTrailTriangleSafelyInFront(HWDrawInfo *di, const DVector3& viewDir,
	const FVector3& a, const FVector3& b, const FVector3& c)
{
	if (di == nullptr || !ParticleTrailPointFinite(a) || !ParticleTrailPointFinite(b) || !ParticleTrailPointFinite(c))
		return false;

	const double nearDepth = (screen != nullptr ? screen->GetZNear() : 5.0) + 0.25;
	return ParticleTrailViewDepth(di, a, viewDir) >= nearDepth &&
		ParticleTrailViewDepth(di, b, viewDir) >= nearDepth &&
		ParticleTrailViewDepth(di, c, viewDir) >= nearDepth;
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::DrawSprite(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	bool additivefog = false;
	bool foglayer = false;

	// [Nakara V22] Tail-alpha passes calculate their exact opacity while building
	// ribbon geometry. Persistent vertex buffers used to build that geometry later,
	// after SetColor had already consumed `trans`, so create ribbon vertices up front.
	// Non-ribbon sprites keep the original lazy path below.
	if (isParticleTrailMesh && particleTrailSource == 2 && screen->BuffersArePersistent())
	{
		CreateVertices(di);
	}

	const bool noFogOnSprite = actor != nullptr && !isSpriteShadow && !isParticleTrailMesh && (actor->renderflags2 & RF2_NOFOGONSPRITE);
	int rel = fullbright ? 0 : getExtraLight();
	auto &vp = di->Viewpoint;	

	bool focusHighlight = actor != nullptr && !isSpriteShadow && !isParticleTrailMesh && (actor->flags9 & MF9_FOCUSHIGHLIGHT);
	state.SetFocusHighlight(focusHighlight ? 1.0f : 0.0f);

	float nkCloakAmount = 0.0f;
	if (nk_cloak_enable && actor != nullptr && !isSpriteShadow && !isParticleTrailMesh && (actor->flags9 & MF9_CLOAK))
	{
		nkCloakAmount = 1.0f - float(clamp<double>(actor->Alpha, 0.0, 1.0));
	}
	state.SetNkCloakAmount(nkCloakAmount);
	if (nkCloakAmount > 0.0001f)
	{
		nk_cloak_rendered_this_frame = true;
	}

	if (nk_focus_mask_enable && focusHighlight && actor->FocusTintAmount > 0.0 && nk_focus_tint_strength_runtime > 0.0f)
	{
		double tintStrength = clamp<double>(nk_focus_tint_strength_runtime, 0.0, 1.0);
		double tintAmount = clamp<double>(actor->FocusTintAmount, 0.0, 1.0) * tintStrength;
		state.SetFocusTint(actor->FocusTintColor, float(tintAmount), actor->FocusTintStyle);
	}
	else
	{
		state.SetFocusTint(PalEntry(255, 255, 255), 0.0f, 0);
	}


	// [Nakara] Per-actor tint for the separate through-wall redraw list.
	// This is property-only: no CVar color/alpha fallback.
	// The state is captured while the VisThruWall draw list is being built.
	if (di != nullptr && !isSpriteShadow && !isParticleTrailMesh && (di->VisThruWallCollectPass || di->VisThruWallRenderPass) && actor != nullptr && (actor->flags9 & MF9_VISTHRUWALL))
	{
		float spriteAlpha = 1.0f;

		if (actor->bVisThruWallSpriteAlphaSet)
		{
			spriteAlpha = float(clamp<double>(actor->VisThruWallSpriteAlpha, 0.0, 1.0));
		}

		float outlineAlpha = 1.0f;
		if (actor->bVisThruWallOutlineAlphaSet)
		{
			outlineAlpha = float(clamp<double>(actor->VisThruWallOutlineAlpha, 0.0, 1.0));
		}

		// [Nakara] Optional distance fade. VisThruWallMaxDistance 0 means unlimited,
		// and VisThruWallFadeDistance 0 means no fade. The fade only affects this
		// separate through-wall redraw pass.
		float distanceFade = 1.0f;
		if (actor->VisThruWallMaxDistance > 0.0 && actor->VisThruWallFadeDistance > 0.0)
		{
			double maxDistance = max<double>(actor->VisThruWallMaxDistance, 0.0);
			double fadeDistance = clamp<double>(actor->VisThruWallFadeDistance, 0.0, maxDistance);

			if (fadeDistance > 0.0)
			{
				double distance = (actor->InterpolatedPosition(vp.TicFrac) - vp.Pos).Length();
				distanceFade = float(clamp<double>((maxDistance - distance) / fadeDistance, 0.0, 1.0));
			}
		}

		float runtimeFade = float(clamp<double>(actor->VisThruWallCurrentFade, 0.0, 1.0));
		if (di != nullptr)
		{
			runtimeFade *= float(clamp<double>(di->VisThruWallGlobalFade, 0.0, 1.0));
		}

		spriteAlpha *= distanceFade * runtimeFade;
		outlineAlpha *= distanceFade * runtimeFade;

		state.SetNkVisThruWallSpriteAlpha(spriteAlpha);
		state.SetNkVisThruWallOutlineThickness(float(clamp<double>(actor->VisThruWallOutlineThickness, 0.0, 8.0)));
		state.SetNkVisThruWallOutlineAlpha(outlineAlpha);

		if (actor->bVisThruWallColorSet)
		{
			float tintAmount = 1.0f;

			if (actor->bVisThruWallAlphaSet)
			{
				tintAmount = float(clamp<double>(actor->VisThruWallAlpha, 0.0, 1.0));
			}

			tintAmount *= runtimeFade;
			state.SetNkVisThruWallTint(actor->VisThruWallColor, tintAmount);
		}
		else
		{
			state.SetNkVisThruWallTint(0, 0.0f);
		}
	}
	else
	{
		state.SetNkVisThruWallTint(0, 0.0f);
		state.SetNkVisThruWallSpriteAlpha(1.0f);
		state.SetNkVisThruWallOutlineThickness(0.0f);
		state.SetNkVisThruWallOutlineAlpha(1.0f);
	}

	if (nk_focus_mask_enable && focusHighlight && (actor->flags9 & MF9_FOCUSTINTOUTLINE) && actor->FocusTintOutlineThickness > 0.0)
	{
		PalEntry outlineColor = PalEntry(0, 0, 0);

		if (actor->bFocusTintOutlineColorSet)
		{
			outlineColor = actor->FocusTintOutlineColor;
		}
		else if (actor->bFocusTintColorSet)
		{
			outlineColor = actor->FocusTintColor;
		}

		state.SetFocusOutline(outlineColor, float(clamp<double>(actor->FocusTintOutlineThickness, 0.0, 8.0)));
	}
	else
	{
		state.SetFocusOutline(PalEntry(0, 0, 0), 0.0f);
	}

	if (translucent)
	{
		// The translucent pass requires special setup for the various modes.

		// for special render styles brightmaps would not look good - especially for subtractive.
		if (RenderStyle.BlendOp != STYLEOP_Add)
		{
			state.EnableBrightmap(false);
		}

		// Optionally use STYLE_ColorBlend in place of STYLE_Add for fullbright items.
		if (RenderStyle == LegacyRenderStyles[STYLE_Add] && trans > 1.f - FLT_EPSILON &&
			gl_usecolorblending && !di->isFullbrightScene() && actor &&
			fullbright && texture && !texture->GetTranslucency())
		{
			RenderStyle = LegacyRenderStyles[STYLE_ColorAdd];
		}

		state.SetRenderStyle(RenderStyle);
		state.SetTextureMode(RenderStyle);

		if (hw_styleflags == STYLEHW_NoAlphaTest)
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
		}
		else if (!texture || !texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
		else state.AlphaFunc(Alpha_Greater, 0.f);

		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			float fuzzalpha = 0.44f;
			float minalpha = 0.1f;

			// fog + fuzz don't work well without some fiddling with the alpha value!
			if (!noFogOnSprite && !Colormap.FadeColor.isBlack())
			{
				float dist = Dist2(vp.Pos.X, vp.Pos.Y, x, y);
				int fogd = GetFogDensity(di->Level, di->lightmode, lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

				// this value was determined by trial and error and is scale dependent!
				float factor = 0.05f + exp(-fogd * dist / 62500.f);
				fuzzalpha *= factor;
				minalpha *= factor;
			}

			state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
			state.SetColor(0.2f, 0.2f, 0.2f, fuzzalpha, Colormap.Desaturation);
			additivefog = true;
			lightlist = nullptr;	// the fuzz effect does not use the sector's light di->Level-> so splitting is not needed.
		}
		else if (RenderStyle.BlendOp == STYLEOP_Add && RenderStyle.DestAlpha == STYLEALPHA_One)
		{
			additivefog = true;
		}
	}
	else if (modelframe == nullptr)
	{
		// This still needs to set the texture mode. As blend mode it will always use GL_ONE/GL_ZERO
		state.SetTextureMode(RenderStyle);
		state.SetDepthBias(-1, -128);
	}
	if (RenderStyle.BlendOp != STYLEOP_Shadow)
	{
		if (di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright)
		{
			if (dynlightindex == -1)	// only set if we got no light buffer index. This covers all cases where sprite lighting is used.
			{
				float out[3] = {};
				di->GetDynSpriteLight(gl_light_sprites ? actor : nullptr, gl_light_particles ? particle : nullptr, out);
				state.SetDynLight(out[0], out[1], out[2]);
			}
		}
		sector_t *cursec = actor ? actor->Sector : particle ? particle->subsector->sector : nullptr;
		if (cursec != nullptr)
		{
			const PalEntry finalcol = fullbright
				? ThingColor
				: ThingColor.Modulate(cursec->SpecialColors[sector_t::sprites]);

			state.SetObjectColor(finalcol);
			state.SetAddColor(cursec->AdditiveColors[sector_t::sprites] | 0xff000000);
		}
		SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, trans);
	}


	if (Colormap.FadeColor.isBlack()) foglevel = lightlevel;

	if (RenderStyle.Flags & STYLEF_FadeToBlack)
	{
		Colormap.FadeColor = 0;
		additivefog = true;
	}

	if (RenderStyle.BlendOp == STYLEOP_RevSub || RenderStyle.BlendOp == STYLEOP_Sub)
	{
		if (!modelframe)
		{
			// non-black fog with subtractive style needs special treatment
			if (!noFogOnSprite && !Colormap.FadeColor.isBlack())
			{
				foglayer = true;
				// Due to the two-layer approach we need to force an alpha test that lets everything pass
				state.AlphaFunc(Alpha_Greater, 0);
			}
		}
		else RenderStyle.BlendOp = STYLEOP_Fuzz;	// subtractive with models is not going to work.
	}

	if (noFogOnSprite)
	{
		state.EnableFog(false);
		state.SetFog(0, 0);
	}
	else if (!foglayer)
	{
		SetFog(state, di->Level, di->lightmode, foglevel, rel, di->isFullbrightScene(), &Colormap, additivefog);
	}
	else
	{
		state.EnableFog(false);
		state.SetFog(0, 0);
	}

	int clampmode = CLAMP_XY;

	if (texture && texture->isNoMipmap())
	{
		clampmode = CLAMP_XY_NOMIP;
	}

	uint32_t spritetype = actor? uint32_t(actor->renderflags & RF_SPRITETYPEMASK) : 0;
	if (texture) state.SetMaterial(texture, UF_Sprite, (spritetype == RF_FACESPRITE) ? CTF_Expand : 0, clampmode, translation, OverrideShader);
	else if (!modelframe) state.EnableTexture(false);

	//SetColor(lightlevel, rel, Colormap, trans);

	unsigned int iter = lightlist ? lightlist->Size() : 1;
	bool clipping = false;
	if (lightlist || topclip != LARGE_VALUE || bottomclip != -LARGE_VALUE)
	{
		clipping = true;
		state.EnableSplit(true);
	}

	secplane_t bottomp = { { 0, 0, -1. }, bottomclip, 1. };
	secplane_t topp = { { 0, 0, -1. }, topclip, 1. };
	for (unsigned i = 0; i < iter; i++)
	{
		if (lightlist)
		{
			// set up the light slice
			secplane_t *topplane = i == 0 ? &topp : &(*lightlist)[i].plane;
			secplane_t *lowplane = i == (*lightlist).Size() - 1 ? &bottomp : &(*lightlist)[i + 1].plane;
			int thislight = (*lightlist)[i].caster != nullptr ? hw_ClampLight(*(*lightlist)[i].p_lightlevel) : lightlevel;
			int thisll = actor == nullptr ? thislight : (uint8_t)actor->Sector->CheckSpriteGlow(thislight, actor->InterpolatedPosition(vp.TicFrac));

			FColormap thiscm;
			thiscm.CopyFog(Colormap);
			CopyFrom3DLight(thiscm, &(*lightlist)[i]);
			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				thiscm.Decolorize();
			}

			SetColor(state, di->Level, di->lightmode, thisll, rel, di->isFullbrightScene(), thiscm, trans);
			if (!noFogOnSprite && !foglayer)
			{
				SetFog(state, di->Level, di->lightmode, thislight, rel, di->isFullbrightScene(), &thiscm, additivefog);
			}
			SetSplitPlanes(state, *topplane, *lowplane);
		}
		else if (clipping)
		{
			SetSplitPlanes(state, topp, bottomp);
		}

		if (!modelframe)
		{
			state.SetNormal(0, 0, 0);


			if (screen->BuffersArePersistent() && !(isParticleTrailMesh && particleTrailSource == 2))
			{
				CreateVertices(di);
			}
			if (polyoffset)
			{
				state.SetDepthBias(-1, -128);
			}
			state.SetLightIndex(-1);
			if (isParticleTrailMesh && particleTrailSource == 2)
			{
				if (particleTrailVertexCount > 0 && vertexindex >= 0)
					state.Draw(DT_Triangles, vertexindex, particleTrailVertexCount);
			}
			else
			{
				state.Draw(DT_TriangleStrip, vertexindex, 4);
			}

			if (foglayer)
			{
				// If we get here we know that we have colored fog and no fixed colormap.
				SetFog(state, di->Level, di->lightmode, foglevel, rel, false, &Colormap, additivefog);
				state.SetTextureMode(TM_FOGLAYER);
				state.SetRenderStyle(STYLE_Translucent);
				if (isParticleTrailMesh && particleTrailSource == 2)
				{
					if (particleTrailVertexCount > 0 && vertexindex >= 0)
						state.Draw(DT_Triangles, vertexindex, particleTrailVertexCount);
				}
				else
				{
					state.Draw(DT_TriangleStrip, vertexindex, 4);
				}
				state.SetTextureMode(TM_NORMAL);
			}
		}
		else
		{
			if (actor && di->Level->LightProbes.Size() > 0)
			{
				LightProbe* probe = FindLightProbe(di->Level, actor->X(), actor->Y(), actor->Center());
				if (probe)
					state.SetDynLight(probe->Red, probe->Green, probe->Blue);
			}

			FHWModelRenderer renderer(di, state, dynlightindex);
			RenderModel(&renderer, x, y, z, modelframe, actor, di->Viewpoint.TicFrac);
			state.SetVertexBuffer(screen->mVertexData);
		}
	}

	if (clipping)
	{
		state.EnableSplit(false);
	}

	if (translucent)
	{
		state.EnableBrightmap(true);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
		{
			state.ClearDepthBias();
		}
	}
	else if (modelframe == nullptr)
	{
		state.ClearDepthBias();
	}

	state.SetFocusHighlight(0.0f);
	state.SetFocusTint(PalEntry(255, 255, 255), 0.0f, 0);
	state.SetNkCloakAmount(0.0f);
	state.SetFocusOutline(PalEntry(0, 0, 0), 0.0f);
	state.SetNkVisThruWallOutlineThickness(0.0f);
	state.SetNkVisThruWallOutlineAlpha(1.0f);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableTexture(true);
	state.SetDynLight(0, 0, 0);
}

//==========================================================================
//
// 
//
//==========================================================================

void HandleSpriteOffsets(Matrix3x4 *mat, const FRotator *HW, FVector2 *offset, bool XYBillboard)
{
	FAngle zero = FAngle::fromDeg(0);
	FAngle pitch = (XYBillboard) ? HW->Pitch : zero;
	FAngle yaw = FAngle::fromDeg(270.) - HW->Yaw;

	FQuaternion quat = FQuaternion::FromAngles(yaw, pitch, zero);
	FVector3 sideVec = quat * FVector3(0, 1, 0);
	FVector3 upVec = quat * FVector3(0, 0, 1);
	FVector3 res = sideVec * offset->X + upVec * offset->Y;
	mat->Translate(res.X, res.Z, res.Y);
}

// [Nakara] Fish are identified by their exact external particle group. Using a
// combination of generic particle flags as a marker also matched ribbon carriers
// and other portal-side particles, corrupting their orientation.
static bool IsNakaraUnderwaterFishParticle(const HWDrawInfo *di, const particle_t *particle)
{
	if (di == nullptr || di->Level == nullptr || particle == nullptr) return false;

	const auto &particles = di->Level->Particles;
	const auto &groups = di->Level->NakaraParticleGroups;
	if (particles.Size() == 0 || groups.Size() == 0) return false;

	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(particles.Data());
	const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(particle);
	const std::uintptr_t byteCount = std::uintptr_t(particles.Size()) * sizeof(particle_t);
	if (address < base || address >= base + byteCount) return false;

	const std::uintptr_t offset = address - base;
	if ((offset % sizeof(particle_t)) != 0) return false;
	const unsigned index = unsigned(offset / sizeof(particle_t));
	return index < groups.Size() && groups[index] == NPG_UnderwaterFish;
}

bool HWSprite::CalculateVertices(HWDrawInfo* di, FVector3* v, DVector3* vp)
{
	float pixelstretch = di->Level->pixelstretch;

	FVector3 center = FVector3((x1 + x2) * 0.5, (y1 + y2) * 0.5, (z1 + z2) * 0.5);
	const auto& HWAngles = di->Viewpoint.HWAngles;
	Matrix3x4 mat;

	// [Nakara] World-oriented plane for the lightweight fish group only.
	if (IsNakaraUnderwaterFishParticle(di, particle) &&
		particle->texture.isValid() && particle->spriteScaleY != 0.0f)
	{
		const float viewX = di->Viewpoint.ViewVector.X;
		const float viewY = di->Viewpoint.ViewVector.Y;
		const float camRightX = -viewY;
		const float camRightY = viewX;
		const float localX1 = (x1 - x) * camRightX + (y1 - y) * camRightY;
		const float localX2 = (x2 - x) * camRightX + (y2 - y) * camRightY;
		const float localZ1 = z1 - z;
		const float localZ2 = z2 - z;

		const double yaw = Angles.Yaw.Radians();
		const double pitch = Angles.Pitch.Radians();
		const double cp = cos(pitch);
		FVector3 forward(float(cp * cos(yaw)), float(cp * sin(yaw)), float(sin(pitch)));
		const float horiz = sqrtf(forward.X * forward.X + forward.Y * forward.Y);

		FVector3 side;
		FVector3 up;
		if (horiz > 0.0001f)
		{
			side = FVector3(forward.Y / horiz, -forward.X / horiz, 0.0f);
			up = FVector3(-forward.X * forward.Z / horiz, -forward.Y * forward.Z / horiz, horiz);
		}
		else
		{
			side = FVector3(0.0f, -1.0f, 0.0f);
			up = FVector3(1.0f, 0.0f, 0.0f);
		}

		const double roll = Angles.Roll.Radians();
		const float cr = float(cos(roll));
		const float sr = float(sin(roll));
		const FVector3 rolledUp = up * cr + side * sr;

		auto FishVertex = [&](float lx, float lz)
		{
			const FVector3 world = FVector3(x, y, z) + forward * lx + rolledUp * lz;
			return FVector3(world.X, world.Z, world.Y);
		};

		v[0] = FishVertex(localX1, localZ1);
		v[1] = FishVertex(localX2, localZ1);
		v[2] = FishVertex(localX1, localZ2);
		v[3] = FishVertex(localX2, localZ2);
		return true;
	}
	if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
	{
		// [MC] Rotate around the center or offsets given to the sprites.
		// Counteract any existing rotations, then rotate the angle.
		// Tilt the actor up or down based on pitch (increase 'somersaults' forward).
		// Then counteract the roll and DO A BARREL ROLL.

		mat.MakeIdentity();
		FAngle pitch = FAngle::fromDeg(-Angles.Pitch.Degrees());
		pitch.Normalized180();

		mat.Translate(x, z, y);
		mat.Rotate(0, 1, 0, 270. - Angles.Yaw.Degrees());
		mat.Rotate(1, 0, 0, pitch.Degrees());

		if (actor->renderflags & RF_ROLLCENTER)
		{
			mat.Translate(center.X - x, 0, center.Y - y);
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-center.X, -z, -center.Y);
		}
		else
		{
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-x, -z, -y);
		}
		v[0] = mat * FVector3(x2, z, y2);
		v[1] = mat * FVector3(x1, z, y2);
		v[2] = mat * FVector3(x2, z, y1);
		v[3] = mat * FVector3(x1, z, y1);

		return true;
	}
	
	// [BB] Billboard stuff
	const bool drawWithXYBillboard = ((particle && gl_billboard_particles && !(particle->flags & SPF_NO_XY_BILLBOARD)) || (!(actor && actor->renderflags & RF_FORCEYBILLBOARD)
		//&& di->mViewActor != nullptr
		&& (gl_billboard_mode == 1 || (actor && actor->renderflags & RF_FORCEXYBILLBOARD))));

	const bool drawBillboardFacingCamera = hw_force_cambbpref ? gl_billboard_faces_camera :
		gl_billboard_faces_camera
		|| ((actor && (!(actor->renderflags2 & RF2_BILLBOARDNOFACECAMERA) && (actor->renderflags2 & RF2_BILLBOARDFACECAMERA)))
		|| (particle && particle->texture.isValid() && (!(particle->flags & SPF_NOFACECAMERA) && (particle->flags & SPF_FACECAMERA))));

	// [Nash] has +ROLLSPRITE
	const bool drawRollSpriteActor = (actor != nullptr && actor->renderflags & RF_ROLLSPRITE);
	const bool drawRollParticle = (particle != nullptr && particle->flags & SPF_ROLL);
	const bool doRoll = (drawRollSpriteActor || drawRollParticle);

	// [fgsfds] check sprite type mask
	uint32_t spritetype = (uint32_t)-1;
	if (actor != nullptr) spritetype = actor->renderflags & RF_SPRITETYPEMASK;

	// [Nash] is a flat sprite
	const bool isWallSprite = (actor != nullptr) && (spritetype == RF_WALLSPRITE);
	const bool useOffsets = ((actor != nullptr) && !(actor->renderflags & RF_ROLLCENTER)) || (particle && !(particle->flags & SPF_ROLLCENTER));

	FVector2 offset = FVector2( offx, offy );
	float xx = -center.X + x;
	float yy = -center.Y + y;
	float zz = -center.Z + z;
	// [Nash] check for special sprite drawing modes
	if (drawWithXYBillboard || drawBillboardFacingCamera || isWallSprite)
	{
		mat.MakeIdentity();
		mat.Translate(center.X, center.Z, center.Y); // move to sprite center
		mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio

		// [MC] Sprite offsets.
		if (!offset.isZero())
			HandleSpriteOffsets(&mat, &HWAngles, &offset, true);

		// Order of rotations matters. Perform yaw rotation (Y, face camera) before pitch (X, tilt up/down).
		if (drawBillboardFacingCamera && !isWallSprite)
		{
			// [CMB] Rotate relative to camera XY position, not just camera direction,
			// which is nicer in VR
			float xrel = center.X - vp->X;
			float yrel = center.Y - vp->Y;
			float absAngleDeg = atan2(-yrel, xrel) * (180 / M_PI);
			float counterRotationDeg = 270. - HWAngles.Yaw.Degrees(); // counteracts existing sprite rotation
			float relAngleDeg = counterRotationDeg + absAngleDeg;

			mat.Rotate(0, 1, 0, relAngleDeg);
		}

		// [fgsfds] calculate yaw vectors
		float rollDegrees = doRoll ? Angles.Roll.Degrees() : 0;
		float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();

		// [fgsfds] Rotate the sprite about the sight vector (roll) 
		if (isWallSprite)
		{
			float yawvecX = Angles.Yaw.Cos();
			float yawvecY = Angles.Yaw.Sin();
			mat.Rotate(0, 1, 0, 0);
			if (drawRollSpriteActor)
			{

				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Rotate(yawvecX, 0, yawvecY, rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
			}
		}
		else if (doRoll)
		{
			if (useOffsets) mat.Translate(xx, zz, yy);
			if (drawWithXYBillboard)
			{
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -HWAngles.Pitch.Degrees());
			}
			mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
			if (useOffsets) mat.Translate(-xx, -zz, -yy);
		}
		else if (drawWithXYBillboard)
		{
			// Rotate the sprite about the vector starting at the center of the sprite
			// triangle strip and with direction orthogonal to where the player is looking
			// in the x/y plane.
			mat.Rotate(-sin(angleRad), 0, cos(angleRad), -HWAngles.Pitch.Degrees());
		}

		mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
		mat.Translate(-center.X, -center.Z, -center.Y); // retreat from sprite center

		v[0] = mat * FVector3(x1, z1, y1);
		v[1] = mat * FVector3(x2, z1, y2);
		v[2] = mat * FVector3(x1, z2, y1);
		v[3] = mat * FVector3(x2, z2, y2);
	}
	else // traditional "Y" billboard mode
	{
		if (doRoll || !offset.isZero() || (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES)))
		{
			mat.MakeIdentity();

			if (!offset.isZero())
				HandleSpriteOffsets(&mat, &HWAngles, &offset, false);
			
			if (doRoll)
			{
				// Compute center of sprite
				float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();
				float rollDegrees = Angles.Roll.Degrees();

				mat.Translate(center.X, center.Z, center.Y);
				mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio
				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
				mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			if (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES) && di->Viewpoint.IsOrtho())
			{
				float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();
				mat.Translate(center.X, center.Z, center.Y);
				mat.Translate(0.0, z2 - center.Z, 0.0);
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -actor->isotheta);
				mat.Translate(0.0, center.Z - z2, 0.0);
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			v[0] = mat * FVector3(x1, z1, y1);
			v[1] = mat * FVector3(x2, z1, y2);
			v[2] = mat * FVector3(x1, z2, y1);
			v[3] = mat * FVector3(x2, z2, y2);
			
		}
		else
		{
			v[0] = FVector3(x1, z1, y1);
			v[1] = FVector3(x2, z1, y2);
			v[2] = FVector3(x1, z2, y1);
			v[3] = FVector3(x2, z2, y2);
		}
		
	}
	return false;
}

//==========================================================================
//
// 
//
//==========================================================================

inline void HWSprite::PutSprite(HWDrawInfo *di, bool translucent)
{
	// That's a lot of checks...
	if (modelframe && !modelframe->isVoxel && !(modelframeflags & MDL_NOPERPIXELLIGHTING) && RenderStyle.BlendOp != STYLEOP_Shadow && gl_light_sprites && di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright)
	{
		hw_GetDynModelLight(actor, lightdata);
		dynlightindex = screen->mLights->UploadLights(lightdata);
	}
	else
		dynlightindex = -1;

	vertexindex = -1;
	if (!screen->BuffersArePersistent())
	{
		CreateVertices(di);
	}
	di->AddSprite(this, translucent);
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::CreateVertices(HWDrawInfo *di)
{
	if (isParticleTrailMesh && particleTrailSource == 2)
	{
		CreateParticleTrailVertices(di);
		return;
	}

	if (modelframe == nullptr)
	{
		FVector3 v[4];
		polyoffset = CalculateVertices(di, v, &di->Viewpoint.Pos);
		auto vert = screen->mVertexData->AllocVertices(4);
		auto vp = vert.first;
		vertexindex = vert.second;

		vp[0].Set(v[0][0], v[0][1], v[0][2], ul, vt);
		vp[1].Set(v[1][0], v[1][1], v[1][2], ur, vt);
		vp[2].Set(v[2][0], v[2][1], v[2][2], ul, vb);
		vp[3].Set(v[3][0], v[3][1], v[3][2], ur, vb);
	}
}

// [Nakara Debug] Keep render diagnostics useful without flooding the console at
// frame rate. The simulation-side HIST-BEGIN line is once per generation; renderer
// warnings are capped per game tic across all ribbon actors.
static int ParticleTrailDebugRenderTic = -1;
static unsigned ParticleTrailDebugRenderLinesThisTic = 0;

static bool ParticleTrailDebugAllowRenderLine(AActor *actor)
{
	if (!nk_ribbon_debug || actor == nullptr || actor->Level == nullptr) return false;
	if (ParticleTrailDebugRenderTic != actor->Level->maptime)
	{
		ParticleTrailDebugRenderTic = actor->Level->maptime;
		ParticleTrailDebugRenderLinesThisTic = 0;
	}
	if (ParticleTrailDebugRenderLinesThisTic >= 32) return false;
	++ParticleTrailDebugRenderLinesThisTic;
	return true;
}

static bool ParticleTrailDebugNearOrigin(const DVector3& point)
{
	return point.LengthSquared() <= 0.01;
}

static bool ParticleTrailPositionInPortalGroup(FLevelLocals *level, const DVector3& position, int sourceGroup, int targetGroup, DVector3& out)
{
	if (level == nullptr) return false;
	if (sourceGroup < 0 || targetGroup < 0 ||
		sourceGroup >= level->Displacements.size || targetGroup >= level->Displacements.size)
	{
		return false;
	}

	out = position;
	out.XY() += level->Displacements.getOffset(sourceGroup, targetGroup);
	return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
}

void HWSprite::CreateParticleTrailVertices(HWDrawInfo *di)
{
	vertexindex = -1;
	particleTrailVertexCount = 0;
	if (di == nullptr || particleTrailSource != 2 || particleTrailGeneration == 0) return;

	// [Nakara] The active ribbon is owned by its AActor. Completed portal-side
	// generations and post-destruction trails are owned by a transient
	// DVisualThinker instead. Both feed the exact same mesh builder.
	FLevelLocals *trailLevel = nullptr;
	const TArray<FParticleTrailHistorySample> *trailHistory = nullptr;
	double lifetimeSeconds = 0.35;
	double trailScale = 1.0;
	double trailRadius = 1.0;
	double trailSpeed = 0.0;
	double tailFeather = 0.0;
	double headFeather = 0.0;
	double waveAmplitude = 0.0;
	double waveFrequency = 0.0;
	double waveSpeed = 0.0;
	if (actor != nullptr)
	{
		trailLevel = actor->Level;
		trailHistory = &actor->ParticleTrailHistory;
		lifetimeSeconds = actor->ParticleTrailLifetime > 0.0 ? actor->ParticleTrailLifetime : 0.35;
		trailScale = actor->ParticleTrailScale > 0.0 ? actor->ParticleTrailScale : 1.0;
		trailRadius = max<double>(fabs(actor->radius), 1.0);
		trailSpeed = actor->Vel.Length();
		tailFeather = clamp<double>(actor->ParticleTrailTailAlphaFade, 0.0, 1.0);
		headFeather = clamp<double>(actor->ParticleTrailHeadFeather, 0.0, 1.0);
		waveAmplitude = max<double>(actor->ParticleTrailWaveAmplitude, 0.0);
		waveFrequency = max<double>(actor->ParticleTrailWaveFrequency, 0.0);
		waveSpeed = actor->ParticleTrailWaveSpeed;
	}
	else if (particleTrailVisual != nullptr && particleTrailVisual->bParticleTrailRibbonCarrier)
	{
		trailLevel = particleTrailVisual->Level;
		trailHistory = &particleTrailVisual->ParticleTrailHistory;
		lifetimeSeconds = particleTrailVisual->ParticleTrailLifetime > 0.0 ? particleTrailVisual->ParticleTrailLifetime : 0.35;
		trailScale = particleTrailVisual->ParticleTrailScale > 0.0 ? particleTrailVisual->ParticleTrailScale : 1.0;
		trailRadius = max<double>(particleTrailVisual->ParticleTrailRadius, 1.0);
		tailFeather = clamp<double>(particleTrailVisual->ParticleTrailTailAlphaFade, 0.0, 1.0);
		headFeather = clamp<double>(particleTrailVisual->ParticleTrailHeadFeather, 0.0, 1.0);
		waveAmplitude = max<double>(particleTrailVisual->ParticleTrailWaveAmplitude, 0.0);
		waveFrequency = max<double>(particleTrailVisual->ParticleTrailWaveFrequency, 0.0);
		waveSpeed = particleTrailVisual->ParticleTrailWaveSpeed;
	}
	if (trailLevel == nullptr || trailHistory == nullptr || trailHistory->Size() == 0) return;
	const auto &history = *trailHistory;
	const uint32_t trailGeneration = particleTrailGeneration;
	const bool isCurrentGeneration = actor != nullptr && trailGeneration == actor->ParticleTrailGeneration;

	const double lifetimeTicks = max<double>(1.0, lifetimeSeconds * TICRATE);
	// P_Ticker increments maptime after actor thinkers have already recorded the
	// current Prev->Pos movement. Rendering then interpolates that same movement
	// with TicFrac. Therefore the actual render time of the interpolated actor is
	// (maptime - 1) + TicFrac, not maptime + TicFrac. Using the latter made every
	// current-tic history sample look one tic old, so samples *ahead* of the
	// interpolated projectile were appended before the head and the ribbon doubled
	// back on itself. With several projectiles those folds looked like cross-links.
	const double now = (double)trailLevel->maptime - 1.0 + di->Viewpoint.TicFrac;

	if (nk_ribbon_debug && history.Size() > 128 && ParticleTrailDebugAllowRenderLine(actor))
	{
		Printf("[RIBDBG:RENDER-OVERSIZE] tic=%d frac=%.4f actor=%p class=%s gen=%u historySize=%u\n",
			trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
			(unsigned)trailGeneration, history.Size());
	}

	// [Nakara] Ribbon trails are source-agnostic here. Sprite and model ribbons both
	// follow only the actor's center-line movement history; no source geometry or
	// model orientation is inspected, so the cost depends only on path samples.
	struct TrailNode
	{
		DVector3 Pos;
		double SpawnTime; // [Nakara V22.1] Stable age key for continuous tail-alpha fade.
		double PathDistance; // [Nakara V25.2] Stable emission-time wave phase coordinate.
		float WidthFactor;
		DVector3 Side;
		DVector3 PortalSeamTangent;
		uint8_t PortalSeamFlags;
	};

	DVector3 viewDir = di->Viewpoint.ViewVector3D;
	if (viewDir.LengthSquared() <= 0.000001) return;
	viewDir.MakeUnit();

	// [Nakara] Render only the newest temporally-contiguous run of this draw copy's
	// selected generation. Skipping a bad/missing sample with `continue` and then accepting
	// later samples can silently stitch two independent runs into one strip. A
	// continuously sampled ribbon never has more than one tic between adjacent
	// sample timestamps, even when only one history sample is emitted per tic.
	int newestIndex = -1;
	for (int i = (int)history.Size() - 1; i >= 0; --i)
	{
		const auto& h = history[i];
		if (h.Generation != trailGeneration || !std::isfinite(h.SpawnTime)) continue;
		const double age = now - h.SpawnTime;
		if (age >= -0.000001 && age < lifetimeTicks)
		{
			newestIndex = i;
			break;
		}
	}
	if (newestIndex < 0) return;

	// The newest history sample must still belong to the actor's current portal
	// space. If the actor crossed a portal after the last recorded sample, do not
	// bridge the stale strip to the new-space head for this render frame.
	if (isCurrentGeneration)
	{
		const int actorPortalGroup = actor->Sector != nullptr ? actor->Sector->PortalGroup : actor->PrevPortalGroup;
		if (history[newestIndex].PortalGroup != actorPortalGroup)
		{
			if (nk_ribbon_debug && ParticleTrailDebugAllowRenderLine(actor))
			{
				Printf("[RIBDBG:RENDER-PORTAL-STALE] tic=%d frac=%.4f actor=%p class=%s gen=%u "
					"newest=%d histgroup=%d actorgroup=%d\n",
					trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
					(unsigned)trailGeneration, newestIndex,
					history[newestIndex].PortalGroup, actorPortalGroup);
			}
			return;
		}
	}

	int oldestIndex = newestIndex;
	double newerTime = history[newestIndex].SpawnTime;
	int newerPortalGroup = history[newestIndex].PortalGroup;
	for (int i = newestIndex - 1; i >= 0; --i)
	{
		const auto& h = history[i];
		if (h.Generation != trailGeneration || !std::isfinite(h.SpawnTime)) break;
		// Never let a center-line run span two line-portal coordinate spaces. This
		// is a renderer-side safety net for old saves/stale samples; the simulation
		// also starts a new generation on the same transition.
		if (h.PortalGroup != newerPortalGroup)
		{
			if (nk_ribbon_debug && ParticleTrailDebugAllowRenderLine(actor))
			{
				Printf("[RIBDBG:RENDER-PORTAL-BREAK] tic=%d frac=%.4f actor=%p class=%s gen=%u "
					"older=%d oldgroup=%d newer=%d newgroup=%d\n",
					trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
					(unsigned)trailGeneration, i, h.PortalGroup, i + 1, newerPortalGroup);
			}
			break;
		}

		const double age = now - h.SpawnTime;
		const double sampleGap = newerTime - h.SpawnTime;
		if (age < -0.000001 || age >= lifetimeTicks || sampleGap < -0.000001 || sampleGap > 1.000001) break;

		oldestIndex = i;
		newerTime = h.SpawnTime;
	}

	TArray<TrailNode> nodes;
	for (int i = oldestIndex; i <= newestIndex; ++i)
	{
		const auto& h = history[i];
		const double age = now - h.SpawnTime;

		TrailNode node;
		if (!ParticleTrailPositionInPortalGroup(trailLevel, h.Pos, h.PortalGroup,
			particleTrailRenderPortalGroup, node.Pos))
		{
			if (ParticleTrailDebugAllowRenderLine(actor))
			{
				Printf("[RIBDBG:RENDER-PORTAL-FAIL] tic=%d frac=%.4f actor=%p class=%s gen=%u "
					"hist[%d]=(%.3f %.3f %.3f) group=%d targetgroup=%d\n",
					trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
					(unsigned)trailGeneration, i, h.Pos.X, h.Pos.Y, h.Pos.Z,
					h.PortalGroup, particleTrailRenderPortalGroup);
			}
			// A failed portal-space conversion is a hard chain boundary. Keeping the
			// older nodes and continuing would connect across the missing sample.
			nodes.Clear();
			continue;
		}

		if ((ParticleTrailDebugNearOrigin(h.Pos) || ParticleTrailDebugNearOrigin(node.Pos)) &&
			ParticleTrailDebugAllowRenderLine(actor))
		{
			const DVector3 rawActorPos = actor->Pos();
			const DVector3 visualActorPos = rawActorPos + actor->WorldOffset;
			const DVector3 interpRaw = actor->InterpolatedPosition(di->Viewpoint.TicFrac);
			const DVector3 interpVisual = interpRaw + actor->WorldOffset;
			Printf("[RIBDBG:NODE-ORIGIN] tic=%d frac=%.4f actor=%p class=%s gen=%u histSize=%u histIndex=%d "
				"raw=(%.6f %.6f %.6f) converted=(%.6f %.6f %.6f) spawn=%.6f age=%.6f "
				"actorRaw=(%.6f %.6f %.6f) actorVisual=(%.6f %.6f %.6f) "
				"interpRaw=(%.6f %.6f %.6f) interpVisual=(%.6f %.6f %.6f) worldoff=(%.6f %.6f %.6f) groups=%d->%d\n",
				trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)trailGeneration, history.Size(), i,
				h.Pos.X, h.Pos.Y, h.Pos.Z, node.Pos.X, node.Pos.Y, node.Pos.Z, h.SpawnTime, age,
				rawActorPos.X, rawActorPos.Y, rawActorPos.Z, visualActorPos.X, visualActorPos.Y, visualActorPos.Z,
				interpRaw.X, interpRaw.Y, interpRaw.Z, interpVisual.X, interpVisual.Y, interpVisual.Z,
				actor->WorldOffset.X, actor->WorldOffset.Y, actor->WorldOffset.Z, h.PortalGroup, particleTrailRenderPortalGroup);
		}

		if ((ParticleTrailDebugNearOrigin(h.Pos) != ParticleTrailDebugNearOrigin(node.Pos)) &&
			ParticleTrailDebugAllowRenderLine(actor))
		{
			Printf("[RIBDBG:PORTAL-MOVE-ORIGIN] tic=%d frac=%.4f actor=%p class=%s gen=%u "
				"hist[%d]=(%.3f %.3f %.3f) group=%d converted=(%.3f %.3f %.3f) targetgroup=%d\n",
				trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)trailGeneration, i, h.Pos.X, h.Pos.Y, h.Pos.Z, h.PortalGroup,
				node.Pos.X, node.Pos.Y, node.Pos.Z, particleTrailRenderPortalGroup);
		}

		// [Nakara] Keep the normal age-based taper for both active and retired
		// generations. The real trail tail should continue to become thinner as it
		// expires even after a portal split. Portal seam endpoints are handled
		// separately below so the artificial cut surface does not taper to a point.
		node.SpawnTime = h.SpawnTime;
		node.PathDistance = max<double>(0.0, h.PathDistance);
		node.WidthFactor = float(clamp<double>(1.0 - age / lifetimeTicks, 0.0, 1.0));
		node.Side = DVector3(0.0, 0.0, 0.0);
		node.PortalSeamTangent = h.PortalSeamTangent;
		node.PortalSeamFlags = h.PortalSeamFlags;

		// [Nakara] Portal entry/exit samples share the same exact crossing time. Now
		// that completed generations are detached to source-side VisualThinkers, both
		// halves can use the same normal age width. Forcing seam nodes back to 1.0
		// creates a visible bulb/spike at the portal as the neighboring nodes shrink.
		// Keeping the age-derived width makes the two exact seam endpoints shrink in
		// lockstep without changing the normal ribbon silhouette.

		if (nodes.Size() == 0 || (nodes.Last().Pos - node.Pos).LengthSquared() > 0.000001)
		{
			if (nodes.Size() > 0)
			{
				const double nodeDistance = (nodes.Last().Pos - node.Pos).Length();
				const double expectedStep = max<double>(8.0, trailSpeed * 2.0 + 4.0);
				if ((nodeDistance > expectedStep || ParticleTrailDebugNearOrigin(nodes.Last().Pos) || ParticleTrailDebugNearOrigin(node.Pos)) &&
					ParticleTrailDebugAllowRenderLine(actor))
				{
					Printf("[RIBDBG:NODE-SEG] tic=%d frac=%.4f actor=%p class=%s gen=%u "
						"a=(%.3f %.3f %.3f) b=(%.3f %.3f %.3f) dist=%.3f expected=%.3f\n",
						trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
						(unsigned)trailGeneration, nodes.Last().Pos.X, nodes.Last().Pos.Y, nodes.Last().Pos.Z,
						node.Pos.X, node.Pos.Y, node.Pos.Z, nodeDistance, expectedStep);
				}
			}
			nodes.Push(node);
		}
	}

	// Terminate the ribbon at the actor's interpolated current position only when
	// the newest history sample is temporally adjacent to this render time. This
	// prevents an active/reused actor from drawing one giant bridge from a stale
	// sample directly to its new head before the simulation has emitted a new run.
	const double newestAge = now - history[newestIndex].SpawnTime;
	if (isCurrentGeneration && (actor->effects & FX_PARTICLETRAIL) && actor->ParticleTrailHistoryMode == 2 && newestAge <= 1.000001)
	{
		TrailNode node;
		const DVector3 actorPos = actor->Pos() + actor->WorldOffset;
		const DVector3 interpolatedRaw = actor->InterpolatedPosition(di->Viewpoint.TicFrac);
		const DVector3 headPos = interpolatedRaw + actor->WorldOffset;
		const int headGroup = actor->Sector != nullptr ? actor->Sector->PortalGroup : actor->PrevPortalGroup;
		const int newestHistoryGroup = history[newestIndex].PortalGroup;
		if (headGroup != newestHistoryGroup)
		{
			if (nk_ribbon_debug && ParticleTrailDebugAllowRenderLine(actor))
			{
				Printf("[RIBDBG:HEAD-PORTAL-BREAK] tic=%d frac=%.4f actor=%p class=%s gen=%u "
					"histgroup=%d headgroup=%d head=(%.3f %.3f %.3f)\n",
					trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
					(unsigned)trailGeneration, newestHistoryGroup, headGroup,
					headPos.X, headPos.Y, headPos.Z);
			}
		}
		else if (ParticleTrailPositionInPortalGroup(trailLevel, headPos, headGroup,
			particleTrailRenderPortalGroup, node.Pos))
		{
			const double interpDelta = (headPos - actorPos).Length();
			const double expectedInterpDelta = max<double>(8.0, trailSpeed * 2.0 + 4.0);
			double lastHistoryToHead = 0.0;
			if (nodes.Size() > 0) lastHistoryToHead = (nodes.Last().Pos - node.Pos).Length();
			const bool headOriginMismatch = ParticleTrailDebugNearOrigin(headPos) && !ParticleTrailDebugNearOrigin(actorPos);
			const bool convertedOriginMismatch = ParticleTrailDebugNearOrigin(node.Pos) && !ParticleTrailDebugNearOrigin(actorPos);
			const bool interpolationMismatch = interpDelta > expectedInterpDelta;
			const bool historyBridge = nodes.Size() > 0 && lastHistoryToHead > expectedInterpDelta;
			if ((headOriginMismatch || convertedOriginMismatch || interpolationMismatch || historyBridge) &&
				ParticleTrailDebugAllowRenderLine(actor))
			{
				Printf("[RIBDBG:HEAD] tic=%d frac=%.4f actor=%p class=%s gen=%u reason=%s%s%s%s "
					"pos=(%.3f %.3f %.3f) prev=(%.3f %.3f %.3f) interpRaw=(%.3f %.3f %.3f) "
					"worldoff=(%.3f %.3f %.3f) head=(%.3f %.3f %.3f) converted=(%.3f %.3f %.3f) "
					"lastHist=(%.3f %.3f %.3f) interpDelta=%.3f histHead=%.3f expected=%.3f groups=%d->%d\n",
					trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
					(unsigned)trailGeneration,
					headOriginMismatch ? "HEAD_ORIGIN " : "", convertedOriginMismatch ? "CONVERTED_ORIGIN " : "",
					interpolationMismatch ? "INTERP_DELTA " : "", historyBridge ? "HISTORY_BRIDGE " : "",
					actorPos.X, actorPos.Y, actorPos.Z, actor->Prev.X, actor->Prev.Y, actor->Prev.Z,
					interpolatedRaw.X, interpolatedRaw.Y, interpolatedRaw.Z,
					actor->WorldOffset.X, actor->WorldOffset.Y, actor->WorldOffset.Z,
					headPos.X, headPos.Y, headPos.Z, node.Pos.X, node.Pos.Y, node.Pos.Z,
					nodes.Size() > 0 ? nodes.Last().Pos.X : 0.0, nodes.Size() > 0 ? nodes.Last().Pos.Y : 0.0, nodes.Size() > 0 ? nodes.Last().Pos.Z : 0.0,
					interpDelta, lastHistoryToHead, expectedInterpDelta, headGroup, particleTrailRenderPortalGroup);
			}

			node.SpawnTime = now;
			node.PathDistance = nodes.Size() > 0 ?
				nodes.Last().PathDistance + (node.Pos - nodes.Last().Pos).Length() : 0.0;
			node.WidthFactor = 1.f;
			node.Side = DVector3(0.0, 0.0, 0.0);
			node.PortalSeamTangent = DVector3(0.0, 0.0, 0.0);
			node.PortalSeamFlags = PTHSF_None;
			if (nodes.Size() == 0 || (nodes.Last().Pos - node.Pos).LengthSquared() > 0.000001)
			{
				nodes.Push(node);
			}
		}
		else if (ParticleTrailDebugAllowRenderLine(actor))
		{
			Printf("[RIBDBG:HEAD-PORTAL-FAIL] tic=%d frac=%.4f actor=%p class=%s gen=%u "
				"pos=(%.3f %.3f %.3f) prev=(%.3f %.3f %.3f) interp=(%.3f %.3f %.3f) groups=%d->%d\n",
				trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)trailGeneration, actorPos.X, actorPos.Y, actorPos.Z,
				actor->Prev.X, actor->Prev.Y, actor->Prev.Z, headPos.X, headPos.Y, headPos.Z,
				headGroup, particleTrailRenderPortalGroup);
		}
	}
	if (nk_ribbon_debug && nodes.Size() > 129 && ParticleTrailDebugAllowRenderLine(actor))
	{
		Printf("[RIBDBG:NODES-OVERSIZE] tic=%d frac=%.4f actor=%p class=%s gen=%u historySize=%u oldest=%d newest=%d nodes=%u\n",
			trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
			(unsigned)trailGeneration, history.Size(), oldestIndex, newestIndex, nodes.Size());
	}
	if (nodes.Size() < 2) return;

	// [Nakara] Do not inflate the nodes around a portal seam. The detached source
	// generation and destination generation already meet at matching exact crossing
	// samples, so artificial 1.00/0.92/0.78 width recovery only produces a local
	// shoulder in an otherwise smooth ribbon.

	// [Nakara] Normal spatial tail taper. Lifetime controls when history expires,
	// while this profile controls the *shape* of the visible oldest end. Keeping
	// those concepts separate prevents a young trail from ending in a blunt full-
	// width cut. Portal-exit generations are excluded: their oldest node is an
	// artificial seam and must stay full width so both portal halves meet cleanly.
	if (!(nodes[0].PortalSeamFlags & PTHSF_PortalExit))
	{
		static const float tailProfile[5] = { 0.05f, 0.30f, 0.65f, 0.90f, 1.00f };
		const unsigned taperCount = min<unsigned>(5, nodes.Size());
		for (unsigned i = 0; i < taperCount; ++i)
		{
			// Resample the five-point profile when the ribbon is still very short so
			// the projectile/head end remains full width even with only 2-4 nodes.
			const double profilePos = taperCount > 1 ? (double)i * 4.0 / (double)(taperCount - 1) : 0.0;
			const int profileA = clamp<int>((int)floor(profilePos), 0, 4);
			const int profileB = min<int>(profileA + 1, 4);
			const float profileFrac = float(profilePos - (double)profileA);
			const float spatialWidth = tailProfile[profileA] +
				(tailProfile[profileB] - tailProfile[profileA]) * profileFrac;
			nodes[i].WidthFactor = min<float>(nodes[i].WidthFactor, spatialWidth);
		}
	}

	// [Nakara V25.1] Apply the optional world-space lateral center-line wave BEFORE
	// building the billboard ribbon frame. V25 displaced nodes only after tangent/
	// side vectors had already been calculated from the un-waved path. The narrow
	// core mostly hid that mismatch, but the wider glow exposed it as an apparent
	// phase/sync offset. Building the final frame from the displaced center-line keeps
	// core and glow concentric and makes their width axes follow the same wave.
	//
	// The wave itself stays camera-independent. Both visible endpoints remain anchored
	// to the recorded center-line so the active projectile head and portal seams keep
	// their exact structural positions.
	if (waveAmplitude > 0.000001 && waveFrequency > 0.000001)
	{
		double wavePathLength = 0.0;
		for (unsigned i = 1; i < nodes.Size(); ++i)
		{
			wavePathLength += (nodes[i].Pos - nodes[i - 1].Pos).Length();
		}

		if (wavePathLength > 0.000001)
		{
			TArray<DVector3> waveSides;
			waveSides.Resize(nodes.Size());
			DVector3 previousWaveSide(0.0, 0.0, 0.0);
			bool havePreviousWaveSide = false;

			for (unsigned i = 0; i < nodes.Size(); ++i)
			{
				// Use only the original center-line geometry here. The normal billboard
				// frame is rebuilt below after all wave displacements have been applied.
				DVector3 tangent;
				if (i == 0)
				{
					tangent = nodes[1].Pos - nodes[0].Pos;
				}
				else if (i + 1 == nodes.Size())
				{
					tangent = nodes[i].Pos - nodes[i - 1].Pos;
				}
				else
				{
					tangent = nodes[i + 1].Pos - nodes[i - 1].Pos;
				}
				if (tangent.LengthSquared() <= 0.000001) tangent = DVector3(1.0, 0.0, 0.0);
				else tangent.MakeUnit();

				DVector3 waveSide = DVector3(0.0, 0.0, 1.0) ^ tangent;
				if (waveSide.LengthSquared() <= 0.000001)
				{
					waveSide = DVector3(0.0, 1.0, 0.0) - tangent * tangent.Y;
				}
				if (waveSide.LengthSquared() <= 0.000001)
				{
					waveSide = DVector3(1.0, 0.0, 0.0) - tangent * tangent.X;
				}
				if (waveSide.LengthSquared() <= 0.000001) waveSide = DVector3(1.0, 0.0, 0.0);
				else waveSide.MakeUnit();

				if (havePreviousWaveSide && (waveSide | previousWaveSide) < 0.0) waveSide = -waveSide;
				waveSides[i] = waveSide;
				previousWaveSide = waveSide;
				havePreviousWaveSide = true;
			}

			const double waveLength = 64.0 / waveFrequency;
			const double waveRadiansPerUnit = (2.0 * M_PI) / waveLength;

			for (unsigned i = 0; i < nodes.Size(); ++i)
			{
				// [Nakara V25.3] Freeze the wave phase when each history sample is emitted.
				// V25.2 stabilized the spatial PathDistance but still subtracted the current
				// render time here, so every old node kept oscillating sideways every frame.
				// Combined with the moving head/tail anchor envelope this made neighboring
				// lobes fold over each other while the projectile was in flight. SpawnTime is
				// immutable for stored samples, so WaveSpeed now advances only the phase of
				// newly emitted samples instead of re-positioning already laid-down geometry.
				const double sampleTimePhase =
					(nodes[i].SpawnTime / (double)TICRATE) * waveSpeed * (2.0 * M_PI);
				const double phase = nodes[i].PathDistance * waveRadiansPerUnit - sampleTimePhase;

				// Structural endpoints must remain exact. Portal entry/exit samples are the
				// shared seam anchors, and the live interpolated head must stay attached to
				// the projectile. Do not use a moving multi-node anchor envelope: changing
				// that envelope as the trail grows would move old history samples again.
				const bool structuralAnchor =
					(nodes[i].PortalSeamFlags & (PTHSF_PortalEntry | PTHSF_PortalExit)) != 0 ||
					(isCurrentGeneration && i + 1 == nodes.Size());
				const double offset = structuralAnchor ? 0.0 : sin(phase) * waveAmplitude;
				nodes[i].Pos += waveSides[i] * offset;
			}
		}
	}

	// [Nakara] Build the ribbon frame from the final Wave path. V24/V25 carried
	// one head-anchored side through the full strip; on a strongly curved Wave that
	// transported frame can accumulate visible twist, especially on a wide Glow.
	// V25.4 computes a camera-facing frame locally per final Wave segment and keeps
	// only sign continuity between neighbors.
	TArray<DVector3> segmentSides;
	segmentSides.Resize(nodes.Size() - 1);

	DVector3 cameraRight(-viewDir.Y, viewDir.X, 0.0);
	if (cameraRight.LengthSquared() <= 0.000001)
	{
		cameraRight = DVector3(1.0, 0.0, 0.0);
	}
	else
	{
		cameraRight.MakeUnit();
	}

	DVector3 cameraUp = cameraRight ^ viewDir;
	if (cameraUp.LengthSquared() <= 0.000001)
	{
		cameraUp = DVector3(0.0, 0.0, 1.0);
	}
	else
	{
		cameraUp.MakeUnit();
	}

	auto fallbackSide = [&](const DVector3& tangent)
	{
		DVector3 side = cameraRight - tangent * (cameraRight | tangent);
		if (side.LengthSquared() <= 0.000001)
		{
			side = cameraUp - tangent * (cameraUp | tangent);
		}
		if (side.LengthSquared() <= 0.000001)
		{
			side = DVector3(0.0, 0.0, 1.0) - tangent * tangent.Z;
		}
		if (side.LengthSquared() <= 0.000001)
		{
			side = DVector3(0.0, 1.0, 0.0) - tangent * tangent.Y;
		}
		if (side.LengthSquared() > 0.000001)
		{
			side.MakeUnit();
		}
		return side;
	};

	// [Nakara] Portal seam tangent continuity. The exact entry/exit samples carry
	// path tangents transformed by the line portal. Keep those tangents authoritative
	// near each seam, but let the final billboard side be recomputed locally below.
	const unsigned frameSegmentCount = nodes.Size() - 1;
	TArray<DVector3> segmentTangents;
	segmentTangents.Resize(frameSegmentCount);

	auto normalizedSeamTangent = [](const DVector3& tangent)
	{
		DVector3 result = tangent;
		if (!std::isfinite(result.X) || !std::isfinite(result.Y) || !std::isfinite(result.Z) ||
			result.LengthSquared() <= 0.000001)
		{
			return DVector3(0.0, 0.0, 0.0);
		}
		result.MakeUnit();
		return result;
	};

	// [Nakara] Keep the stored portal tangent authoritative for a few segments on
	// each side of the seam, not only for the single boundary segment. Line-portal
	// clipping can expose the next segment for a frame while the exact crossing
	// segment is clipped away; letting that next segment immediately fall back to
	// its geometric tangent makes the ribbon appear to kink or retract from the
	// portal. A short 3-segment falloff keeps the local seam frame coherent while
	// still handing control back to the real curved path almost immediately.
	static const double seamTangentWeights[3] = { 1.0, 0.72, 0.36 };
	const DVector3 exitSeamDirection = (nodes[0].PortalSeamFlags & PTHSF_PortalExit) ?
		normalizedSeamTangent(nodes[0].PortalSeamTangent) : DVector3(0.0, 0.0, 0.0);
	const DVector3 entrySeamDirection = (nodes.Last().PortalSeamFlags & PTHSF_PortalEntry) ?
		normalizedSeamTangent(nodes.Last().PortalSeamTangent) : DVector3(0.0, 0.0, 0.0);

	auto blendSeamTangent = [&](DVector3 tangent, DVector3 seamDirection, double weight)
	{
		if (seamDirection.LengthSquared() <= 0.000001 || weight <= 0.0) return tangent;
		if (tangent.LengthSquared() <= 0.000001) return seamDirection;
		if ((tangent | seamDirection) < 0.0) seamDirection = -seamDirection;
		tangent = tangent * (1.0 - weight) + seamDirection * weight;
		if (tangent.LengthSquared() <= 0.000001) return seamDirection;
		tangent.MakeUnit();
		return tangent;
	};

	for (unsigned i = 0; i < frameSegmentCount; ++i)
	{
		DVector3 tangent = nodes[i + 1].Pos - nodes[i].Pos;
		if (tangent.LengthSquared() > 0.000001) tangent.MakeUnit();

		if (exitSeamDirection.LengthSquared() > 0.000001 && i < 3)
		{
			tangent = blendSeamTangent(tangent, exitSeamDirection, seamTangentWeights[i]);
		}

		const unsigned entryDistance = frameSegmentCount - 1 - i;
		if (entrySeamDirection.LengthSquared() > 0.000001 && entryDistance < 3)
		{
			tangent = blendSeamTangent(tangent, entrySeamDirection, seamTangentWeights[entryDistance]);
		}

		if (tangent.LengthSquared() <= 0.000001) tangent = DVector3(1.0, 0.0, 0.0);
		segmentTangents[i] = tangent;
	}

	auto billboardSideAt = [&](const DVector3& tangent, const DVector3& position)
	{
		DVector3 eyeDir = position - di->Viewpoint.Pos;
		if (eyeDir.LengthSquared() <= 0.000001) eyeDir = viewDir;
		if (eyeDir.LengthSquared() > 0.000001) eyeDir.MakeUnit();

		// [Nakara V24.1] A camera-facing ribbon becomes mathematically singular when
		// its tangent points almost directly at the camera: tangent x eyeDir tends
		// toward zero. V24 switched abruptly to fallbackSide() below a fixed 0.01
		// squared-length threshold, which made the whole transported ribbon frame
		// suddenly appear to lie down or stand up as the view crossed that angle.
		// Blend continuously into the same camera-relative fallback instead. The
		// blend is restricted to the near-parallel region, so normal billboard and
		// portal tangent behavior remain unchanged away from the singularity.
		DVector3 billboardSide = tangent ^ eyeDir;
		const double billboardStrength = billboardSide.Length();
		DVector3 stableSide = fallbackSide(tangent);

		if (billboardStrength > 0.000001)
		{
			billboardSide /= billboardStrength;
		}

		if (stableSide.LengthSquared() <= 0.000001)
		{
			stableSide = billboardStrength > 0.000001 ? billboardSide : DVector3(1.0, 0.0, 0.0);
		}

		if (billboardStrength <= 0.000001)
		{
			return stableSide;
		}

		// Keep both candidate sides in the same hemisphere before interpolation so
		// the blend can never introduce an artificial 180-degree flip.
		if ((billboardSide | stableSide) < 0.0) stableSide = -stableSide;

		// billboardStrength is sin(view/tangent angle). Start blending inside about
		// 14 degrees and become fully stable inside about 3 degrees.
		double billboardWeight = clamp<double>((billboardStrength - 0.05) / 0.20, 0.0, 1.0);
		billboardWeight = billboardWeight * billboardWeight * (3.0 - 2.0 * billboardWeight);

		DVector3 side = stableSide * (1.0 - billboardWeight) + billboardSide * billboardWeight;
		if (side.LengthSquared() <= 0.000001) side = stableSide;
		if (side.LengthSquared() > 0.000001) side.MakeUnit();
		if (side.LengthSquared() <= 0.000001) side = DVector3(1.0, 0.0, 0.0);
		return side;
	};

	// [Nakara V25.4] Local billboard frame.
	// Do not carry one head/seam frame through the whole curved Wave strip. Doing
	// that makes a wide Glow expose the accumulated frame twist while the actor is
	// moving. Instead, each final Wave segment chooses its own camera-facing side
	// from its final tangent and local midpoint. The only continuity constraint is
	// hemisphere/sign matching with the previous segment, which prevents harmless
	// left/right label swaps from becoming visible 180-degree flips.
	DVector3 previousLocalSide(0.0, 0.0, 0.0);
	bool havePreviousLocalSide = false;
	for (unsigned i = 0; i < frameSegmentCount; ++i)
	{
		const DVector3 midpoint = (nodes[i].Pos + nodes[i + 1].Pos) * 0.5;
		DVector3 side = billboardSideAt(segmentTangents[i], midpoint);

		if (havePreviousLocalSide && (side | previousLocalSide) < 0.0)
		{
			side = -side;
		}

		if (side.LengthSquared() <= 0.000001)
		{
			side = fallbackSide(segmentTangents[i]);
		}
		if (side.LengthSquared() <= 0.000001)
		{
			side = DVector3(1.0, 0.0, 0.0);
		}
		else
		{
			side.MakeUnit();
		}

		segmentSides[i] = side;
		previousLocalSide = side;
		havePreviousLocalSide = true;
	}

	if (nk_ribbon_debug &&
		((nodes[0].PortalSeamFlags & PTHSF_PortalExit) || (nodes.Last().PortalSeamFlags & PTHSF_PortalEntry)) &&
		ParticleTrailDebugAllowRenderLine(actor))
	{
		Printf("[RIBDBG:SEAM-FRAME] tic=%d frac=%.4f actor=%p class=%s gen=%u segments=%u "
			"startflags=%u starttan=(%.4f %.4f %.4f) startside=(%.4f %.4f %.4f) "
			"endflags=%u endtan=(%.4f %.4f %.4f) endside=(%.4f %.4f %.4f)\n",
			trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
			(unsigned)trailGeneration, frameSegmentCount, (unsigned)nodes[0].PortalSeamFlags,
			nodes[0].PortalSeamTangent.X, nodes[0].PortalSeamTangent.Y, nodes[0].PortalSeamTangent.Z,
			segmentSides[0].X, segmentSides[0].Y, segmentSides[0].Z,
			(unsigned)nodes.Last().PortalSeamFlags,
			nodes.Last().PortalSeamTangent.X, nodes.Last().PortalSeamTangent.Y, nodes.Last().PortalSeamTangent.Z,
			segmentSides[frameSegmentCount - 1].X, segmentSides[frameSegmentCount - 1].Y, segmentSides[frameSegmentCount - 1].Z);
	}

	// Average the two adjacent local billboard sides at each internal node. Since
	// neighboring segment signs were already aligned above, this smooths bends
	// without transporting a rotation frame from the projectile head or a portal.
	for (unsigned i = 0; i < nodes.Size(); ++i)
	{
		DVector3 side;
		if (i == 0)
		{
			side = segmentSides[0];
		}
		else if (i + 1 == nodes.Size())
		{
			side = segmentSides[frameSegmentCount - 1];
		}
		else
		{
			side = segmentSides[i - 1] + segmentSides[i];
			if (side.LengthSquared() <= 0.000001) side = segmentSides[i];
		}
		if (side.LengthSquared() <= 0.000001) return;
		side.MakeUnit();
		nodes[i].Side = side;
	}

	// ParticleTrail.Scale is a width multiplier for the single ribbon. It never
	// scales the source sprite/model geometry.
	const double drawWidthScale = particleTrailWidthScale > 0.0f ? (double)particleTrailWidthScale : 1.0;
	// [Nakara V24.1] Scale/GlowScale are authored width controls, so do not cap
	// them at the old 12/24 world-unit half-width limits. Those caps made larger
	// values look as if the ribbon had been cropped by an invisible mask. The
	// near-plane triangle safety check below remains responsible for rejecting
	// geometry that actually crosses behind the camera.
	const float coreHalfWidth = float(max<double>(trailRadius * 0.12 * trailScale, 0.65));
	const float baseHalfWidth = float(max<double>((double)coreHalfWidth * drawWidthScale, 0.10));

	// [Nakara] Portal seam micro-overlap. V14 used a long tapered feather whose
	// narrowing outer vertices were visible as a small spike/arrowhead at the portal.
	// The source-side carrier now keeps the two generations in their correct portal
	// spaces, so only a tiny constant-width overlap is needed to hide raster/clip
	// precision cracks. Its width is exactly the seam node's current age-derived
	// width, so it cannot create a bulge and both halves decay identically.
	if (particleTrailSeamCap != 0)
	{
		const bool entryCap = particleTrailSeamCap == 1;
		const TrailNode& seamNode = entryCap ? nodes.Last() : nodes[0];
		const uint8_t requiredFlag = entryCap ? PTHSF_PortalEntry : PTHSF_PortalExit;
		if (!(seamNode.PortalSeamFlags & requiredFlag)) return;

		DVector3 tangent = normalizedSeamTangent(seamNode.PortalSeamTangent);
		if (tangent.LengthSquared() <= 0.000001 || seamNode.Side.LengthSquared() <= 0.000001) return;

		const DVector3 outward = entryCap ? tangent : -tangent;

		// [Nakara V17] Start the helper exactly on the real seam and extend only
		// through the portal plane. V15/V16 extended the helper equally backward
		// into the main ribbon and forward through the portal. That backward half
		// was harmless at 18% alpha, but becomes a bright double-blend once the
		// helper uses the same alpha as the ribbon. A one-sided strip shares only
		// the seam edge with the main mesh, so it can cover the portal clip/raster
		// boundary without changing the visible opacity on either ribbon half.
		const double overlapLength = clamp<double>((double)baseHalfWidth * 0.08, 0.10, 0.32);
		const DVector3 innerPos = seamNode.Pos;
		const DVector3 outerPos = seamNode.Pos + outward * overlapLength;

		auto seamEdge = [&](const DVector3& position, FVector3& left, FVector3& right)
		{
			const float halfWidth = baseHalfWidth * seamNode.WidthFactor;
			const FVector3 center((float)position.X, (float)position.Y, (float)position.Z);
			const FVector3 side((float)seamNode.Side.X, (float)seamNode.Side.Y, (float)seamNode.Side.Z);
			left = center - side * halfWidth;
			right = center + side * halfWidth;
		};

		FVector3 innerLeft, innerRight, outerLeft, outerRight;
		seamEdge(innerPos, innerLeft, innerRight);
		seamEdge(outerPos, outerLeft, outerRight);

		auto allocation = screen->mVertexData->AllocVertices(6);
		auto vp = allocation.first;
		vertexindex = allocation.second;
		unsigned out = 0;
		auto emitCapTriangle = [&](const FVector3& a, const FVector3& b, const FVector3& c)
		{
			if (!ParticleTrailTriangleSafelyInFront(di, viewDir, a, b, c)) return;
			vp[out++].Set(a.X, a.Z, a.Y, 0.f, 0.f);
			vp[out++].Set(b.X, b.Z, b.Y, 0.f, 0.f);
			vp[out++].Set(c.X, c.Z, c.Y, 0.f, 0.f);
		};

		emitCapTriangle(innerLeft, innerRight, outerRight);
		emitCapTriangle(innerLeft, outerRight, outerLeft);

		if (out == 0)
		{
			vertexindex = -1;
			return;
		}

		if (nk_ribbon_debug && ParticleTrailDebugAllowRenderLine(actor))
		{
			Printf("[RIBDBG:SEAM-MICRO] tic=%d frac=%.4f actor=%p class=%s gen=%u mode=%s "
				"seam=(%.3f %.3f %.3f) halfLen=%.3f width=%.3f verts=%u\n",
				trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
				(unsigned)trailGeneration, entryCap ? "entry" : "exit",
				seamNode.Pos.X, seamNode.Pos.Y, seamNode.Pos.Z, overlapLength,
				(double)(baseHalfWidth * seamNode.WidthFactor), out);
		}

		particleTrailVertexCount = (int)out;
		return;
	}

	const unsigned segmentCount = nodes.Size() - 1;

	// [Nakara V23.3] TailFeather and HeadFeather are both spatial. Their values are
	// fractions of the current visible ribbon center-line length: TailFeather grows
	// from alpha 0 at the real tail toward full alpha, while HeadFeather mirrors that
	// from the real head. This makes 0.3 consistently cover about 30% of the visible
	// ribbon regardless of projectile speed, Lifetime, or sampling density. Neither
	// effect moves history points or changes ribbon width. Artificial portal-exit tails
	// and portal-entry heads remain independently exempt so the V17/V21 seam fixes stay
	// intact.
	const bool hasRealTail = !(nodes[0].PortalSeamFlags & PTHSF_PortalExit);
	const bool hasRealHead = !(nodes.Last().PortalSeamFlags & PTHSF_PortalEntry);
	bool useTailFeather = tailFeather > 0.0001 && hasRealTail;
	bool useHeadFeather = headFeather > 0.0001 && hasRealHead;

	TArray<double> tailDistance;
	TArray<double> headDistance;
	double visibleRibbonLength = 0.0;
	double tailFeatherDistance = 0.0;
	double headFeatherDistance = 0.0;
	if (useTailFeather || useHeadFeather)
	{
		tailDistance.Resize(nodes.Size());
		tailDistance[0] = 0.0;
		for (unsigned i = 1; i < nodes.Size(); ++i)
		{
			visibleRibbonLength += (nodes[i].Pos - nodes[i - 1].Pos).Length();
			tailDistance[i] = visibleRibbonLength;
		}

		if (useTailFeather)
		{
			tailFeatherDistance = visibleRibbonLength * tailFeather;
			if (tailFeatherDistance <= 0.000001)
			{
				useTailFeather = false;
			}
		}

		if (useHeadFeather)
		{
			headDistance.Resize(nodes.Size());
			for (unsigned i = 0; i < nodes.Size(); ++i)
			{
				headDistance[i] = visibleRibbonLength - tailDistance[i];
			}

			headFeatherDistance = visibleRibbonLength * headFeather;
			if (headFeatherDistance <= 0.000001)
			{
				useHeadFeather = false;
			}
		}
	}
	const bool useEndFeather = useTailFeather || useHeadFeather;

	auto smoothEndAlpha = [](double value)
	{
		value = clamp<double>(value, 0.0, 1.0);
		return float(value * value * (3.0 - 2.0 * value));
	};

	auto segmentEndAlpha = [&](unsigned segment)
	{
		if (!useEndFeather || segment >= segmentCount) return 1.0f;

		float alpha = 1.0f;

		// Spatial tail feather. nodes are ordered tail -> head, so the older endpoint
		// of the first segment has distance 0 and the fade reaches full opacity at
		// visibleRibbonLength * TailFeather.
		if (useTailFeather)
		{
			const double distanceFromTail = tailDistance[segment];
			if (distanceFromTail < tailFeatherDistance)
			{
				alpha = min<float>(alpha, smoothEndAlpha(distanceFromTail / tailFeatherDistance));
			}
		}

		// Spatial head feather. nodes are ordered tail -> head, so the newer endpoint
		// of the last segment has distance 0 and the fade reaches full opacity at
		// visibleRibbonLength * HeadFeather. Using stored positions only changes alpha;
		// no point is pulled toward the actor as the history window advances.
		if (useHeadFeather)
		{
			const double distanceFromHead = headDistance[segment + 1];
			if (distanceFromHead < headFeatherDistance)
			{
				alpha = min<float>(alpha, smoothEndAlpha(distanceFromHead / headFeatherDistance));
			}
		}

		return alpha;
	};

	auto segmentTailPass = [&](unsigned segment)
	{
		const float alpha = segmentEndAlpha(segment);
		if (alpha >= 0.9995f) return 0u; // normal full-alpha body pass
		const unsigned bucket = min<unsigned>((unsigned)floor((double)alpha * PARTICLETRAIL_TAIL_ALPHA_BUCKETS),
			(unsigned)PARTICLETRAIL_TAIL_ALPHA_BUCKETS - 1);
		return bucket + 1;
	};

	// FFlatVertex has no per-vertex alpha, so feathered segments are emitted in 16
	// stable alpha bands. The pass field keeps its old internal V22 name to minimize
	// renderer churn; it now carries either/both endpoint feather effects.
	if (particleTrailTailFadePass != 0)
	{
		if (!useEndFeather || particleTrailTailFadePass > PARTICLETRAIL_TAIL_ALPHA_BUCKETS) return;
		const unsigned bucket = (unsigned)particleTrailTailFadePass - 1;
		const float alphaMultiplier = bucket == 0 ? 0.0f :
			float((double)bucket / (double)(PARTICLETRAIL_TAIL_ALPHA_BUCKETS - 1));
		trans *= alphaMultiplier;
		if (trans <= 0.0001f) return;
	}

	const unsigned vertexCapacity = segmentCount * 6;
	auto allocation = screen->mVertexData->AllocVertices(vertexCapacity);
	auto vp = allocation.first;
	vertexindex = allocation.second;
	unsigned out = 0;

	auto makeEdge = [&](const TrailNode& node, FVector3& left, FVector3& right)
	{
		// [Nakara V24.1] Preserve the authored world-space width even close to the
		// camera. V24's depth * 0.05 cap silently squeezed wide ribbons as they
		// approached the view, producing the apparent rectangular crop in large
		// Scale/GlowScale tests. Triangle-level near-plane rejection still prevents
		// a strip that crosses the camera plane from exploding into a screen-sized quad.
		const float halfWidth = baseHalfWidth * node.WidthFactor;
		const FVector3 center((float)node.Pos.X, (float)node.Pos.Y, (float)node.Pos.Z);

		const FVector3 side((float)node.Side.X, (float)node.Side.Y, (float)node.Side.Z);
		left = center - side * halfWidth;
		right = center + side * halfWidth;
	};

	auto emitTriangle = [&](const FVector3& a, const FVector3& b, const FVector3& c)
	{
		if (!ParticleTrailTriangleSafelyInFront(di, viewDir, a, b, c)) return;
		vp[out++].Set(a.X, a.Z, a.Y, 0.f, 0.f);
		vp[out++].Set(b.X, b.Z, b.Y, 0.f, 0.f);
		vp[out++].Set(c.X, c.Z, c.Y, 0.f, 0.f);
	};

	for (unsigned i = 0; i < segmentCount; ++i)
	{
		const unsigned pass = segmentTailPass(i);
		if ((particleTrailTailFadePass == 0 && pass != 0) ||
			(particleTrailTailFadePass != 0 && pass != (unsigned)particleTrailTailFadePass))
		{
			continue;
		}

		FVector3 tailLeft, tailRight, headLeft, headRight;
		makeEdge(nodes[i], tailLeft, tailRight);
		makeEdge(nodes[i + 1], headLeft, headRight);

		if (nk_ribbon_debug)
		{
			const double segmentLength = (nodes[i + 1].Pos - nodes[i].Pos).Length();
			const double expectedStep = max<double>(8.0, trailSpeed * 2.0 + 4.0);
			const bool originTransition = ParticleTrailDebugNearOrigin(nodes[i].Pos) != ParticleTrailDebugNearOrigin(nodes[i + 1].Pos);
			if ((segmentLength > expectedStep || originTransition) && ParticleTrailDebugAllowRenderLine(actor))
			{
				Printf("[RIBDBG:MESH-SEG] tic=%d frac=%.4f actor=%p class=%s gen=%u seg=%u/%u "
					"centerA=(%.3f %.3f %.3f) centerB=(%.3f %.3f %.3f) len=%.3f expected=%.3f "
					"leftA=(%.3f %.3f %.3f) rightA=(%.3f %.3f %.3f) leftB=(%.3f %.3f %.3f) rightB=(%.3f %.3f %.3f)\n",
					trailLevel->maptime, di->Viewpoint.TicFrac, (void*)actor, actor->GetClass()->TypeName.GetChars(),
					(unsigned)trailGeneration, i, segmentCount,
					nodes[i].Pos.X, nodes[i].Pos.Y, nodes[i].Pos.Z, nodes[i + 1].Pos.X, nodes[i + 1].Pos.Y, nodes[i + 1].Pos.Z,
					segmentLength, expectedStep, tailLeft.X, tailLeft.Y, tailLeft.Z, tailRight.X, tailRight.Y, tailRight.Z,
					headLeft.X, headLeft.Y, headLeft.Z, headRight.X, headRight.Y, headRight.Z);
			}
		}

		// One and only one connected ribbon: two triangles per center-line segment.
		emitTriangle(tailLeft, tailRight, headRight);
		emitTriangle(tailLeft, headRight, headLeft);

	}

	if (out == 0)
	{
		vertexindex = -1;
		return;
	}
	particleTrailVertexCount = (int)out;
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::SplitSprite(HWDrawInfo *di, sector_t * frontsector, bool translucent)
{
	HWSprite copySprite;
	double lightbottom;
	unsigned int i;
	bool put=false;
	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	for(i=0;i<lightlist.Size();i++)
	{
		// Particles don't go through here so we can safely assume that actor is not nullptr
		if (i<lightlist.Size()-1) lightbottom=lightlist[i+1].plane.ZatPoint(actor);
		else lightbottom=frontsector->floorplane.ZatPoint(actor);

		if (lightbottom<z2) lightbottom=z2;

		if (lightbottom<z1)
		{
			copySprite=*this;
			copySprite.lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
			copySprite.Colormap.CopyLight(lightlist[i].extra_colormap);

			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				copySprite.Colormap.Decolorize();
			}

			if (!ThingColor.isWhite())
			{
				copySprite.Colormap.LightColor.r = (copySprite.Colormap.LightColor.r*ThingColor.r) >> 8;
				copySprite.Colormap.LightColor.g = (copySprite.Colormap.LightColor.g*ThingColor.g) >> 8;
				copySprite.Colormap.LightColor.b = (copySprite.Colormap.LightColor.b*ThingColor.b) >> 8;
			}

			z1=copySprite.z2=lightbottom;
			vt=copySprite.vb=copySprite.vt+ 
				(lightbottom-copySprite.z1)*(copySprite.vb-copySprite.vt)/(z2-copySprite.z1);
			copySprite.PutSprite(di, translucent);
			put=true;
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight)
{
	const float NO_VAL = 100000000.0f;
	bool clipthing = (thing->player || thing->flags3&MF3_ISMONSTER || thing->IsKindOf(NAME_Inventory)) && (thing->flags&MF_ICECORPSE || !(thing->flags&MF_CORPSE));
	bool smarterclip = !clipthing && gl_spriteclip == 3;
	if ((clipthing || gl_spriteclip > 1) && !(thing->flags2 & MF2_FLOATBOB))
	{

		float btm = NO_VAL;
		float top = -NO_VAL;
		extsector_t::xfloor &x = thing->Sector->e->XFloor;

		if (x.ffloors.Size())
		{
			for (unsigned int i = 0; i < x.ffloors.Size(); i++)
			{
				F3DFloor * ff = x.ffloors[i];
				if (ff->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
				float floorh = ff->top.plane->ZatPoint(thingpos);
				float ceilingh = ff->bottom.plane->ZatPoint(thingpos);
				if (floorh == thing->floorz)
				{
					btm = floorh;
				}
				if (ceilingh == thing->ceilingz)
				{
					top = ceilingh;
				}
				if (btm != NO_VAL && top != -NO_VAL)
				{
					break;
				}
			}
		}
		else if (thing->Sector->GetHeightSec())
		{
			if (thing->flags2&MF2_ONMOBJ && thing->floorz ==
				thing->Sector->heightsec->floorplane.ZatPoint(thingpos))
			{
				btm = thing->floorz;
				top = thing->ceilingz;
			}
		}
		if (btm == NO_VAL)
			btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		if (top == NO_VAL)
			top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		// +/-1 to account for the one pixel empty frame around the sprite.
		float diffb = (z2+1) - btm;
		float difft = (z1-1) - top;
		if (diffb >= 0 /*|| !gl_sprite_clip_to_floor*/) diffb = 0;
		// Adjust sprites clipping into ceiling and adjust clipping adjustment for tall graphics
		if (smarterclip)
		{
			// Reduce slightly clipping adjustment of corpses
			if (thing->flags & MF_CORPSE || spriteheight > fabs(diffb))
			{
				float ratio = clamp<float>((fabs(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
				diffb *= ratio;
			}
			if (!diffb)
			{
				if (difft <= 0) difft = 0;
				if (difft >= (float)gl_sclipthreshold)
				{
					// dumb copy of the above.
					if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || difft > (float)gl_sclipthreshold)
					{
						difft = 0;
					}
				}
				if (spriteheight > fabs(difft))
				{
					float ratio = clamp<float>((fabs(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					difft *= ratio;
				}
				z2 -= difft;
				z1 -= difft;
			}
		}
		if (diffb <= (0 - (float)gl_sclipthreshold))	// such a large displacement can't be correct! 
		{
			// for living monsters standing on the floor allow a little more.
			if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || diffb < (-1.8*(float)gl_sclipthreshold))
			{
				diffb = 0;
			}
		}
		z2 -= diffb;
		z1 -= diffb;
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::Process(HWDrawInfo *di, AActor* thing, sector_t * sector, area_t in_area, int thruportal, bool isSpriteShadow)
{
	sector_t rs;
	sector_t * rendersector;

	this->isSpriteShadow = isSpriteShadow;
	this->isParticleTrailMesh = false;
	this->particleTrailVertexCount = 0;
	this->particleTrailSource = 0;
	this->particleTrailGeneration = 0;
	this->particleTrailSeamCap = 0;
	this->particleTrailTailFadePass = 0;
	this->particleTrailWidthScale = 1.0f;
	this->particleTrailRenderPortalGroup = 0;
	this->particleTrailVisual = nullptr;

	if (thing == nullptr)
		return;

	// [ZZ] allow CustomSprite-style direct picnum specification
	bool isPicnumOverride = thing->picnum.isValid();

	// Don't waste time projecting sprites that are definitely not visible.
	if ((thing->sprite == 0 && !isPicnumOverride) || !thing->IsVisibleToPlayer() || ((thing->renderflags & RF_MASKROTATION) && !thing->IsInsideVisibleAngles()))
	{
		return;
	}

#if 0
	if (thing->IsKindOf(NAME_Corona))
	{
		di->Coronas.Push(static_cast<ACorona*>(thing));
		return;
	}
#endif

	const auto &vp = di->Viewpoint;
	AActor *camera = vp.camera;

	const bool nkCloakActor = nk_cloak_enable && (thing->flags9 & MF9_CLOAK);
	if (thing->renderflags & RF_INVISIBLE || (!thing->RenderStyle.IsVisible(thing->Alpha) && !nkCloakActor))
	{
		if (!(thing->flags & MF_STEALTH) || !di->isStealthVision() || thing == camera)
			return;
	}

	// check renderrequired vs ~r_rendercaps, if anything matches we don't support that feature,
	// check renderhidden vs r_rendercaps, if anything matches we do support that feature and should hide it.
	if ((!r_debug_disable_vis_filter && !!(thing->RenderRequired & ~r_renderercaps)) ||
		(!!(thing->RenderHidden & r_renderercaps)))
		return;

	int spritenum = thing->sprite;
	DVector2 sprscale(thing->Scale.X, thing->Scale.Y);
	if (thing->player != nullptr)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// [RH] Interpolate the sprite's position to make it look smooth
	DVector3 thingpos = thing->InterpolatedPosition(vp.TicFrac);
	if (thruportal == 1) thingpos += di->Level->Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);

	AActor *viewmaster = thing;
	if ((thing->flags8 & MF8_MASTERNOSEE) && thing->master != nullptr)
	{
		viewmaster = thing->master;
	}

	// [Nash] filter visibility in mirrors
	bool isInMirror = di->mCurrentPortal && (di->mCurrentPortal->mState->MirrorFlag > 0 || di->mCurrentPortal->mState->PlaneMirrorFlag > 0);
	if (thing->renderflags2 & RF2_INVISIBLEINMIRRORS && isInMirror)
	{
		return;
	}
	else if (thing->renderflags2 & RF2_ONLYVISIBLEINMIRRORS && !isInMirror)
	{
		return;
	}
	// Some added checks if the camera actor is not supposed to be seen. It can happen that some portal setup has this actor in view in which case it may not be skipped here
	if (viewmaster == camera && !vp.showviewer)
	{
		if (vp.bForceNoViewer || (viewmaster->player && viewmaster->player->crossingPortal)) return;
		DVector3 vieworigin = viewmaster->Pos();
		if (thruportal == 1) vieworigin += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2) return;

		// Necessary in order to prevent sprite pop-ins with viewpos and models. 
		auto* sec = viewmaster->Sector;
		if (sec && !sec->PortalBlocksMovement(sector_t::ceiling))
		{
			double zh = sec->GetPortalPlaneZ(sector_t::ceiling);
			double top = (viewmaster->player ? max<double>(viewmaster->player->viewz, viewmaster->Top()) + 1 : viewmaster->Top());
			if (viewmaster->Z() < zh && top >= zh)
				return;
		}
	}
	// Thing is invisible if close to the camera.
	if (viewmaster->renderflags & RF_MAYBEINVISIBLE)
	{
		DVector3 viewpos = viewmaster->InterpolatedPosition(vp.TicFrac);
		if (thruportal == 1) viewpos += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(viewpos.X - vp.Pos.X) < 32 && fabs(viewpos.Y - vp.Pos.Y) < 32) return;
	}

	modelframe = isPicnumOverride ? nullptr : FindModelFrame(thing, spritenum, thing->frame, !!(thing->flags & MF_DROPPED));
	modelframeflags = modelframe ? modelframe->getFlags(thing->modelData) : 0;

	// Too close to the camera. This doesn't look good if it is a sprite.
	if (fabs(thingpos.X - vp.Pos.X) < 2 && fabs(thingpos.Y - vp.Pos.Y) < 2
		&& vp.Pos.Z >= thingpos.Z - 2 && vp.Pos.Z <= thingpos.Z + thing->Height + 2
		&& !thing->Vel.isZero() && !modelframe) // exclude vertically moving objects from this check.
	{
		return;
	}

	// don't draw first frame of a player missile
	if (thing->flags&MF_MISSILE)
	{
		if (!(thing->flags7 & MF7_FLYCHEAT) && thing->target == vp.ViewActor && vp.ViewActor != nullptr)
		{
			double speed = thing->Vel.Length();
			if (speed >= thing->target->radius / 2)
			{
				double clipdist = clamp(thing->Speed, thing->target->radius, thing->target->radius * 2);
				if ((thingpos - vp.Pos).LengthSquared() < clipdist * clipdist) return;
			}
		}
		thing->flags7 |= MF7_FLYCHEAT;	// do this only once for the very first frame, but not if it gets into range again.
	}

	if (thruportal != 2 && di->mClipPortal != nullptr)
	{
		int clipres = di->mClipPortal->ClipPoint(thingpos.XY());
		if (clipres == PClip_InFront) return;
	}
	// disabled because almost none of the actual game code is even remotely prepared for this. If desired, use the INTERPOLATE flag.
	if (thing->renderflags & RF_INTERPOLATEANGLES)
		Angles = thing->InterpolatedAngles(vp.TicFrac);
	else
		Angles = thing->Angles;

	if (sector->sectornum != thing->Sector->sectornum && !thruportal)
	{
		// This cannot create a copy in the fake sector cache because it'd interfere with the main thread, so provide a local buffer for the copy.
		// Adding synchronization for this one case would cost more than it might save if the result here could be cached.
		rendersector = hw_FakeFlat(thing->Sector, in_area, false, &rs);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	uint32_t spritetype = (thing->renderflags & RF_SPRITETYPEMASK);
	x = thingpos.X + thing->WorldOffset.X;
	z = thingpos.Z + thing->WorldOffset.Z;
	y = thingpos.Y + thing->WorldOffset.Y;
	if (spritetype == RF_FACESPRITE) z -= thing->Floorclip; // wall and flat sprites are to be considered di->Level-> geometry so this may not apply.

	// snap shadow Z to the floor
	if (isSpriteShadow)
	{
		z = thing->floorz;
	}
	// [RH] Make floatbobbing a renderer-only effect.
	else
	{
		float fz = thing->GetBobOffset(vp.TicFrac);
		z += fz;
	}

	// don't bother drawing sprite shadows if this is a model (it will never look right)
	if (modelframe && isSpriteShadow)
	{
		return;
	}
	if (!modelframe)
	{
		bool mirror = false;
		DAngle ang = (thingpos - vp.Pos).Angle();
		if (di->Viewpoint.IsOrtho()) ang = vp.Angles.Yaw;
		FTextureID patch;
		// [ZZ] add direct picnum override
		if (isPicnumOverride)
		{
			// Animate picnum overrides.
			auto tex = TexMan.GetGameTexture(thing->picnum, true);
			if (tex == nullptr) return;

			if (tex->GetRotations() != 0xFFFF)
			{
				// choose a different rotation based on player view
				spriteframe_t* sprframe = &SpriteFrames[tex->GetRotations()];
				DAngle sprang = thing->GetSpriteAngle(ang, vp.TicFrac);
				angle_t rot;
				if (sprframe->Texture[0] == sprframe->Texture[1])
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
				}
				else
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
				}
				auto picnum = sprframe->Texture[rot];
				if (sprframe->Flip & (1 << rot))
				{
					mirror = true;
				}
			}

			patch =  tex->GetID();
		}
		else
		{
			DAngle sprangle;
			int rot;
			if (!(thing->renderflags & RF_FLATSPRITE) || thing->flags7 & MF7_SPRITEANGLE)
			{
				sprangle = thing->GetSpriteAngle(ang, vp.TicFrac);
				rot = -1;
			}
			else
			{
				// Flat sprites cannot rotate in a predictable manner.
				sprangle = nullAngle;
				rot = 0;
			}
			patch = sprites[spritenum].GetSpriteFrame(thing->frame, rot, sprangle, &mirror, !!(thing->renderflags & RF_SPRITEFLIP));
		}

		if (!patch.isValid()) return;
		int type = thing->renderflags & RF_SPRITETYPEMASK;
		auto tex = TexMan.GetGameTexture(patch, false);
		if (!tex || !tex->isValid()) return;
		auto& spi = tex->GetSpritePositioning(type == RF_FACESPRITE);

		offx = (float)thing->GetSpriteOffset(false);
		offy = (float)thing->GetSpriteOffset(true);

		vt = spi.GetSpriteVT();
		vb = spi.GetSpriteVB();
		if (thing->renderflags & RF_YFLIP) std::swap(vt, vb);

		auto r = spi.GetSpriteRect();

		// [SP] SpriteFlip
		if (thing->renderflags & RF_SPRITEFLIP)
			thing->renderflags ^= RF_XFLIP;

		if (mirror ^ !!(thing->renderflags & RF_XFLIP))
		{
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
			ul = spi.GetSpriteUL();
			ur = spi.GetSpriteUR();
		}
		else
		{
			ul = spi.GetSpriteUR();
			ur = spi.GetSpriteUL();
		}

		texture = tex;
		if (!texture || !texture->isValid())
			return;

		if (thing->renderflags & RF_SPRITEFLIP) // [SP] Flip back
			thing->renderflags ^= RF_XFLIP;

		// If sprite is isometric, do both vertical scaling and partial rotation to face the camera to compensate for Y-billboarding.
		// Using just rotation (about z=0) might cause tall+slender (high aspect ratio) sprites to clip out of collision box
		// at the top and clip into whatever is behind them from the viewpoint's perspective. - [DVR]
		thing->isoscaleY = 1.0;
		thing->isotheta = vp.HWAngles.Pitch.Degrees();
		if (thing->renderflags2 & RF2_ISOMETRICSPRITES)
		{
			float floordist = thing->radius * vp.floordistfact;
			floordist -= 0.5 * r.width * vp.cotfloor;
			float sineisotheta = floordist / r.height;
			double scl = g_sqrt( 1.0 + sineisotheta * sineisotheta - 2.0 * vp.PitchSin * sineisotheta );
			if ((thing->radius > 0.0) && (scl > fabs(vp.PitchCos)))
			{
				thing->isoscaleY = scl / ( fabs(vp.PitchCos) > 0.01 ? fabs(vp.PitchCos) : 0.01 );
				thing->isotheta = 180.0 * asin( sineisotheta / thing->isoscaleY ) / M_PI;
			}
		}

		r.Scale(sprscale.X, isSpriteShadow ? sprscale.Y * 0.15 * thing->isoscaleY : sprscale.Y * thing->isoscaleY);

		if (((thing->renderflags & RF_ROLLSPRITE) || (thing->renderflags2 & RF2_SQUAREPIXELS)) && !(thing->renderflags2 & RF2_STRETCHPIXELS))
		{
			double ps = di->Level->pixelstretch;
			double mult = 1.0 / sqrt(ps); // shrink slightly
			r.Scale(mult * ps, mult);
		}

		float rightfac = -r.left;
		float leftfac = rightfac - r.width;
		z1 = z - r.top;
		z2 = z1 - r.height;

		float spriteheight = sprscale.Y * r.height * thing->isoscaleY;

		// Tests show that this doesn't look good for many decorations and corpses
		if (spriteheight > 0 && gl_spriteclip > 0 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
		{
			PerformSpriteClipAdjustment(thing, thingpos.XY(), spriteheight);
		}

		switch (spritetype)
		{
		case RF_FACESPRITE:
		{
			float viewvecX = vp.ViewVector.X;
			float viewvecY = vp.ViewVector.Y;

			x1 = x - viewvecY*leftfac;
			x2 = x - viewvecY*rightfac;
			y1 = y + viewvecX*leftfac;
			y2 = y + viewvecX*rightfac;
			if (thing->renderflags2 & RF2_ISOMETRICSPRITES) // If sprites are drawn from an isometric perspective
			{
				x1 -= viewvecX * thing->radius * MY_SQRT2;
				x2 -= viewvecX * thing->radius * MY_SQRT2;
				y1 -= viewvecY * thing->radius * MY_SQRT2;
				y2 -= viewvecY * thing->radius * MY_SQRT2;
			}
			break;
		}
		case RF_FLATSPRITE:
		{
			float bottomfac = -r.top;
			float topfac = bottomfac - r.height;

			x1 = x + leftfac;
			x2 = x + rightfac;
			y1 = y - topfac;
			y2 = y - bottomfac;
			// [MC] Counteract in case of any potential problems. Tests so far haven't
			// shown any outstanding issues but that doesn't mean they won't appear later
			// when more features are added.
			z1 += offy;
			z2 += offy;
			break;
		}
		case RF_WALLSPRITE:
		{
			float viewvecX = Angles.Yaw.Cos();
			float viewvecY = Angles.Yaw.Sin();

			x1 = x + viewvecY*leftfac;
			x2 = x + viewvecY*rightfac;
			y1 = y - viewvecX*leftfac;
			y2 = y - viewvecX*rightfac;
			break;
		}
		}
	}
	else
	{
		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		texture = nullptr;
	}

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);
	if(thing->renderflags2 & RF2_ISOMETRICSPRITES) depth = depth * vp.PitchCos - vp.PitchSin * z2; // Helps with stacking actors with small xy offsets
	if (isSpriteShadow) depth += 1.f/65536.f; // always sort shadows behind the sprite.

	if (gl_spriteclip == -1 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE) // perform anamorphosis
	{
		float minbias = r_spriteclipanamorphicminbias;
		minbias = clamp(minbias, 0.3f, 1.0f);

		float btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		float top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		float vbtm = thing->Sector->floorplane.ZatPoint(vp.Pos);
		float vtop = thing->Sector->ceilingplane.ZatPoint(vp.Pos);

		float vpx = vp.Pos.X;
		float vpy = vp.Pos.Y;
		float vpz = vp.Pos.Z;

		float tpx = thingpos.X;
		float tpy = thingpos.Y;
		float tpz = thingpos.Z;

		if (!(r_debug_nolimitanamorphoses))
		{
			// this should help prevent clipping through walls ...
			float objradiusbias = 1.f - thing->radius / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
			minbias = max(minbias, objradiusbias);
		}

		float bintersect, tintersect;
		if (z2 < vpz && vbtm < vpz)
			bintersect = min((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
		else
			bintersect = 1.0;

		if (z1 > vpz && vtop > vpz)
			tintersect = min((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
		else
			tintersect = 1.0;

		if (thing->waterlevel >= 1 && thing->waterlevel <= 2)
			bintersect = tintersect = 1.0f;

		float spbias = clamp(min(bintersect, tintersect), minbias, 1.0f);
		float vpbias = 1.0 - spbias;
		x1 = x1 * spbias + vpx * vpbias;
		y1 = y1 * spbias + vpy * vpbias;
		z1 = z1 * spbias + vpz * vpbias;
		x2 = x2 * spbias + vpx * vpbias;
		y2 = y2 * spbias + vpy * vpbias;
		z2 = z2 * spbias + vpz * vpbias;		
	}

	// light calculation

	bool enhancedvision = false;

	// allow disabling of the fullbright flag by a brightmap definition
	// (e.g. to do the gun flashes of Doom's zombies correctly.
	fullbright = (thing->flags5 & MF5_BRIGHT) ||
		((thing->renderflags & RF_FULLBRIGHT) && (!texture || !texture->isFullbrightDisabled()));

	if (fullbright)	lightlevel = 255;
	else lightlevel = hw_ClampLight(thing->GetLightLevel(rendersector));

	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255); // this *must* use the sector's light level or the fog will just look bad.

	lightlevel = rendersector->CheckSpriteGlow(lightlevel, thingpos);

	ThingColor = (thing->RenderStyle.Flags & STYLEF_ColorIsFixed) ? thing->fillcolor : 0xffffff;
	ThingColor.a = 255;
	RenderStyle = thing->RenderStyle;

	// [Nakara] Once +CLOAK starts fading below Alpha 1.0, use regular
	// source-alpha blending so Actor.Alpha remains the visible amount even when
	// the actor's declared style is Normal. At Alpha 1.0 the original style is
	// preserved because the cloak amount is zero.
	if (nkCloakActor && thing->Alpha < 1.0)
	{
		RenderStyle = LegacyRenderStyles[STYLE_Translucent];
	}

	// colormap stuff is a little more complicated here...
	if (di->isFullbrightScene())
	{
		enhancedvision = di->isStealthVision();

		Colormap.Clear();

		if (di->isNightvision())
		{
			if ((thing->IsKindOf(NAME_Inventory) || thing->flags3&MF3_ISMONSTER || thing->flags&MF_MISSILE || thing->flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &di->Level->sectors[rendersector->sectornum] || in_area != area_below)
				// under water areas keep their color for fullbright objects
			{
				// Only make the light white but keep everything else (fog, desaturation and Boom colormap.)
				Colormap.MakeWhite();
			}
			else
			{
				// Keep the color, but brighten things a bit so that a difference can be seen.
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = thing->Translation;

	OverrideShader = -1;
	trans = thing->Alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;	// trans may not be 1 here
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				// Without shaders only the standard effect is available.
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency)
	{
		// [SP] "canonical transparency" - with the flip of a CVar, disable transparency for Doom objects,
		//   and disable 'additive' translucency for certain objects from other games.
		if (thing->renderflags & RF_ZDOOMTRANS)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}

	// [Nakara] The through-wall redraw uses a separate sprite list. Most normal sprites are
	// still rendered with an opaque STYLE_Normal blend state, so changing only the shader
	// output alpha does not actually make them translucent. Force only this redraw copy to
	// use normal source-alpha blending, and feed the requested opacity through the sprite
	// color alpha path used by SetColor().
	if (di != nullptr && (di->VisThruWallCollectPass || di->VisThruWallRenderPass) &&
		(thing->flags9 & MF9_VISTHRUWALL) && thing->bVisThruWallSpriteAlphaSet)
	{
		float visAlpha = float(clamp<double>(thing->VisThruWallSpriteAlpha, 0.0, 1.0));

		if (thing->VisThruWallMaxDistance > 0.0 && thing->VisThruWallFadeDistance > 0.0)
		{
			double maxDistance = max<double>(thing->VisThruWallMaxDistance, 0.0);
			double fadeDistance = clamp<double>(thing->VisThruWallFadeDistance, 0.0, maxDistance);

			if (fadeDistance > 0.0)
			{
				double distance = (thing->InterpolatedPosition(vp.TicFrac) - vp.Pos).Length();
				visAlpha *= float(clamp<double>((maxDistance - distance) / fadeDistance, 0.0, 1.0));
			}
		}

		float runtimeFade = float(clamp<double>(thing->VisThruWallCurrentFade, 0.0, 1.0));
		if (di != nullptr)
		{
			runtimeFade *= float(clamp<double>(di->VisThruWallGlobalFade, 0.0, 1.0));
		}
		visAlpha *= runtimeFade;
		trans *= visAlpha;
		RenderStyle = LegacyRenderStyles[STYLE_Translucent];
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
		trans = 1.f;

		if (!gl_sprite_blend || modelframe ||
			(thing->renderflags & (RF_FLATSPRITE | RF_WALLSPRITE)) ||
			(hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && !(thing->renderflags2 & RF2_BILLBOARDNOFACECAMERA)) ||
			thing->renderflags2 & RF2_BILLBOARDFACECAMERA))
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((texture && texture->GetTranslucency()) || (RenderStyle.Flags & STYLEF_RedIsAlpha) || (modelframe && thing->RenderStyle != DefaultRenderStyle()))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			// enhanced vision makes them more visible!
			trans = 0.5f;
			FRenderStyle rs = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
		}
		else if (thing->flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	// for sprite shadow, use a translucent stencil renderstyle
	if (isSpriteShadow)
	{
		RenderStyle = STYLE_Stencil;
		ThingColor = MAKEARGB(255, 0, 0, 0);
		// fade shadow progressively as the thing moves higher away from the floor
		if (r_actorspriteshadowfadeheight > 0.0) {
			trans *= clamp(0.0f, float(r_actorspriteshadowalpha - (thingpos.Z - thing->floorz) * (1.0 / r_actorspriteshadowfadeheight)), float(r_actorspriteshadowalpha));
		} else {
			trans *= r_actorspriteshadowalpha;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	// [Nakara] Fully cloaked actors still need to reach the draw call so the
	// dedicated G-buffer cloak mask is written even though their visible color
	// contribution is zero.
	if (trans == 0.0f && !nkCloakActor) return;

	// end of light calculation

	actor = thing;
	index = thing->SpawnOrder;

	// sprite shadows should have a fixed index of -1 (ensuring they're drawn behind particles which have index 0)
	// sorting should be irrelevant since they're always translucent
	if (isSpriteShadow)
	{
		index = -1;
	}

	particle = nullptr;

	const bool drawWithXYBillboard = (!(actor->renderflags & RF_FORCEYBILLBOARD)
		&& (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE
		&& (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD));


	// no light splitting when:
	// 1. no lightlist
	// 2. any fixed colormap
	// 3. any bright object
	// 4. any with render style shadow (which doesn't use the sector light)
	// 5. anything with render style reverse subtract (light effect is not what would be desired here)
	if (thing->Sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)	// on old hardware we are rather limited...
		{
			lightlist = nullptr;
			if (!drawWithXYBillboard && !modelframe)
			{
				SplitSprite(di, thing->Sector, hw_styleflags != STYLEHW_Solid);
			}
		}
		else
		{
			lightlist = &thing->Sector->e->XFloor.lightlist;
		}
	}
	else
	{
		lightlist = nullptr;
	}

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;

	// [Nakara] Ribbon trails use the same center-line mesh for both sprite and
	// model sources. PTTRL2D_AfterImage remains the only sprite-shaped trail.
	// Each retained Generation becomes its own HWSprite draw copy, so a portal
	// split can never connect entrance and exit center-lines. Older generations
	// remain visible until ParticleTrail.Lifetime expires and fade independently.
	const bool specialCollectPass = di->VisThruWallCollectPass || di->VisThruWallOccluderCollectPass ||
		di->VisThruWallSpriteMaskCollectPass || di->VisThruWallRenderPass;
	if (!isSpriteShadow && !specialCollectPass && thruportal != 2 &&
		thing->ParticleTrailHistory.Size() > 0 && thing->ParticleTrailHistoryMode == 2)
	{
		const double lifeSeconds = thing->ParticleTrailLifetime > 0.0 ? thing->ParticleTrailLifetime : 0.35;
		const double lifeTicks = max<double>(1.0, lifeSeconds * TICRATE);
		const double now = (double)thing->Level->maptime - 1.0 + vp.TicFrac;
		const double baseAlpha = thing->ParticleTrailAlpha > 0.0 ?
			clamp<double>(thing->ParticleTrailAlpha, 0.0, 1.0) : 1.0;
		const int renderGroup = sector != nullptr ? sector->PortalGroup :
			(thing->Sector != nullptr ? thing->Sector->PortalGroup : thing->PrevPortalGroup);

		// Generations are appended chronologically and therefore form contiguous
		// runs in history. Walk each run independently instead of collecting IDs in
		// another TArray (important: GZDoom TArray::Reserve changes Count).
		int runEnd = (int)thing->ParticleTrailHistory.Size() - 1;
		while (runEnd >= 0)
		{
			const uint32_t generation = thing->ParticleTrailHistory[runEnd].Generation;
			int runStart = runEnd;
			while (runStart > 0 && thing->ParticleTrailHistory[runStart - 1].Generation == generation)
			{
				--runStart;
			}

			// Completed generations are detached to source-side VisualThinkers. Never
			// draw a stale old generation through the actor's current portal context.
			if (generation != thing->ParticleTrailGeneration)
			{
				runEnd = runStart - 1;
				continue;
			}

			int newestIndex = -1;
			for (int i = runEnd; i >= runStart; --i)
			{
				const auto &sample = thing->ParticleTrailHistory[i];
				if (!std::isfinite(sample.SpawnTime) || sample.SpawnTime > now + 0.000001) continue;
				const double age = now - sample.SpawnTime;
				if (age >= 0.0 && age < lifeTicks)
				{
					newestIndex = i;
					break;
				}
			}

			if (generation != 0 && newestIndex >= 0)
			{
				const double newestAge = now - thing->ParticleTrailHistory[newestIndex].SpawnTime;
				const bool currentGeneration = generation == thing->ParticleTrailGeneration;
				const double generationFade = currentGeneration ? 1.0 :
					clamp<double>(1.0 - newestAge / lifeTicks, 0.0, 1.0);

				if (generationFade > 0.0001)
				{
					HWSprite trail = *this;
					trail.isParticleTrailMesh = true;
					trail.particleTrailSource = 2;
					trail.particleTrailGeneration = generation;
					trail.particleTrailSeamCap = 0;
					trail.particleTrailTailFadePass = 0;
					trail.particleTrailWidthScale = 1.0f;
					trail.particleTrailRenderPortalGroup = renderGroup;
					trail.particleTrailVertexCount = 0;
					trail.vertexindex = -1;
					trail.modelframe = nullptr;
					trail.texture = nullptr;
					trail.translation = NO_TRANSLATION;
					trail.RenderStyle = STYLE_Translucent;
					trail.ThingColor = thing->bParticleTrailColorSet ? thing->ParticleTrailColor : PalEntry(255, 255, 255);
					trail.ThingColor.a = 255;
					trail.trans = float(baseAlpha * generationFade);
					trail.fullbright = true;
					trail.lightlevel = 255;
					trail.hw_styleflags = STYLEHW_NoAlphaTest;
					trail.OverrideShader = 0;
					trail.lightlist = nullptr;
					trail.particle = nullptr;
					trail.polyoffset = false;
					trail.topclip = LARGE_VALUE;
					trail.bottomclip = -LARGE_VALUE;
					trail.dynlightindex = -1;

					const auto& runFirst = thing->ParticleTrailHistory[runStart];
					const auto& runLast = thing->ParticleTrailHistory[runEnd];
					const bool useFeather = thing->ParticleTrailTailAlphaFade > 0.0001 || thing->ParticleTrailHeadFeather > 0.0001;

					auto putRibbonLayer = [&](HWSprite layer)
					{
						if (layer.trans <= 0.0001f) return;

						layer.PutSprite(di, true);
						rendered_sprites++;

						if (useFeather)
						{
							for (uint8_t tailPass = 1; tailPass <= PARTICLETRAIL_TAIL_ALPHA_BUCKETS; ++tailPass)
							{
								HWSprite feather = layer;
								feather.particleTrailTailFadePass = tailPass;
								feather.particleTrailVertexCount = 0;
								feather.vertexindex = -1;
								feather.PutSprite(di, true);
								rendered_sprites++;
							}
						}

						auto putSeamCap = [&](uint8_t capMode, const FParticleTrailHistorySample& seamSample)
						{
							const double seamAge = now - seamSample.SpawnTime;
							if (!std::isfinite(seamSample.SpawnTime) || seamAge < 0.0 || seamAge >= lifeTicks) return;

							HWSprite cap = layer;
							cap.particleTrailSeamCap = capMode;
							cap.particleTrailTailFadePass = 0;
							cap.particleTrailVertexCount = 0;
							cap.vertexindex = -1;
							if (cap.trans <= 0.0001f) return;
							cap.PutSprite(di, true);
							rendered_sprites++;
						};

						if (runLast.PortalSeamFlags & PTHSF_PortalEntry) putSeamCap(1, runLast);
						if (runFirst.PortalSeamFlags & PTHSF_PortalExit) putSeamCap(2, runFirst);
					};

					// [Nakara V24] The existing ribbon remains the inner core. An optional
					// wider translucent copy is submitted first as the outer glow, sharing
					// the exact same center-line, feather passes, and portal seam covers.
					const double glowScale = clamp<double>(thing->ParticleTrailGlowScale, 0.0, 8.0);
					const double glowAlpha = clamp<double>(thing->ParticleTrailGlowAlpha, 0.0, 1.0);
					if (glowScale > 0.0001 && glowAlpha > 0.0001)
					{
						HWSprite glow = trail;
						glow.particleTrailWidthScale = float(glowScale);
						glow.ThingColor = thing->bParticleTrailGlowColorSet ? thing->ParticleTrailGlowColor : trail.ThingColor;
						glow.ThingColor.a = 255;
						glow.trans = float(glowAlpha * generationFade);
						putRibbonLayer(glow);
					}

					putRibbonLayer(trail);

					if (nk_ribbon_debug && !currentGeneration && ParticleTrailDebugAllowRenderLine(thing))
					{
						Printf("[RIBDBG:RENDER-SPLIT-GEN] tic=%d frac=%.4f actor=%p class=%s gen=%u current=%u "
							"run=%d..%d newest=%d age=%.3f fade=%.3f group=%d\n",
							thing->Level->maptime, vp.TicFrac, (void*)thing, thing->GetClass()->TypeName.GetChars(),
							(unsigned)generation, (unsigned)thing->ParticleTrailGeneration, runStart, runEnd, newestIndex,
							newestAge, generationFade, renderGroup);
					}
				}
			}

			runEnd = runStart - 1;
		}
	}


}


//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::ProcessParticle(HWDrawInfo *di, particle_t *particle, sector_t *sector, DVisualThinker *spr)//, int shade, int fakeside)
{
	isSpriteShadow = false;
	isParticleTrailMesh = false;
	particleTrailVertexCount = 0;
	particleTrailSource = 0;
	particleTrailGeneration = 0;
	particleTrailSeamCap = 0;
	particleTrailTailFadePass = 0;
	particleTrailWidthScale = 1.0f;
	particleTrailRenderPortalGroup = 0;
	particleTrailVisual = nullptr;

	if (!particle || particle->alpha <= 0)
		return;

	// [Nakara] Detached ribbon carrier. It intentionally has no sprite texture;
	// the VisualThinker exists only to preserve source-side render ownership after
	// a portal split or after the projectile itself has been destroyed.
	if (spr && spr->bParticleTrailRibbonCarrier)
	{
		if (di == nullptr || sector == nullptr || spr->Level == nullptr ||
			spr->ParticleTrailGeneration == 0 || spr->ParticleTrailHistory.Size() < 2)
		{
			return;
		}

		const double lifeSeconds = spr->ParticleTrailLifetime > 0.0 ? spr->ParticleTrailLifetime : 0.35;
		const double lifeTicks = max<double>(1.0, lifeSeconds * TICRATE);
		const double now = (double)spr->Level->maptime - 1.0 + di->Viewpoint.TicFrac;
		// [Nakara] Do not apply a second whole-generation alpha fade to detached
		// ribbon carriers. Portal entry and exit samples share the same exact
		// crossing time, but the destination/current generation renders at the
		// configured ParticleTrailAlpha while the old source-side carrier used to
		// multiply that alpha by its age. That made an otherwise continuous seam
		// visibly change opacity at the portal. Individual history nodes already
		// shrink by lifetime in BuildParticleTrailMesh, so detached trails still
		// retire smoothly without introducing a second alpha curve.

		bool haveVisibleSample = false;
		for (int i = (int)spr->ParticleTrailHistory.Size() - 1; i >= 0; --i)
		{
			const double sampleAge = now - spr->ParticleTrailHistory[i].SpawnTime;
			if (std::isfinite(spr->ParticleTrailHistory[i].SpawnTime) && sampleAge >= -0.000001 && sampleAge < lifeTicks)
			{
				haveVisibleSample = true;
				break;
			}
		}
		if (!haveVisibleSample) return;

		actor = nullptr;
		this->particle = particle;
		particleTrailVisual = spr;
		isParticleTrailMesh = true;
		particleTrailSource = 2;
		particleTrailGeneration = spr->ParticleTrailGeneration;
		particleTrailSeamCap = 0;
		particleTrailTailFadePass = 0;
		particleTrailWidthScale = 1.0f;
		particleTrailRenderPortalGroup = spr->ParticleTrailPortalGroup;
		particleTrailVertexCount = 0;
		vertexindex = -1;

		modelframe = nullptr;
		texture = nullptr;
		translation = NO_TRANSLATION;
		RenderStyle = STYLE_Translucent;
		ThingColor = spr->ParticleTrailColor;
		ThingColor.a = 255;
		trans = float(clamp<double>(spr->ParticleTrailAlpha, 0.0, 1.0));
		if (trans <= 0.0001f) return;
		fullbright = true;
		lightlevel = 255;
		foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);
		Colormap = sector->Colormap;
		Colormap.ClearColor();
		hw_styleflags = STYLEHW_NoAlphaTest;
		OverrideShader = 0;
		lightlist = nullptr;
		polyoffset = false;
		topclip = LARGE_VALUE;
		bottomclip = -LARGE_VALUE;
		dynlightindex = -1;
		index = 0;

		x = (float)spr->PT.Pos.X;
		y = (float)spr->PT.Pos.Y;
		z = (float)spr->PT.Pos.Z;
		const auto &vp = di->Viewpoint;
		depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

		const auto &first = spr->ParticleTrailHistory[0];
		const auto &last = spr->ParticleTrailHistory.Last();
		const bool useFeather = spr->ParticleTrailTailAlphaFade > 0.0001 || spr->ParticleTrailHeadFeather > 0.0001;

		auto putDetachedRibbonLayer = [&](HWSprite layer)
		{
			if (layer.trans <= 0.0001f) return;

			layer.PutSprite(di, true);
			rendered_sprites++;

			if (useFeather)
			{
				for (uint8_t tailPass = 1; tailPass <= PARTICLETRAIL_TAIL_ALPHA_BUCKETS; ++tailPass)
				{
					HWSprite feather = layer;
					feather.particleTrailTailFadePass = tailPass;
					feather.particleTrailVertexCount = 0;
					feather.vertexindex = -1;
					feather.PutSprite(di, true);
					rendered_sprites++;
				}
			}

			auto putDetachedSeamCap = [&](uint8_t capMode, const FParticleTrailHistorySample &seamSample)
			{
				const double seamAge = now - seamSample.SpawnTime;
				if (!std::isfinite(seamSample.SpawnTime) || seamAge < 0.0 || seamAge >= lifeTicks) return;
				HWSprite cap = layer;
				cap.particleTrailSeamCap = capMode;
				cap.particleTrailTailFadePass = 0;
				cap.particleTrailVertexCount = 0;
				cap.vertexindex = -1;
				if (cap.trans <= 0.0001f) return;
				cap.PutSprite(di, true);
				rendered_sprites++;
			};

			if (last.PortalSeamFlags & PTHSF_PortalEntry) putDetachedSeamCap(1, last);
			if (first.PortalSeamFlags & PTHSF_PortalExit) putDetachedSeamCap(2, first);
		};

		// [Nakara V24] Detached generations preserve the same two-layer appearance.
		if (spr->ParticleTrailGlowScale > 0.0001 && spr->ParticleTrailGlowAlpha > 0.0001)
		{
			HWSprite glow = *this;
			glow.particleTrailWidthScale = float(clamp<double>(spr->ParticleTrailGlowScale, 0.0, 8.0));
			glow.ThingColor = spr->ParticleTrailGlowColor;
			glow.ThingColor.a = 255;
			glow.trans = float(clamp<double>(spr->ParticleTrailGlowAlpha, 0.0, 1.0));
			putDetachedRibbonLayer(glow);
		}

		putDetachedRibbonLayer(*this);
		return;
	}

	if (spr && !spr->ValidTexture())
		return;

	lightlevel = hw_ClampLight(spr ? spr->GetLightLevel(sector) : sector->GetSpriteLight());
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);

	trans = particle->alpha;
	OverrideShader = (particle->flags & SPF_ALLOWSHADERS) ? -1 : 0;
	modelframe = nullptr;
	texture = nullptr;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;
	index = 0;
	actor = nullptr;
	this->particle = particle;
	fullbright = particle->flags & SPF_FULLBRIGHT;

	if (di->isFullbrightScene()) 
	{
		Colormap.Clear();
	}
	else if (!(particle->flags & SPF_FULLBRIGHT))
	{
		TArray<lightlist_t> & lightlist=sector->e->XFloor.lightlist;
		double lightbottom;

		Colormap = sector->Colormap;
		for(unsigned int i=0;i<lightlist.Size();i++)
		{
			if (i<lightlist.Size()-1) lightbottom = lightlist[i+1].plane.ZatPoint(particle->Pos);
			else lightbottom = sector->floorplane.ZatPoint(particle->Pos);

			if (lightbottom < particle->Pos.Z)
			{
				lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
				Colormap.CopyLight(lightlist[i].extra_colormap);
				break;
			}
		}
		if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();	// ZDoom never applies colored light to particles.
		}
	}
	else
	{
		lightlevel = 255;
		Colormap = sector->Colormap;
		Colormap.ClearColor();
	}

	if(particle->style != STYLE_None)
	{
		RenderStyle = particle->style;
	}
	else
	{
		RenderStyle = STYLE_Translucent;
	}

	ThingColor = particle->color;
	ThingColor.a = 255;
	const auto& vp = di->Viewpoint;

	double timefrac = vp.TicFrac;
	if (paused || (di->Level->isFrozen() && !(particle->flags & SPF_NOTIMEFREEZE)))
		timefrac = 0.;

	
	if (spr && !(spr->flags & VTF_IsParticle))
	{
		AdjustVisualThinker(di, spr, sector);
	}
	else
	{
		bool has_texture = false;
		bool custom_animated_texture = false;
		int particle_style = 0;
		float size = particle->size;
		if (!spr)
		{
			has_texture = particle->texture.isValid();
			custom_animated_texture = (particle->flags & SPF_LOCAL_ANIM) && particle->animData.ok;
			particle_style = has_texture ? 2 : gl_particles_style; // Treat custom texture the same as smooth particles
		}
		else
		{
			size = float(spr->Scale.X);
			const int ptype = spr->GetParticleType();
			particle_style = (ptype != PT_DEFAULT) ? ptype : gl_particles_style;
		}
		// [BB] Load the texture for round or smooth particles
		if (particle_style)
		{
			FTextureID lump;
			if (particle_style == 1)
			{
				lump = TexMan.glPart2;
			}
			else if (particle_style == 2)
			{
				if(custom_animated_texture)
				{
					lump = TexAnim.UpdateStandaloneAnimation(particle->animData, di->Level->maptime + timefrac);
				}
				else if(has_texture)
				{
					lump = particle->texture;
				}
				else
				{
					lump = TexMan.glPart;
				}
			}
			else
			{
				lump.SetNull();
			}

			if (lump.isValid())
			{
				translation = NO_TRANSLATION;

				ul = vt = 0;
				ur = vb = 1;

				texture = TexMan.GetGameTexture(lump, !custom_animated_texture);
			}
		}


		float xvf = (particle->Vel.X) * timefrac;
		float yvf = (particle->Vel.Y) * timefrac;
		float zvf = (particle->Vel.Z) * timefrac;

		offx = 0.f;
		offy = 0.f;

		x = float(particle->Pos.X) + xvf;
		y = float(particle->Pos.Y) + yvf;
		z = float(particle->Pos.Z) + zvf;

		if (IsNakaraUnderwaterFishParticle(di, particle))
		{
			// Fish simulation stores heading yaw/pitch in RollVel/RollAcc and bank in Roll.
			Angles.Yaw = TAngle<double>::fromDeg(particle->RollVel);
			Angles.Pitch = TAngle<double>::fromDeg(particle->RollAcc);
			Angles.Roll = TAngle<double>::fromDeg(particle->Roll);
		}
		else if(particle->flags & SPF_ROLL)
		{
			float rvf = (particle->RollVel) * timefrac;
			Angles.Roll = TAngle<double>::fromDeg(particle->Roll + rvf);
		}
	
		// [Nakara] A non-zero spriteScaleY marks a textured particle that should
		// retain the source sprite's native aspect ratio and offsets. This is used
		// by +PARTICLETRAIL afterimages and does not affect normal/custom particles.
		if (has_texture && particle->spriteScaleY != 0.0f && texture && texture->isValid())
		{
			auto& spi = texture->GetSpritePositioning(1);
			vt = spi.GetSpriteVT();
			vb = spi.GetSpriteVB();
			ul = spi.GetSpriteUR();
			ur = spi.GetSpriteUL();
			if (particle->spriteScaleY < 0.0f)
			{
				std::swap(ul, ur);
			}

			auto r = spi.GetSpriteRect();
			r.Scale(size, fabs(particle->spriteScaleY));

			float viewvecX = vp.ViewVector.X;
			float viewvecY = vp.ViewVector.Y;
			float rightfac = -r.left;
			float leftfac = rightfac - r.width;

			x1 = x - viewvecY * leftfac;
			x2 = x - viewvecY * rightfac;
			y1 = y + viewvecX * leftfac;
			y2 = y + viewvecX * rightfac;
			z1 = z - r.top;
			z2 = z1 - r.height;

			depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);
			hw_styleflags = STYLEHW_NoAlphaTest;
		}
		else
		{
			float factor;
			if (particle_style == 1) factor = 1.3f / 7.f;
			else if (particle_style == 2) factor = 2.5f / 7.f;
			else factor = 1 / 7.f;
			float scalefac= size * factor;

			float ps = di->Level->pixelstretch;

			scalefac /= sqrt(ps); // shrink it slightly to account for the stretch

			float viewvecX = vp.ViewVector.X * scalefac * ps;
			float viewvecY = vp.ViewVector.Y * scalefac;

			x1=x+viewvecY;
			x2=x-viewvecY;
			y1=y-viewvecX;
			y2=y+viewvecX;
			z1=z-scalefac;
			z2=z+scalefac;

			depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);
	
			// [BB] Translucent particles have to be rendered without the alpha test.
			if (particle_style != 2 && trans>=1.0f-FLT_EPSILON) hw_styleflags = STYLEHW_Solid;
			else hw_styleflags = STYLEHW_NoAlphaTest;
		}
	}

	if (sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright)
		lightlist = &sector->e->XFloor.lightlist;
	else
		lightlist = nullptr;

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

// [MC] VisualThinkers are to be rendered akin to actor sprites. The reason this whole system
// is hitching a ride on particle_t is because of the large number of checks with 
// HWSprite elsewhere in the draw lists.
void HWSprite::AdjustVisualThinker(HWDrawInfo* di, DVisualThinker* spr, sector_t* sector)
{
	translation = spr->Translation;

	const auto& vp = di->Viewpoint;
	double timefrac = vp.TicFrac;

	if (paused || spr->isFrozen())
		timefrac = 0.;
	
	bool custom_anim = ((spr->PT.flags & SPF_LOCAL_ANIM) && spr->PT.animData.ok);

	texture = TexMan.GetGameTexture(
			custom_anim
			? TexAnim.UpdateStandaloneAnimation(spr->PT.animData, di->Level->maptime + timefrac)
			: spr->PT.texture, !custom_anim);

	if (spr->flags & VTF_DontInterpolate)
		timefrac = 0.;

	FVector3 interp = spr->InterpolatedPosition(timefrac);
	x = interp.X;
	y = interp.Y;
	z = interp.Z;

	offx = (float)spr->GetOffset(false);
	offy = (float)spr->GetOffset(true);

	if (spr->PT.flags & SPF_ROLL)
		Angles.Roll = TAngle<double>::fromDeg(spr->InterpolatedRoll(timefrac));

	auto& spi = texture->GetSpritePositioning(0);

	vt = spi.GetSpriteVT();
	vb = spi.GetSpriteVB();
	ul = spi.GetSpriteUR();
	ur = spi.GetSpriteUL();

	auto r = spi.GetSpriteRect();
	r.Scale(spr->Scale.X, spr->Scale.Y);

	if ((spr->PT.flags & SPF_ROLL) && !(spr->PT.flags & SPF_STRETCHPIXELS))
	{
		double ps = di->Level->pixelstretch;
		double mult = 1.0 / sqrt(ps); // shrink slightly
		r.Scale(mult * ps, mult);
	}
	if (spr->flags & VTF_FlipX)
	{
		std::swap(ul,ur);
		r.left = -r.width - r.left;	// mirror the sprite's x-offset
	}
	if (spr->flags & VTF_FlipY)	std::swap(vt,vb);

	float viewvecX = vp.ViewVector.X;
	float viewvecY = vp.ViewVector.Y;
	float rightfac = -r.left;
	float leftfac = rightfac - r.width;

	x1 = x - viewvecY * leftfac;
	x2 = x - viewvecY * rightfac;
	y1 = y + viewvecX * leftfac;
	y2 = y + viewvecX * rightfac;
	z1 = z - r.top;
	z2 = z1 - r.height;

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

	// [BB] Translucent particles have to be rendered without the alpha test.
	hw_styleflags = STYLEHW_NoAlphaTest;
}

//==========================================================================
//
// 
//
//==========================================================================

void HWDrawInfo::ProcessActorsInPortal(FLinePortalSpan *glport, area_t in_area)
{
	TMap<AActor*, bool> processcheck;
	if (glport->validcount == validcount) return;	// only process once per frame
	glport->validcount = validcount;
    const auto &vp = Viewpoint;
	for (auto port : glport->lines)
	{
		line_t *line = port->mOrigin;
		if (line->isLinePortal())	// only crossable ones
		{
			FLinePortal *port2 = port->mDestination->getPortal();
			// process only if the other side links back to this one.
			if (port2 != nullptr && port->mDestination == port2->mOrigin && port->mOrigin == port2->mDestination)
			{
				for (portnode_t *node = port->lineportal_thinglist; node != nullptr; node = node->m_snext)
				{
					AActor *th = node->m_thing;

					// process each actor only once per portal.
					bool *check = processcheck.CheckKey(th);
					if (check && *check) continue;
					processcheck[th] = true;

					DAngle savedangle = th->Angles.Yaw;
					DVector3 savedpos = th->Pos();
					DVector3 newpos = savedpos;
					sector_t fakesector;

					if (!vp.showviewer)
					{
						AActor *viewmaster = th;
						if ((th->flags8 & MF8_MASTERNOSEE) && th->master != nullptr)
						{
							viewmaster = th->master;
						}

						if (viewmaster == vp.camera)
						{
							DVector3 vieworigin = viewmaster->Pos();

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the original position
								continue;
							}

							P_TranslatePortalXY(line, vieworigin.X, vieworigin.Y);
							P_TranslatePortalZ(line, vieworigin.Z);

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the translated position
								// (This is required for MASTERNOSEE actors with 3D models)
								continue;
							}
						}
					}

					P_TranslatePortalXY(line, newpos.X, newpos.Y);
					P_TranslatePortalZ(line, newpos.Z);
					P_TranslatePortalAngle(line, th->Angles.Yaw);
					th->SetXYZ(newpos);
					th->Prev += newpos - savedpos;

					HWSprite spr;

					// [Nash] draw sprite shadow
					if (R_ShouldDrawSpriteShadow(th))
					{
						spr.Process(this, th, hw_FakeFlat(th->Sector, in_area, false, &fakesector), in_area, 2, true);
					}

					// This is called from the worker thread and must not alter the fake sector cache.
					spr.Process(this, th, hw_FakeFlat(th->Sector, in_area, false, &fakesector), in_area, 2);
					th->Angles.Yaw = savedangle;
					th->SetXYZ(savedpos);
					th->Prev -= newpos - savedpos;
				}
			}
		}
	}
}
