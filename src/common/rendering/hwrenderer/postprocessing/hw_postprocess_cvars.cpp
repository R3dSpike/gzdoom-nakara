/*
**  Postprocessing framework
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
*/

#include "hw_postprocess_cvars.h"
#include "v_video.h"

//==========================================================================
//
// CVARs
//
//==========================================================================
CVAR(Bool, gl_bloom, false, CVAR_ARCHIVE);
CUSTOM_CVAR(Float, gl_bloom_amount, 1.4f, CVAR_ARCHIVE)
{
	if (self < 0.1f) self = 0.1f;
}

CVAR(Float, gl_exposure_scale, 1.3f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_min, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_base, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_speed, 0.05f, CVAR_ARCHIVE)

CUSTOM_CVAR(Int, gl_tonemap, 0, CVAR_ARCHIVE)
{
	if (self < 0 || self > 5)
		self = 0;
}

CVAR(Bool, gl_lens, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, gl_lens_k, -0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_kcube, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_chromatic, 1.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, gl_fxaa, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self >= IFXAAShader::Count)
	{
		self = 0;
	}
}

CUSTOM_CVAR(Int, gl_ssao, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self > 3)
		self = 0;
}

CUSTOM_CVAR(Int, gl_ssao_portals, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
		self = 0;
}

CVAR(Float, gl_ssao_strength, 0.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_ssao_debug, 0, 0)
CVAR(Float, gl_ssao_bias, 0.2f, 0)
CVAR(Float, gl_ssao_radius, 80.0f, 0)
CUSTOM_CVAR(Float, gl_ssao_blur, 16.0f, 0)
{
	if (self < 0.1f) self = 0.1f;
}

CUSTOM_CVAR(Float, gl_ssao_exponent, 1.8f, 0)
{
	if (self < 0.1f) self = 0.1f;
}

CUSTOM_CVAR(Float, gl_paltonemap_powtable, 2.0f, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	screen->UpdatePalette();
}

CUSTOM_CVAR(Bool, gl_paltonemap_reverselookup, true, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	screen->UpdatePalette();
}

CVAR(Float, gl_menu_blur, -1.0f, CVAR_ARCHIVE)

// [Nakara] Built-in aspect-only replacement for Achthon.
// 1.2 matches Achthon's horizontal 6/5 stretch.
CVAR(Bool, nk_aspect_correction, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nk_aspect_correction_scale, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f) self = 1.0f;
	if (self > 2.0f) self = 2.0f;
}
CUSTOM_CVAR(Float, nk_aspect_correction_smoothing, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}

// [Nakara] Lightweight ReShade-style Ambient Light.
// This is intentionally separate from gl_bloom and uses screen-style mixing instead of additive bloom.
CVAR(Bool, nk_ambient_light, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nk_ambient_light_strength, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 2.0f) self = 2.0f;
}
CUSTOM_CVAR(Float, nk_ambient_light_mix, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_ambient_light_darken, 2.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f) self = 0.25f;
	if (self > 8.0f) self = 8.0f;
}
CUSTOM_CVAR(Float, nk_ambient_light_saturation, 1.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 3.0f) self = 3.0f;
}
CUSTOM_CVAR(Float, nk_ambient_light_blur_amount, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.1f) self = 0.1f;
	if (self > 8.0f) self = 8.0f;
}
CUSTOM_CVAR(Int, nk_ambient_light_levels, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	if (self > 4) self = 4;
}
CUSTOM_CVAR(Bool, nk_ambient_light_sky_guard, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}

// [Nakara] Simple depth based far depth-of-field.
// Uses the existing menu blur/bloom blur chain and only changes the final depth-aware combine pass.
// [Nakara] Depth/matrix based camera motion blur. Experimental and off by default.
CVAR(Bool, nk_motion_blur, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nk_motion_blur_strength, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Int, nk_motion_blur_samples, 10, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 2) self = 2;
	if (self > 16) self = 16;
}
CUSTOM_CVAR(Float, nk_motion_blur_velocity_scale, 1.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 8.0f) self = 8.0f;
}
CUSTOM_CVAR(Float, nk_motion_blur_min_velocity, 0.0015f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.1f) self = 0.1f;
}
CUSTOM_CVAR(Float, nk_motion_blur_max_radius, 0.035f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.25f) self = 0.25f;
}
CUSTOM_CVAR(Float, nk_motion_blur_depth_cutoff, 0.9995f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_motion_blur_center_fade, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_motion_blur_teleport_reset_distance, 512.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 8192.0f) self = 8192.0f;
}

CVAR(Bool, nk_dof, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nk_dof_focus_distance, 768.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_dof_focus_range, 384.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_dof_strength, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
}
CUSTOM_CVAR(Bool, nk_dof_auto_focus, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CUSTOM_CVAR(Float, nk_dof_auto_focus_min, 64.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_dof_auto_focus_max, 4096.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Bool, nk_dof_auto_focus_near_blur, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CUSTOM_CVAR(Float, nk_dof_near_strength, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 2.0f) self = 2.0f;
}
CUSTOM_CVAR(Float, nk_dof_far_strength, 0.62f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 2.0f) self = 2.0f;
}
CUSTOM_CVAR(Float, nk_dof_near_range_mul, 3.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 16.0f) self = 16.0f;
}
CUSTOM_CVAR(Float, nk_dof_far_range_mul, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 16.0f) self = 16.0f;
}

CUSTOM_CVAR(Bool, nk_dof_relative_focus, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CUSTOM_CVAR(Float, nk_dof_relative_range, 0.45f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.01f) self = 0.01f;
	if (self > 4.0f) self = 4.0f;
}
CUSTOM_CVAR(Float, nk_dof_curve, 1.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 8.0f) self = 8.0f;
}
CUSTOM_CVAR(Float, nk_dof_auto_focus_sample_radius, 0.003f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.1f) self = 0.1f;
}
CUSTOM_CVAR(Int, nk_dof_focus_source, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0) self = 0;
	if (self > 2) self = 2;
}

CUSTOM_CVAR(Float, nk_dof_trace_max_distance, 4096.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 256.0f) self = 256.0f;
	if (self > 32768.0f) self = 32768.0f;
}

CUSTOM_CVAR(Float, nk_dof_trace_nohit_distance, 2048.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 64.0f) self = 64.0f;
	if (self > 32768.0f) self = 32768.0f;
}

CUSTOM_CVAR(Bool, nk_dof_trace_hit_actors, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}

CUSTOM_CVAR(Bool, nk_dof_depth_soften, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}

CUSTOM_CVAR(Float, nk_dof_depth_soften_radius, 1.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 4.0f) self = 4.0f;
}

CUSTOM_CVAR(Float, nk_dof_depth_soften_tolerance, 128.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 2048.0f) self = 2048.0f;
}

CUSTOM_CVAR(Float, nk_dof_focus_smoothing, 0.99f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	// [Nakara] QeffectsGL-inspired autofocus stabilization.
	// 0.0 = immediate focus changes, 0.99 = very slow focus changes.
	if (self < 0.0f) self = 0.0f;
	if (self > 0.99f) self = 0.99f;
}
CUSTOM_CVAR(Float, nk_dof_min_focus_width, 512.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.0f) self = 1.0f;
	if (self > 65536.0f) self = 65536.0f;
}
CUSTOM_CVAR(Float, nk_dof_max_amount, 0.85f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}

// [Nakara] Soft mix shaping keeps the focus area clearer and avoids the heavy "wet blur" look.
CUSTOM_CVAR(Bool, nk_dof_soft_mix, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CUSTOM_CVAR(Float, nk_dof_focus_deadzone, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.95f) self = 0.95f;
}
CUSTOM_CVAR(Float, nk_dof_blur_gamma, 2.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 8.0f) self = 8.0f;
}

// [Nakara] Optional upper limit for the in-focus distance band.
// This keeps small maps from looking like DoF is disabled when relative focus is enabled.
// 0 means no upper clamp.
CUSTOM_CVAR(Float, nk_dof_focus_width_max, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 65536.0f) self = 65536.0f;
}

// [Nakara] Fade DoF out on extremely far/sky-like depths.
// This prevents bright water/sky/fog colors from being blurred over the entire image.
// 0 disables the far-depth fade.
CUSTOM_CVAR(Float, nk_dof_far_fade_start, 3072.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 65536.0f) self = 65536.0f;
}
CUSTOM_CVAR(Float, nk_dof_far_fade_end, 8192.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 65536.0f) self = 65536.0f;
}

CUSTOM_CVAR(Float, nk_dof_blur_amount, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.1f) self = 0.1f;
}
CUSTOM_CVAR(Int, nk_dof_levels, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	if (self > 4) self = 4;
}
CUSTOM_CVAR(Bool, nk_dof_center_mask, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CUSTOM_CVAR(Float, nk_dof_center_radius, 0.22f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}
CUSTOM_CVAR(Float, nk_dof_center_feather, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.001f) self = 0.001f;
	if (self > 1.0f) self = 1.0f;
}

// [Nakara] Optional normalized aperture blur.
// This is not additive; it replaces part of the gaussian blur with a normalized
// multi-sample bokeh blur, so it should not brighten sectors by itself.
CUSTOM_CVAR(Bool, nk_dof_bokeh, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CUSTOM_CVAR(Float, nk_dof_bokeh_radius, 6.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 64.0f) self = 64.0f;
}
CUSTOM_CVAR(Float, nk_dof_bokeh_mix, 1.00f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 1.0f) self = 1.0f;
}

CUSTOM_CVAR(Bool, nk_dof_alpha_protect, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
}
CVAR(Bool, nk_dof_debug, false, 0)

// [Nakara] Enables writing +FOCUSHIGHLIGHT actors into the scene normal alpha channel.
// This forces the scene GBuffer path so postprocess shaders can sample SceneNormal.a as a mask.
CVAR(Bool, nk_focus_mask_enable, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [Nakara] StarCraft-style cloak. +CLOAK actors use Actor.Alpha as their visible amount:
// 1.0 = normal sprite, 0.0 = only the refractive silhouette remains.
CVAR(Bool, nk_cloak_enable, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, nk_cloak_distortion, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 4.0f) self = 4.0f;
}
CUSTOM_CVAR(Float, nk_cloak_shimmer, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 4.0f) self = 4.0f;
}
// [Nakara] Extra refraction applied only near the cloak silhouette boundary.
// 0 = V2-style edge, 1 = default enhanced edge, up to 4 for testing.
CUSTOM_CVAR(Float, nk_cloak_edge_refraction, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 4.0f) self = 4.0f;
}
// [Nakara] V4: darken only the refracted scene inside the cloak silhouette.
// The shader feathers this darkening inward from the silhouette edge.
CUSTOM_CVAR(Float, nk_cloak_darkening, 0.08f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.5f) self = 0.5f;
}

// Set only by the hardware sprite path. This avoids paying for the fullscreen
// cloak composite on frames where no partially/fully cloaked actor was drawn.
bool nk_cloak_rendered_this_frame = false;

// [Nakara] Runtime focus tint strength used to scale per-actor FocusTintAmount.
// This is intentionally not a CVar because engine-defined CVars cannot be changed
// freely from gameplay ZScript outside menu code. Drive it with PPShader.SetFocusTintStrength().
float nk_focus_tint_strength_runtime = 0.0f;

