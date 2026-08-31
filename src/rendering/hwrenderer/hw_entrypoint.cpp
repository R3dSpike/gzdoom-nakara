// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2004-2016 Christoph Oelckers
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
** gl_scene.cpp
** manages the rendering of the player's view
**
*/

#include "gi.h"
#include "a_dynlight.h"
#include "m_png.h"
#include "doomstat.h"
#include "r_data/r_interpolate.h"
#include "r_utility.h"
#include "d_player.h"
#include "i_time.h"
#include "swrenderer/r_swscene.h"
#include "swrenderer/r_renderer.h"
#include "swrenderer/textures/r_swtexture.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "v_palette.h"
#include "d_main.h"
#include "g_cvars.h"
#include "v_draw.h"
#include "texturemanager.h"
#include "r_defs.h"
#include "r_data/models.h"
#include <string.h>
#include <algorithm>

#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/hw_shadowmap.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_vrmodes.h"
#include "p_trace.h"

EXTERN_CVAR(Bool, cl_capfps)
extern bool NoInterpolateView;
extern TArray<spritedef_t> sprites;

static SWSceneDrawer *swdrawer;

void CleanSWDrawer()
{
	if (swdrawer) delete swdrawer;
	swdrawer = nullptr;
}

#include "g_levellocals.h"
#include "a_dynlight.h"


// [Nakara] v11 receiver-shader sprite projectors.
// This path does not spawn actor shadows and does not draw floor/wall quads.
// It uploads small alpha bitmasks and sprite-plane/light data to an SSBO that main.fp
// samples for every world receiver fragment. OpenGL/SSBO path first; Vulkan is intentionally
// left for a later pass.
CVAR(Bool, nk_receiver_sprite_shadows, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nk_receiver_sprite_shadows_max_projectors, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_actor_distance, 1024.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_light_distance, 384.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_strength, 0.65f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_second_light_scale, 0.55f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_mask_softness, 0.10f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_max_length, 1024.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_min_light_radius, 16.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v30: hard light range guards. light_distance is an absolute actor-light cutoff;
// strict_light_range additionally rejects lights whose own radius does not reach the caster.
CVAR(Bool, nk_receiver_sprite_shadows_strict_light_range, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_light_radius_scale, 1.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, nk_receiver_sprite_shadows_alpha_threshold, 24, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_receiver_sprite_shadows_debug_fullmask, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// 0=normal, 1=solid all world receivers, 2=plane-hit only, 3=UV-rect only. Diagnostic only.
CVAR(Int, nk_receiver_sprite_shadows_debug_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_receiver_sprite_shadows_allow_dontlightmap, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_receiver_sprite_shadows_scan_section_lights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_receiver_sprite_shadows_scan_all_lights, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v16: CPU alpha mask orientation fix. Leave this on if the projected receiver shadow is horizontally reversed.
CVAR(Bool, nk_receiver_sprite_shadows_flip_mask_x, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_receiver_sprite_shadows_flip_mask_y, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v23: The normal path no longer builds synthetic light/actor/view-facing planes.
// Receiver projectors now use the same visible sprite quad anchor as the v9 projected
// floor-shadow path. This CVAR is kept only to avoid breaking old configs; it is ignored
// by the v23 normal path.
CVAR(Int, nk_receiver_sprite_shadows_billboard_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Small world-unit offset along the actual v9 caster-plane normal. Use only for tiny
// piercing/anchor diagnostics; leave at 0 for normal testing.
CVAR(Float, nk_receiver_sprite_shadows_plane_push, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Small manual anchor offsets in the projector plane. Use these only for per-stage/prototype tuning
// if a sprite's visual origin is unusual. X follows the projector right vector; Z follows world height.
CVAR(Float, nk_receiver_sprite_shadows_anchor_x_bias, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_anchor_z_bias, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v24 tuning: keep the v23/v9 anchor, but artistically enlarge the caster quad
// near lights and soften/extend the top edge to avoid a harsh clipped shadow cap.
CVAR(Float, nk_receiver_sprite_shadows_near_scale, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_near_scale_distance, 192.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_side_extend, 0.04f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v25: smooth the left/right edges after artistic side expansion. This keeps the
// expanded receiver shadow from showing hard rectangular borders.
CVAR(Float, nk_receiver_sprite_shadows_side_fade, 0.03f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v26: contact-hardening style softness. The shadow remains crisp near the
// caster plane, then gradually softens as the receiver point gets farther
// away from the caster intersection. This keeps the v23/v24/v25 single
// receiver-projector path and does not add slices/line blockers.
CVAR(Float, nk_receiver_sprite_shadows_contact_softness, 0.18f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_contact_distance, 256.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v27: distance-based opacity. Farther receiver points can fade out gradually,
// separate from the hard maxLength clamp. This helps long projected shadows
// feel less like a uniformly stamped mask.
CVAR(Float, nk_receiver_sprite_shadows_distance_opacity, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v28: receiver surface angle fade. This weakens shadows on receiver pixels where
// the light-to-surface ray is grazing the world surface, making the result feel
// less like a flat stamp while still using one v9-anchor projector.
CVAR(Float, nk_receiver_sprite_shadows_surface_angle_fade, 0.18f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_surface_min_dot, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_surface_power, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// v29: performance safety knobs. Keep weak light/projector pairs from entering the per-fragment shader loop.
CVAR(Float, nk_receiver_sprite_shadows_min_score, 0.06f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, nk_receiver_sprite_shadows_fast_path, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_top_extend, 0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, nk_receiver_sprite_shadows_top_fade, 0.08f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static constexpr int NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE = 32;
static constexpr int NK_RECEIVER_SPRITE_SHADOW_MASK_FLOATS = NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE * NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE;

struct NKReceiverSpriteShadowFrameInfo
{
	FSoftwareTexture *swtex = nullptr;
	FloatRect rect;
	int texWidth = 0;
	int texHeight = 0;
	bool mirrorX = false;
	bool flipY = false;
};

struct NKReceiverSpriteShadowCandidate
{
	AActor *Thing = nullptr;
	FDynamicLight *Light = nullptr;
	float Score = 0.f;
	float AlphaScale = 1.f;
};

static bool NKCanCastReceiverSpriteShadow(AActor *thing)
{
	if (!nk_receiver_sprite_shadows || thing == nullptr)
	{
		return false;
	}
	if (!(thing->flags9 & MF9_NKSHADOWCAST))
	{
		return false;
	}
	if (thing->sprite <= 0 || thing->sprite >= (int)sprites.Size())
	{
		return false;
	}
	if (thing->Alpha <= 0.0f || (thing->renderflags & RF_INVISIBLE))
	{
		return false;
	}
	if ((thing->renderflags & RF_SPRITETYPEMASK) != RF_FACESPRITE)
	{
		return false;
	}
	return true;
}

static bool NKGetReceiverSpriteShadowFrameInfo(AActor *thing, FDynamicLight *light, NKReceiverSpriteShadowFrameInfo &info)
{
	info = NKReceiverSpriteShadowFrameInfo();
	if (thing == nullptr || light == nullptr)
	{
		return false;
	}
	if (thing->sprite <= 0 || thing->sprite >= (int)sprites.Size())
	{
		return false;
	}

	bool frameMirror = false;
	// v23: Match the same frame selection used by the visible HWSprite/v9 floor
	// projector path. The receiver projector must use the same sprite frame/offset
	// anchor as the v9 projected floor shadow; otherwise the mask may look correct
	// but the caster plane starts from a different place.
	DVector3 thingpos = thing->InterpolatedPosition(r_viewpoint.TicFrac);
	DAngle spriteViewAngle = (thingpos - r_viewpoint.Pos).Angle();
	if (r_viewpoint.IsOrtho())
	{
		spriteViewAngle = r_viewpoint.Angles.Yaw;
	}
	DAngle sprangle = thing->GetSpriteAngle(spriteViewAngle, r_viewpoint.TicFrac);
	FTextureID patch = sprites[thing->sprite].GetSpriteFrame(thing->frame, -1, sprangle, &frameMirror, !!(thing->renderflags & RF_SPRITEFLIP));
	if (!patch.isValid())
	{
		return false;
	}

	FGameTexture *tex = TexMan.GetGameTexture(patch, false);
	if (tex == nullptr || !tex->isValid())
	{
		return false;
	}

	FSoftwareTexture *swtex = GetSoftwareTexture(tex);
	if (swtex == nullptr)
	{
		return false;
	}

	const int texWidth = swtex->GetPhysicalWidth();
	const int texHeight = swtex->GetPhysicalHeight();
	if (texWidth <= 0 || texHeight <= 0)
	{
		return false;
	}

	const int spriteType = thing->renderflags & RF_SPRITETYPEMASK;
	auto &spi = tex->GetSpritePositioning(spriteType == RF_FACESPRITE);
	FloatRect rect = spi.GetSpriteRect();

	const bool finalMirrorX = frameMirror ^ !!(thing->renderflags & RF_XFLIP) ^ !!(thing->renderflags & RF_SPRITEFLIP);
	if (finalMirrorX)
	{
		rect.left = -rect.width - rect.left;
	}
	rect.Scale((float)thing->Scale.X, (float)thing->Scale.Y);

	info.swtex = swtex;
	info.rect = rect;
	info.texWidth = texWidth;
	info.texHeight = texHeight;
	info.mirrorX = finalMirrorX;
	info.flipY = !!(thing->renderflags & RF_YFLIP);
	return true;
}

static void NKSetReceiverSpriteShadowMaskAlpha(float outMask[NK_RECEIVER_SPRITE_SHADOW_MASK_FLOATS], int x, int y, float alpha)
{
	if (x < 0 || x >= NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE || y < 0 || y >= NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE)
	{
		return;
	}
	const int index = y * NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE + x;
	outMask[index] = clamp<float>(alpha, 0.f, 1.f);
}

static void NKBuildReceiverSpriteShadowMask(const NKReceiverSpriteShadowFrameInfo &info, float outMask[NK_RECEIVER_SPRITE_SHADOW_MASK_FLOATS])
{
	for (int i = 0; i < NK_RECEIVER_SPRITE_SHADOW_MASK_FLOATS; ++i)
	{
		outMask[i] = 0.f;
	}

	if (nk_receiver_sprite_shadows_debug_fullmask)
	{
		for (int i = 0; i < NK_RECEIVER_SPRITE_SHADOW_MASK_FLOATS; ++i)
		{
			outMask[i] = 1.f;
		}
		return;
	}

	const uint32_t *pixels = info.swtex != nullptr ? info.swtex->GetPixelsBgra() : nullptr;
	if (pixels == nullptr || info.texWidth <= 0 || info.texHeight <= 0)
	{
		return;
	}

	const int threshold = clamp<int>(nk_receiver_sprite_shadows_alpha_threshold, 0, 255);
	const bool sampleFlipX = info.mirrorX ^ !!nk_receiver_sprite_shadows_flip_mask_x;
	const bool sampleFlipY = info.flipY ^ !!nk_receiver_sprite_shadows_flip_mask_y;

	for (int my = 0; my < NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE; ++my)
	{
		for (int mx = 0; mx < NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE; ++mx)
		{
			const int x0 = clamp<int>((int)floor((double)mx * info.texWidth / NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE), 0, info.texWidth - 1);
			const int x1 = clamp<int>((int)floor((double)(mx + 1) * info.texWidth / NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE) - 1, x0, info.texWidth - 1);
			const int y0 = clamp<int>((int)floor((double)my * info.texHeight / NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE), 0, info.texHeight - 1);
			const int y1 = clamp<int>((int)floor((double)(my + 1) * info.texHeight / NK_RECEIVER_SPRITE_SHADOW_MASK_SIZE) - 1, y0, info.texHeight - 1);

			int alphaSum = 0;
			int sampleCount = 0;
			for (int sy = y0; sy <= y1; ++sy)
			{
				const int sampleY = sampleFlipY ? (info.texHeight - 1 - sy) : sy;
				for (int sx = x0; sx <= x1; ++sx)
				{
					const int sampleX = sampleFlipX ? (info.texWidth - 1 - sx) : sx;
					const uint32_t pix = pixels[sampleX * info.texHeight + sampleY];
					const int alpha = (int)((pix >> 24) & 255);
					alphaSum += alpha > threshold ? alpha : 0;
					++sampleCount;
				}
			}

			float coverage = sampleCount > 0 ? (float)alphaSum / (255.f * (float)sampleCount) : 0.f;
			// Slightly boost thin pixel-art features so small hair/edge pixels do not vanish
			// when the source sprite is reduced to the receiver mask. This is still a 2D
			// alpha mask, not the old vertical blocker/slice approximation.
			coverage = sqrt(clamp<float>(coverage, 0.f, 1.f));
			NKSetReceiverSpriteShadowMaskAlpha(outMask, mx, my, coverage);
		}
	}
}


// v23: Build the receiver caster plane from the same visible sprite anchor that
// the v9 projected floor shadow used. This is intentionally not a synthetic
// light-facing/actor-facing plane. It recreates the RF_FACESPRITE setup from
// HWSprite::Process up to x1/x2/y1/y2/z1/z2, then converts the real sprite quad
// into origin/right/up half-vectors for the receiver shader.
static bool NKBuildReceiverSpriteShadowV9CasterBasis(AActor *thing, const NKReceiverSpriteShadowFrameInfo &frame,
	DVector3 &outOrigin, DVector3 &outRight, DVector3 &outUp)
{
	if (thing == nullptr || frame.swtex == nullptr)
	{
		return false;
	}
	if ((thing->renderflags & RF_SPRITETYPEMASK) != RF_FACESPRITE)
	{
		return false;
	}

	DVector3 thingpos = thing->InterpolatedPosition(r_viewpoint.TicFrac);
	DVector2 sprscale(thing->Scale.X, thing->Scale.Y);
	int spritenum = thing->sprite;
	if (thing->player != nullptr)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// Match the HWSprite::Process base position rules for face sprites.
	double x = thingpos.X + thing->WorldOffset.X;
	double y = thingpos.Y + thing->WorldOffset.Y;
	double z = thingpos.Z + thing->WorldOffset.Z - thing->Floorclip;

	// Renderer-only bobbing is part of the visible sprite position and was also
	// present in v9 before the projected floor vertices were generated.
	z += thing->GetBobOffset(r_viewpoint.TicFrac);

	FloatRect r = frame.rect;
	// frame.rect is already scaled by thing->Scale in NKGetReceiverSpriteShadowFrameInfo.
	// Apply the isometric vertical scale used by the real HWSprite path when present.
	float isoscaleY = 1.0f;
	if (thing->renderflags2 & RF2_ISOMETRICSPRITES)
	{
		// Keep this conservative in the receiver path. The full isometric correction
		// depends on temporary HWSprite members; for Nakara's normal face sprites this
		// branch should normally remain unused.
		isoscaleY = thing->isoscaleY != 0.0 ? thing->isoscaleY : 1.0f;
		r.Scale(1.0f, isoscaleY);
	}

	const float rightfac = -r.left;
	const float leftfac = rightfac - r.width;
	const float zTop = (float)(z - r.top);
	const float zBottom = zTop - r.height;

	float viewvecX = r_viewpoint.ViewVector.X;
	float viewvecY = r_viewpoint.ViewVector.Y;
	if (r_viewpoint.IsOrtho())
	{
		viewvecX = r_viewpoint.Angles.Yaw.Cos();
		viewvecY = r_viewpoint.Angles.Yaw.Sin();
	}

	const double x1 = x - viewvecY * leftfac;
	const double x2 = x - viewvecY * rightfac;
	const double y1 = y + viewvecX * leftfac;
	const double y2 = y + viewvecX * rightfac;

	// Map-space quad matching HWSprite CalculateVertices for a non-XY face sprite:
	// top-left/top-right/bottom-left/bottom-right in map coordinates (X,Y,Z).
	const DVector3 v0(x1, y1, zTop);
	const DVector3 v1(x2, y2, zTop);
	const DVector3 v2(x1, y1, zBottom);
	const DVector3 v3(x2, y2, zBottom);

	outOrigin = (v0 + v1 + v2 + v3) * 0.25;
	outRight = ((v1 - v0) + (v3 - v2)) * 0.25;
	outUp = ((v0 - v2) + (v1 - v3)) * 0.25;

	// Manual biases are now applied on top of the v9 anchor, not instead of it.
	if (nk_receiver_sprite_shadows_anchor_x_bias != 0.0f)
	{
		DVector3 rightN = outRight;
		double len = rightN.Length();
		if (len > 0.001)
		{
			rightN /= len;
			outOrigin += rightN * (double)nk_receiver_sprite_shadows_anchor_x_bias;
		}
	}
	if (nk_receiver_sprite_shadows_anchor_z_bias != 0.0f)
	{
		outOrigin.Z += (double)nk_receiver_sprite_shadows_anchor_z_bias;
	}
	if (nk_receiver_sprite_shadows_plane_push != 0.0f)
	{
		// Push along the actual caster plane normal. Positive generally moves the
		// plane out of the wall/receiver and helps diagnose tiny piercing issues.
		DVector3 n(outRight.Y * outUp.Z - outRight.Z * outUp.Y,
			outRight.Z * outUp.X - outRight.X * outUp.Z,
			outRight.X * outUp.Y - outRight.Y * outUp.X);
		double len = n.Length();
		if (len > 0.001)
		{
			n /= len;
			outOrigin += n * (double)nk_receiver_sprite_shadows_plane_push;
		}
	}

	return outRight.LengthSquared() > 0.001 && outUp.LengthSquared() > 0.001;
}

static float NKReceiverSpriteShadowScore(FDynamicLight *light, const DVector3 &lightPos, const DVector3 &actorPos, double maxDistance)
{
	const double dx = lightPos.X - actorPos.X;
	const double dy = lightPos.Y - actorPos.Y;
	const double dz = lightPos.Z - actorPos.Z;
	const double dist = sqrt(dx * dx + dy * dy + dz * dz);
	const float rangeFade = (float)clamp<double>(1.0 - dist / max<double>(maxDistance, 1.0), 0.0, 1.0);
	const float brightness = clamp<float>((max(max(light->GetRed(), light->GetGreen()), light->GetBlue())) / 255.f, 0.15f, 1.0f);
	const float radiusScore = clamp<float>((float)(light->GetRadius() / max<double>(maxDistance, 1.0)), 0.05f, 1.0f);
	return clamp<float>(rangeFade * (brightness * 0.75f + radiusScore * 0.25f), 0.0f, 1.0f);
}

static void NKInsertReceiverSpriteShadowCandidate(TArray<NKReceiverSpriteShadowCandidate> &candidates, const NKReceiverSpriteShadowCandidate &candidate, int maxProjectors)
{
	if (candidate.Thing == nullptr || candidate.Light == nullptr || candidate.Score <= 0.f || maxProjectors <= 0)
	{
		return;
	}

	int insertAt = (int)candidates.Size();
	for (unsigned int i = 0; i < candidates.Size(); ++i)
	{
		if (candidate.Score > candidates[i].Score)
		{
			insertAt = (int)i;
			break;
		}
	}
	if (insertAt >= maxProjectors)
	{
		return;
	}

	candidates.Insert(insertAt, candidate);
	if ((int)candidates.Size() > maxProjectors)
	{
		candidates.Pop();
	}
}

static bool NKBuildReceiverSpriteShadowProjector(AActor *thing, FDynamicLight *light, float alphaScale, NkSpriteShadowProjector &out)
{
	NKReceiverSpriteShadowFrameInfo frame;
	if (!NKGetReceiverSpriteShadowFrameInfo(thing, light, frame))
	{
		return false;
	}

	DVector3 originMap, rightVecMap, upVecMap;
	if (!NKBuildReceiverSpriteShadowV9CasterBasis(thing, frame, originMap, rightVecMap, upVecMap))
	{
		return false;
	}
	DVector3 lightMap = light->PosRelative(thing->Sector != nullptr ? thing->Sector->PortalGroup : 0);

	// v24: keep the actual v9/v23 bottom anchor, but allow the caster silhouette
	// to read more like a real projected shadow. A point light already creates
	// perspective growth on receivers, but the sprite billboard plane can still
	// feel too perfectly fitted. Expanding the v9 caster quad around its bottom
	// center makes nearby lights feel larger without breaking the anchor.
	{
		DVector3 bottomCenter = originMap - upVecMap;
		const double lightDist = max<double>((lightMap - originMap).Length(), 1.0);
		const double nearDist = max<double>((double)nk_receiver_sprite_shadows_near_scale_distance, 1.0);
		const double nearT = clamp<double>(1.0 - lightDist / nearDist, 0.0, 1.0);
		const double nearBoost = max<double>((double)nk_receiver_sprite_shadows_near_scale, 0.0) * nearT * nearT;
		const double sideScale = max<double>(0.05, 1.0 + (double)nk_receiver_sprite_shadows_side_extend + nearBoost);
		const double topScale = max<double>(0.05, 1.0 + (double)nk_receiver_sprite_shadows_top_extend + nearBoost);
		rightVecMap *= sideScale;
		upVecMap *= topScale;
		originMap = bottomCenter + upVecMap;
	}

	memset(&out, 0, sizeof(out));
	// Shader world coordinates are (map X, height Z, map Y).
	out.lightX = (float)lightMap.X;
	out.lightY = (float)lightMap.Z;
	out.lightZ = (float)lightMap.Y;
	// radius.w is used as a small parameter channel in this experimental path.
	// Negative values still encode nk_receiver_sprite_shadows_debug_mode; normal
	// mode stores top-edge fade width for the shader.
	const int debugMode = clamp<int>(nk_receiver_sprite_shadows_debug_mode, 0, 3);
	out.radius = debugMode > 0 ? -(float)debugMode : clamp<float>(nk_receiver_sprite_shadows_top_fade, 0.0f, 0.45f);

	out.originX = (float)originMap.X;
	out.originY = (float)originMap.Z;
	out.originZ = (float)originMap.Y;
	out.strength = clamp<float>(nk_receiver_sprite_shadows_strength * alphaScale, 0.0f, 1.0f);

	out.rightX = (float)rightVecMap.X;
	out.rightY = (float)rightVecMap.Z;
	out.rightZ = (float)rightVecMap.Y;
	out.softness = clamp<float>(nk_receiver_sprite_shadows_mask_softness, 0.0f, 1.0f);

	out.upX = (float)upVecMap.X;
	out.upY = (float)upVecMap.Z;
	out.upZ = (float)upVecMap.Y;
	out.maxLength = max<float>(nk_receiver_sprite_shadows_max_length, 1.f);

	// v25: side edge fade in UV space. This is shader-side only: the v23/v24
	// caster anchor remains unchanged, and no slice/line/blocker approximation is used.
	out.sideFade = clamp<float>(nk_receiver_sprite_shadows_side_fade, 0.0f, 0.45f);
	// v26: fadeParams.y/z are used by main.fp for contact-hardening softness.
	// y = additional far softness, z = distance at which that extra softness is fully applied.
	out.reserved0 = clamp<float>(nk_receiver_sprite_shadows_contact_softness, 0.0f, 1.0f);
	out.reserved1 = max<float>(nk_receiver_sprite_shadows_contact_distance, 1.0f);
	out.reserved2 = clamp<float>(nk_receiver_sprite_shadows_distance_opacity, 0.0f, 1.0f);
	// v28: surface grazing-angle fade parameters. The shader uses abs(N dot L)
	// for robustness with GZDoom wall/floor normal orientation.
	out.angleFade = clamp<float>(nk_receiver_sprite_shadows_surface_angle_fade, 0.0f, 1.0f);
	out.angleMinDot = clamp<float>(nk_receiver_sprite_shadows_surface_min_dot, 0.0f, 0.95f);
	out.anglePower = max<float>(nk_receiver_sprite_shadows_surface_power, 0.05f);
	out.angleReserved = nk_receiver_sprite_shadows_fast_path ? 1.0f : 0.0f;

	NKBuildReceiverSpriteShadowMask(frame, out.mask);
	return true;
}


static void NKReceiverSpriteShadowTryAddLight(AActor *thing, FDynamicLight *light, int portalGroup,
	const DVector3 &actorCenter, double maxDistanceSq, double minRadius,
	TArray<NKReceiverSpriteShadowCandidate> &candidates, int maxProjectors,
	TArray<FDynamicLight *> &addedLights)
{
	if (thing == nullptr || light == nullptr || !light->IsActive() || light->IsSubtractive())
	{
		return;
	}
	if (light->DontLightMap() && !nk_receiver_sprite_shadows_allow_dontlightmap)
	{
		return;
	}
	if (std::find(addedLights.begin(), addedLights.end(), light) != addedLights.end())
	{
		return;
	}

	const double radius = light->GetRadius();
	if (radius < minRadius)
	{
		return;
	}

	DVector3 lightPos = light->PosRelative(portalGroup);
	if (lightPos.Z <= thing->floorz + 1.0)
	{
		return;
	}

	const double dx = lightPos.X - actorCenter.X;
	const double dy = lightPos.Y - actorCenter.Y;
	const double dz = lightPos.Z - actorCenter.Z;
	const double distSq = dx * dx + dy * dy + dz * dz;
	if (distSq > maxDistanceSq)
	{
		return;
	}

	// v30: A distant normal light with a large map link range should not cast
	// sprite receiver shadows unless its own light radius plausibly reaches the
	// caster. This prevents far decorative/map lights from spending projector
	// budget or creating unwanted shadows.
	if (nk_receiver_sprite_shadows_strict_light_range)
	{
		const double radiusScale = max<double>((double)nk_receiver_sprite_shadows_light_radius_scale, 0.01);
		const double effectiveRadius = max<double>(radius * radiusScale, minRadius);
		if (distSq > effectiveRadius * effectiveRadius)
		{
			return;
		}
	}

	const double maxDistance = sqrt(maxDistanceSq);
	const float score = NKReceiverSpriteShadowScore(light, lightPos, actorCenter, maxDistance);
	if (score <= 0.f || score < clamp<float>(nk_receiver_sprite_shadows_min_score, 0.0f, 1.0f))
	{
		return;
	}

	addedLights.Push(light);

	NKReceiverSpriteShadowCandidate candidate;
	candidate.Thing = thing;
	candidate.Light = light;
	candidate.Score = score;
	candidate.AlphaScale = score;
	NKInsertReceiverSpriteShadowCandidate(candidates, candidate, maxProjectors);
}

static void NKReceiverSpriteShadowCollectLightList(AActor *thing, FLightNode *lighthead, int portalGroup,
	const DVector3 &actorCenter, double maxDistanceSq, double minRadius,
	TArray<NKReceiverSpriteShadowCandidate> &candidates, int maxProjectors,
	TArray<FDynamicLight *> &addedLights)
{
	for (FLightNode *node = lighthead; node != nullptr; node = node->nextLight)
	{
		NKReceiverSpriteShadowTryAddLight(thing, node->lightsource, portalGroup,
			actorCenter, maxDistanceSq, minRadius, candidates, maxProjectors, addedLights);
	}
}

static void NKReceiverSpriteShadowCollectGlobalLights(AActor *thing, int portalGroup,
	const DVector3 &actorCenter, double maxDistanceSq, double minRadius,
	TArray<NKReceiverSpriteShadowCandidate> &candidates, int maxProjectors,
	TArray<FDynamicLight *> &addedLights)
{
	if (thing == nullptr || thing->Level == nullptr)
	{
		return;
	}
	for (FDynamicLight *light = thing->Level->lights; light != nullptr; light = light->next)
	{
		NKReceiverSpriteShadowTryAddLight(thing, light, portalGroup,
			actorCenter, maxDistanceSq, minRadius, candidates, maxProjectors, addedLights);
	}
}

static void NKCollectReceiverSpriteShadowProjectors(FLevelLocals *Level, AActor *camera)
{
	IShadowMap *sm = &screen->mShadowMap;
	sm->ClearNkSpriteShadowProjectors();

	if (!nk_receiver_sprite_shadows || Level == nullptr || camera == nullptr)
	{
		return;
	}

	const int maxProjectors = clamp<int>(nk_receiver_sprite_shadows_max_projectors, 0, 8);
	if (maxProjectors <= 0)
	{
		return;
	}

	const double actorDistance = max<double>(nk_receiver_sprite_shadows_actor_distance, 1.0);
	const double actorDistanceSq = actorDistance * actorDistance;
	const double lightDistance = max<double>(nk_receiver_sprite_shadows_light_distance, 1.0);
	const double lightDistanceSq = lightDistance * lightDistance;
	const double minRadius = max<double>(nk_receiver_sprite_shadows_min_light_radius, 0.0);

	TArray<NKReceiverSpriteShadowCandidate> candidates;
	auto it = Level->GetThinkerIterator<AActor>();
	while (AActor *thing = it.Next())
	{
		if (!NKCanCastReceiverSpriteShadow(thing))
		{
			continue;
		}
		const double camDx = thing->X() - camera->X();
		const double camDy = thing->Y() - camera->Y();
		const double camDz = thing->Z() - camera->Z();
		if (camDx * camDx + camDy * camDy + camDz * camDz > actorDistanceSq)
		{
			continue;
		}

		DVector3 actorCenter(thing->X(), thing->Y(), thing->Center());
		const int portalGroup = thing->Sector != nullptr ? thing->Sector->PortalGroup : 0;
		TArray<FDynamicLight *> addedLights;
		addedLights.Clear();

		// v13: Receiver-shader projectors must use the same dynamic-light sources that
		// normal sprite lighting can see. Some actor-attached/player-attached lights do
		// not show up in Level->lights for the shadowmap stat path, but they are linked
		// through render sections. Scan both paths.
		if (nk_receiver_sprite_shadows_scan_all_lights)
		{
			NKReceiverSpriteShadowCollectGlobalLights(thing, portalGroup,
				actorCenter, lightDistanceSq, minRadius, candidates, maxProjectors, addedLights);
		}

		if (nk_receiver_sprite_shadows_scan_section_lights)
		{
			if (thing->section != nullptr)
			{
				NKReceiverSpriteShadowCollectLightList(thing, thing->section->lighthead, portalGroup,
					actorCenter, lightDistanceSq, minRadius, candidates, maxProjectors, addedLights);
			}
			if (thing->subsector != nullptr && thing->subsector->section != nullptr && thing->subsector->section != thing->section)
			{
				NKReceiverSpriteShadowCollectLightList(thing, thing->subsector->section->lighthead, portalGroup,
					actorCenter, lightDistanceSq, minRadius, candidates, maxProjectors, addedLights);
			}

			// Broad fallback for moving lights linked to adjacent sections. This mirrors the
			// path that made the floor-projection prototype react to player-following lights.
			dl_validcount++;
			// v30: BSPWalkCircle expects a radius, not radius squared. Passing
			// lightDistanceSq made the section-light fallback scan a huge area.
			BSPWalkCircle(Level, (float)actorCenter.X, (float)actorCenter.Y, (float)lightDistance, [&](subsector_t *subsector)
			{
				if (subsector == nullptr || subsector->section == nullptr)
				{
					return;
				}
				auto section = subsector->section;
				if (section->validcount == dl_validcount)
				{
					return;
				}
				section->validcount = dl_validcount;

				NKReceiverSpriteShadowCollectLightList(thing, section->lighthead, portalGroup,
					actorCenter, lightDistanceSq, minRadius, candidates, maxProjectors, addedLights);
			});
		}
	}

	for (unsigned int i = 0; i < candidates.Size(); ++i)
	{
		float alphaScale = candidates[i].AlphaScale;
		if (i > 0)
		{
			alphaScale *= clamp<float>(nk_receiver_sprite_shadows_second_light_scale, 0.0f, 1.0f);
		}

		NkSpriteShadowProjector projector;
		if (NKBuildReceiverSpriteShadowProjector(candidates[i].Thing, candidates[i].Light, alphaScale, projector))
		{
			sm->AddNkSpriteShadowProjector(projector);
		}
	}
}


void CollectLights(FLevelLocals* Level, AActor *camera)
{
	IShadowMap* sm = &screen->mShadowMap;
	int lightindex = 0;

	NKCollectReceiverSpriteShadowProjectors(Level, camera);

	// Todo: this should go through the blockmap in a spiral pattern around the player so that closer lights are preferred.
	for (auto light = Level->lights; light; light = light->next)
	{
		IShadowMap::LightsProcessed++;
		if (light->shadowmapped && light->IsActive() && lightindex < 1024)
		{
			IShadowMap::LightsShadowmapped++;

			light->mShadowmapIndex = lightindex;
			sm->SetLight(lightindex, (float)light->X(), (float)light->Y(), (float)light->Z(), light->GetRadius());
			lightindex++;
		}
		else
		{
			light->mShadowmapIndex = 1024;
		}

	}

	for (; lightindex < 1024; lightindex++)
	{
		sm->SetLight(lightindex, 0, 0, 0, 0);
	}
}


//-----------------------------------------------------------------------------
//
// Renders one viewpoint in a scene
//
//-----------------------------------------------------------------------------

sector_t* RenderViewpoint(FRenderViewpoint& mainvp, AActor* camera, IntRect* bounds, float fov, float ratio, float fovratio, bool mainview, bool toscreen)
{
	auto& RenderState = *screen->RenderState();

	R_SetupFrame(mainvp, r_viewwindow, camera);

	// [Nakara] v12: Receiver-shader sprite shadows use the shadowmap upload path only
	// as a per-frame SSBO transport for projector data. They must update even when
	// the ordinary GZDoom light shadowmap is disabled or when no dynamic light is
	// marked as shadowmapped. This is especially important for player-attached
	// normal dynamic lights that move every frame.
	const bool shadowStorageCapable = mainview && toscreen
		&& !(camera->Level->flags3 & LEVEL3_NOSHADOWMAP)
		&& camera->Level->aabbTree != nullptr
		&& screen->allowSSBO()
		&& (screen->hwcaps & RFL_SHADER_STORAGE_BUFFER);

	const bool wantRegularShadowMap = shadowStorageCapable
		&& camera->Level->HasDynamicLights
		&& gl_light_shadowmap;

	const bool wantNkReceiverSpriteShadows = shadowStorageCapable
		&& nk_receiver_sprite_shadows;

	if (wantRegularShadowMap || wantNkReceiverSpriteShadows)
	{
		screen->SetAABBTree(camera->Level->aabbTree);
		screen->mShadowMap.SetCollectLights([=] {
			CollectLights(camera->Level, camera);
		});
		screen->UpdateShadowMap();
	}
	else
	{
		// [Nakara] If receiver projectors were enabled earlier, the GL SSBO can still
		// contain the previous frame's projectors. Upload a zero-strength dummy before
		// disabling the shadowmap path so nk_receiver_sprite_shadows 0 removes the effect
		// immediately instead of leaving stale receiver shadows on world materials.
		if (mainview && toscreen && screen->mShadowMap.mNkSpriteShadowProjectorsBuffer != nullptr)
		{
			screen->mShadowMap.DisableNkSpriteShadowProjectors();
		}

		// null all references to the level if we do not need a shadowmap/projector upload.
		// This will shortcut all internal calculations without further checks.
		screen->SetAABBTree(nullptr);
		screen->mShadowMap.SetCollectLights(nullptr);
	}

	screen->SetLevelMesh(camera->Level->levelMesh);

	// Update the attenuation flag of all light defaults for each viewpoint.
	// This function will only do something if the setting differs.
	FLightDefaults::SetAttenuationForLevel(!!(camera->Level->flags3 & LEVEL3_ATTENUATE));

	// Render (potentially) multiple views for stereo 3d
	// Fixme. The view offsetting should be done with a static table and not require setup of the entire render state for the mode.
	auto vrmode = VRMode::GetVRMode(mainview && toscreen);
	const int eyeCount = vrmode->mEyeCount;
	screen->FirstEye();
	for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
	{
		const auto& eye = vrmode->mEyes[eye_ix];
		screen->SetViewportRects(bounds);

		if (mainview) // Bind the scene frame buffer and turn on draw buffers used by ssao
		{
			bool useSSAO = (gl_ssao != 0);
			bool useFocusMask = nk_focus_mask_enable;
			bool useCloak = nk_cloak_enable;
			bool useGBuffer = useSSAO || useFocusMask || useCloak;
			screen->SetSceneRenderTarget(useGBuffer);
			RenderState.SetPassType(useGBuffer ? GBUFFER_PASS : NORMAL_PASS);
			RenderState.EnableDrawBuffers(RenderState.GetPassDrawBufferCount(), true);
		}

		auto di = HWDrawInfo::StartDrawInfo(mainvp.ViewLevel, nullptr, mainvp, nullptr);
		auto& vp = di->Viewpoint;

		di->Set3DViewport(RenderState);
		di->SetViewArea();
		auto cm = di->SetFullbrightFlags(mainview ? vp.camera->player : nullptr);
		float flash = 1.f;

		// Only used by the GLES2 renderer
		RenderState.SetSpecialColormap(cm, flash);

		di->Viewpoint.FieldOfView = DAngle::fromDeg(fov);	// Set the real FOV for the current scene (it's not necessarily the same as the global setting in r_viewpoint)

		// Stereo mode specific perspective projection
		float inv_iso_dist = 1.0f;
		bool iso_ortho = (camera->ViewPos != NULL) && (camera->ViewPos->Flags & VPSF_ORTHOGRAPHIC);
		if (iso_ortho && (camera->ViewPos->Offset.Length() > 0)) inv_iso_dist = 1.0/camera->ViewPos->Offset.Length();
		di->VPUniforms.mProjectionMatrix = eye.GetProjection(fov, ratio, fovratio * inv_iso_dist, iso_ortho);
		di->ProjectionMatrix2 = eye.GetProjection(fov, ratio, fovratio, false); // Regular ol' perspective projection matrix

		// Stereo mode specific viewpoint adjustment
		vp.Pos += eye.GetViewShift(vp.HWAngles.Yaw.Degrees());
		di->SetupView(RenderState, vp.Pos.X, vp.Pos.Y, vp.Pos.Z, false, false);

		// [Nakara] Store the exact main-view matrices used for the rendered scene so
		// postprocess motion blur can reconstruct per-pixel camera velocity from depth.
		// Only update this for the actual screen view, not camera textures.
		if (mainview && toscreen)
		{
			hw_postprocess.SetMainViewMatrices(di->VPUniforms.mViewMatrix, di->VPUniforms.mProjectionMatrix);

			// [Nakara] DOF view-trace autofocus. This uses gameplay/world collision to decide
			// the focus distance, instead of trusting one unstable center depth-buffer texel.
			bool dofTraceValid = false;
			float dofTraceDistance = 0.0f;
			if (nk_dof && nk_dof_auto_focus && nk_dof_focus_source == 2 && vp.sector != nullptr)
			{
				DVector3 traceDir = vp.ViewVector3D;
				double dirLen = traceDir.Length();
				if (dirLen > 0.0001)
				{
					traceDir = traceDir.Unit();
					FTraceResults traceResults;
					AActor *ignoreActor = vp.ViewActor != nullptr ? vp.ViewActor : vp.camera;
					ActorFlags actorMask = nk_dof_trace_hit_actors ? (MF_SOLID | MF_SHOOTABLE) : ActorFlags::FromInt(0);
					double maxTraceDistance = max<double>((double)nk_dof_trace_max_distance, 64.0);
					if (Trace(vp.Pos, vp.sector, traceDir, maxTraceDistance, actorMask, ML_BLOCKEVERYTHING, ignoreActor, traceResults, TRACE_NoSky))
					{
						dofTraceValid = true;
						dofTraceDistance = (float)max<double>(traceResults.Distance, 1.0);
					}
				}
			}
			hw_postprocess.SetDepthOfFieldTraceFocus(dofTraceValid, dofTraceDistance);
		}

		// [Nakara] The sprite path flips this back to true only if a cloak mask
		// actually needs compositing for this eye/view.
		nk_cloak_rendered_this_frame = false;
		di->ProcessScene(toscreen);

		if (mainview)
		{
			PostProcess.Clock();
			if (toscreen) di->EndDrawScene(mainvp.sector, RenderState); // do not call this for camera textures.

			if (RenderState.GetPassType() == GBUFFER_PASS) // Turn off ssao draw buffers
			{
				RenderState.SetPassType(NORMAL_PASS);
				RenderState.EnableDrawBuffers(1);
			}

			screen->PostProcessScene(false, cm, flash, [&]() { di->DrawEndScene2D(mainvp.sector, RenderState); });
			PostProcess.Unclock();
		}
		// Reset colormap so 2D drawing isn't affected
		RenderState.SetSpecialColormap(CM_DEFAULT, 1);

		di->EndDrawInfo();
		if (eyeCount - eye_ix > 1)
			screen->NextEye(eyeCount);
	}

	return mainvp.sector;
}

void DoWriteSavePic(FileWriter* file, ESSType ssformat, uint8_t* scr, int width, int height, sector_t* viewsector, bool upsidedown)
{
	PalEntry palette[256];
	PalEntry modulateColor;
	auto blend = V_CalcBlend(viewsector, &modulateColor);
	int pixelsize = 1;
	// Apply the screen blend, because the renderer does not provide this.
	if (ssformat == SS_RGB)
	{
		int numbytes = width * height * 3;
		pixelsize = 3;
		if (modulateColor != 0xffffffff)
		{
			float r = modulateColor.r / 255.f;
			float g = modulateColor.g / 255.f;
			float b = modulateColor.b / 255.f;
			for (int i = 0; i < numbytes; i += 3)
			{
				scr[i] = uint8_t(scr[i] * r);
				scr[i + 1] = uint8_t(scr[i + 1] * g);
				scr[i + 2] = uint8_t(scr[i + 2] * b);
			}
		}
		float iblendfac = 1.f - blend.W;
		blend.X *= blend.W;
		blend.Y *= blend.W;
		blend.Z *= blend.W;
		for (int i = 0; i < numbytes; i += 3)
		{
			scr[i] = uint8_t(scr[i] * iblendfac + blend.X);
			scr[i + 1] = uint8_t(scr[i + 1] * iblendfac + blend.Y);
			scr[i + 2] = uint8_t(scr[i + 2] * iblendfac + blend.Z);
		}
	}
	else
	{
		// Apply the screen blend to the palette. The colormap related parts get skipped here because these are already part of the image.
		DoBlending(GPalette.BaseColors, palette, 256, uint8_t(blend.X), uint8_t(blend.Y), uint8_t(blend.Z), uint8_t(blend.W * 255));
	}

	int pitch = width * pixelsize;
	if (upsidedown)
	{
		scr += ((height - 1) * width * pixelsize);
		pitch *= -1;
	}

	M_CreatePNG(file, scr, ssformat == SS_PAL ? palette : nullptr, ssformat, width, height, pitch, vid_gamma);
}

//===========================================================================
//
// Render the view to a savegame picture
//
//===========================================================================

void WriteSavePic(player_t* player, FileWriter* file, int width, int height)
{
	if (!V_IsHardwareRenderer())
	{
		SWRenderer->WriteSavePic(player, file, width, height);
	}
	else
	{
		IntRect bounds;
		bounds.left = 0;
		bounds.top = 0;
		bounds.width = width;
		bounds.height = height;
		auto& RenderState = *screen->RenderState();

		// we must be sure the GPU finished reading from the buffer before we fill it with new data.
		screen->WaitForCommands(false);

		// Switch to render buffers dimensioned for the savepic
		screen->SetSaveBuffers(true);
		screen->ImageTransitionScene(true);

		hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);
		hw_ClearFakeFlat();
		screen->mVertexData->Reset();
		RenderState.SetVertexBuffer(screen->mVertexData);
		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// This shouldn't overwrite the global viewpoint even for a short time.
		FRenderViewpoint savevp;
		sector_t* viewsector = RenderViewpoint(savevp, players[consoleplayer].camera, &bounds, r_viewpoint.FieldOfView.Degrees(), 1.6f, 1.6f, true, false);
		RenderState.EnableStencil(false);
		RenderState.SetNoSoftLightLevel();

		TArray<uint8_t> scr(width * height * 3, true);
		screen->CopyScreenToBuffer(width, height, scr.Data());

		DoWriteSavePic(file, SS_RGB, scr.Data(), width, height, viewsector, screen->FlipSavePic());

		// Switch back the screen render buffers
		screen->SetViewportRects(nullptr);
		screen->SetSaveBuffers(false);
	}
}

//===========================================================================
//
// Renders the main view
//
//===========================================================================

static void CheckTimer(FRenderState &state, uint64_t ShaderStartTime)
{
	// if firstFrame is not yet initialized, initialize it to current time
	// if we're going to overflow a float (after ~4.6 hours, or 24 bits), re-init to regain precision
	if ((state.firstFrame == 0) || (screen->FrameTime - state.firstFrame >= 1 << 24) || ShaderStartTime >= state.firstFrame)
		state.firstFrame = screen->FrameTime - 1;
}


sector_t* RenderView(player_t* player)
{
	auto RenderState = screen->RenderState();
	RenderState->SetVertexBuffer(screen->mVertexData);
	screen->mVertexData->Reset();
	hw_postprocess.SetTonemapMode(level.info ? level.info->tonemap : ETonemapMode::None);

	sector_t* retsec;
	if (!V_IsHardwareRenderer())
	{
		screen->SetActiveRenderTarget();	// only relevant for Vulkan

		if (!swdrawer) swdrawer = new SWSceneDrawer;
		retsec = swdrawer->RenderView(player);
	}
	else
	{
		hw_ClearFakeFlat();

		iter_dlightf = iter_dlight = draw_dlight = draw_dlightf = 0;

		checkBenchActive();

		// reset statistics counters
		ResetProfilingData();

		// Get this before everything else
		if (cl_capfps || r_NoInterpolate) r_viewpoint.TicFrac = 1.;
		else r_viewpoint.TicFrac = I_GetTimeFrac();

		screen->mLights->Clear();
		screen->mBones->Clear();
		screen->mViewpoints->Clear();

		// NoInterpolateView should have no bearing on camera textures, but needs to be preserved for the main view below.
		bool saved_niv = NoInterpolateView;
		NoInterpolateView = false;

		// Shader start time does not need to be handled per level. Just use the one from the camera to render from.
		if (player->camera)
			CheckTimer(*RenderState, player->camera->Level->ShaderStartTime);

		// Draw all canvases that changed
		for (FCanvas* canvas : AllCanvases)
		{
			if (canvas->Tex && canvas->Tex->CheckNeedsUpdate())
			{
				screen->RenderTextureView(canvas->Tex, [=](IntRect& bounds)
					{
						screen->SetViewportRects(&bounds);
						Draw2D(&canvas->Drawer, *screen->RenderState(), 0, 0, canvas->Tex->GetWidth(), canvas->Tex->GetHeight());
						canvas->Drawer.Clear();
					});
				canvas->Tex->SetUpdated(true);
			}
		}

		// prepare all camera textures that have been used in the last frame.
		// This must be done for all levels, not just the primary one!
		for (auto Level : AllLevels())
		{
			Level->canvasTextureInfo.UpdateAll([&](AActor* camera, FCanvasTexture* camtex, double fov)
				{
					screen->RenderTextureView(camtex, [=](IntRect& bounds)
						{
							FRenderViewpoint texvp;
							float ratio = camtex->aspectRatio / Level->info->pixelstretch;
							RenderViewpoint(texvp, camera, &bounds, fov, ratio, ratio, false, false);
						});
				});
		}
		NoInterpolateView = saved_niv;

		// now render the main view
		float fovratio;
		float ratio = r_viewwindow.WidescreenRatio;
		if (r_viewwindow.WidescreenRatio >= 1.3f)
		{
			fovratio = 1.333333f;
		}
		else
		{
			fovratio = ratio;
		}

		screen->ImageTransitionScene(true); // Only relevant for Vulkan.

		retsec = RenderViewpoint(r_viewpoint, player->camera, NULL, r_viewpoint.FieldOfView.Degrees(), ratio, fovratio, true, true);
	}
	All.Unclock();
	return retsec;
}

