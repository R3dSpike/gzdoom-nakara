#pragma once

#include "c_cvars.h"

class IFXAAShader
{
public:
	enum Quality
	{
		None,
		Low,
		Medium,
		High,
		Extreme,
		Count
	};
};



//==========================================================================
//
// CVARs
//
//==========================================================================
EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Float, gl_bloom_amount)
EXTERN_CVAR(Float, gl_exposure_scale)
EXTERN_CVAR(Float, gl_exposure_min)
EXTERN_CVAR(Float, gl_exposure_base)
EXTERN_CVAR(Float, gl_exposure_speed)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, gl_bloom_kernel_size)
EXTERN_CVAR(Bool, gl_lens)
EXTERN_CVAR(Float, gl_lens_k)
EXTERN_CVAR(Float, gl_lens_kcube)
EXTERN_CVAR(Float, gl_lens_chromatic)
EXTERN_CVAR(Int, gl_fxaa)
EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Int, gl_ssao_portals)
EXTERN_CVAR(Float, gl_ssao_strength)
EXTERN_CVAR(Int, gl_ssao_debug)
EXTERN_CVAR(Float, gl_ssao_bias)
EXTERN_CVAR(Float, gl_ssao_radius)
EXTERN_CVAR(Float, gl_ssao_blur)
EXTERN_CVAR(Float, gl_ssao_exponent)
EXTERN_CVAR(Float, gl_paltonemap_powtable)
EXTERN_CVAR(Bool, gl_paltonemap_reverselookup)
EXTERN_CVAR(Float, gl_menu_blur)
EXTERN_CVAR(Bool, nk_aspect_correction)
EXTERN_CVAR(Float, nk_aspect_correction_scale)
EXTERN_CVAR(Float, nk_aspect_correction_smoothing)
EXTERN_CVAR(Bool, nk_ambient_light)
EXTERN_CVAR(Float, nk_ambient_light_strength)
EXTERN_CVAR(Float, nk_ambient_light_mix)
EXTERN_CVAR(Float, nk_ambient_light_darken)
EXTERN_CVAR(Float, nk_ambient_light_saturation)
EXTERN_CVAR(Float, nk_ambient_light_blur_amount)
EXTERN_CVAR(Int, nk_ambient_light_levels)
EXTERN_CVAR(Bool, nk_ambient_light_sky_guard)
EXTERN_CVAR(Bool, nk_motion_blur)
EXTERN_CVAR(Float, nk_motion_blur_strength)
EXTERN_CVAR(Int, nk_motion_blur_samples)
EXTERN_CVAR(Float, nk_motion_blur_velocity_scale)
EXTERN_CVAR(Float, nk_motion_blur_min_velocity)
EXTERN_CVAR(Float, nk_motion_blur_max_radius)
EXTERN_CVAR(Float, nk_motion_blur_depth_cutoff)
EXTERN_CVAR(Float, nk_motion_blur_center_fade)
EXTERN_CVAR(Float, nk_motion_blur_teleport_reset_distance)
EXTERN_CVAR(Bool, nk_dof)
EXTERN_CVAR(Float, nk_dof_focus_distance)
EXTERN_CVAR(Float, nk_dof_focus_range)
EXTERN_CVAR(Float, nk_dof_strength)
EXTERN_CVAR(Bool, nk_dof_auto_focus)
EXTERN_CVAR(Float, nk_dof_auto_focus_min)
EXTERN_CVAR(Float, nk_dof_auto_focus_max)
EXTERN_CVAR(Bool, nk_dof_auto_focus_near_blur)
EXTERN_CVAR(Float, nk_dof_near_strength)
EXTERN_CVAR(Float, nk_dof_far_strength)
EXTERN_CVAR(Float, nk_dof_near_range_mul)
EXTERN_CVAR(Float, nk_dof_far_range_mul)
EXTERN_CVAR(Bool, nk_dof_relative_focus)
EXTERN_CVAR(Float, nk_dof_relative_range)
EXTERN_CVAR(Float, nk_dof_curve)
EXTERN_CVAR(Float, nk_dof_auto_focus_sample_radius)
EXTERN_CVAR(Int, nk_dof_focus_source)
EXTERN_CVAR(Float, nk_dof_trace_max_distance)
EXTERN_CVAR(Float, nk_dof_trace_nohit_distance)
EXTERN_CVAR(Bool, nk_dof_trace_hit_actors)
EXTERN_CVAR(Bool, nk_dof_depth_soften)
EXTERN_CVAR(Float, nk_dof_depth_soften_radius)
EXTERN_CVAR(Float, nk_dof_depth_soften_tolerance)
EXTERN_CVAR(Float, nk_dof_focus_smoothing)
EXTERN_CVAR(Float, nk_dof_min_focus_width)
EXTERN_CVAR(Float, nk_dof_focus_width_max)
EXTERN_CVAR(Float, nk_dof_far_fade_start)
EXTERN_CVAR(Float, nk_dof_far_fade_end)
EXTERN_CVAR(Float, nk_dof_max_amount)
EXTERN_CVAR(Bool, nk_dof_soft_mix)
EXTERN_CVAR(Float, nk_dof_focus_deadzone)
EXTERN_CVAR(Float, nk_dof_blur_gamma)
EXTERN_CVAR(Float, nk_dof_blur_amount)
EXTERN_CVAR(Int, nk_dof_levels)
EXTERN_CVAR(Bool, nk_dof_center_mask)
EXTERN_CVAR(Float, nk_dof_center_radius)
EXTERN_CVAR(Float, nk_dof_center_feather)
EXTERN_CVAR(Bool, nk_dof_bokeh)
EXTERN_CVAR(Float, nk_dof_bokeh_radius)
EXTERN_CVAR(Float, nk_dof_bokeh_mix)
EXTERN_CVAR(Bool, nk_dof_alpha_protect)
EXTERN_CVAR(Bool, nk_dof_debug)
EXTERN_CVAR(Float, vid_brightness)
EXTERN_CVAR(Float, vid_contrast)
EXTERN_CVAR(Float, vid_saturation)
EXTERN_CVAR(Int, gl_satformula)
EXTERN_CVAR(Bool, nk_focus_mask_enable)
EXTERN_CVAR(Bool, nk_cloak_enable)
EXTERN_CVAR(Float, nk_cloak_distortion)
EXTERN_CVAR(Float, nk_cloak_shimmer)
EXTERN_CVAR(Float, nk_cloak_edge_refraction)
EXTERN_CVAR(Float, nk_cloak_darkening)
extern bool nk_cloak_rendered_this_frame;
extern float nk_focus_tint_strength_runtime;

