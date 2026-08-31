layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SharpTexture;
layout(binding=1) uniform sampler2D BlurTexture;
#if defined(MULTISAMPLE)
layout(binding=2) uniform sampler2DMS DepthTexture;
#else
layout(binding=2) uniform sampler2D DepthTexture;
#endif
layout(binding=3) uniform sampler2D FocusTexture;

float LinearizeDepth(float depth)
{
	return 1.0 / (depth * LinearizeDepthA + LinearizeDepthB);
}

float GetLuma(vec3 color)
{
	return max(max(color.r, color.g), color.b);
}

ivec2 GetDepthTextureSize()
{
#if defined(MULTISAMPLE)
	return textureSize(DepthTexture);
#else
	return textureSize(DepthTexture, 0);
#endif
}

float SampleRawDepth(vec2 uv)
{
#if defined(MULTISAMPLE)
	ivec2 texSize = GetDepthTextureSize();
	ivec2 ipos = ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(texSize));
	ipos = clamp(ipos, ivec2(0), texSize - ivec2(1));
	return texelFetch(DepthTexture, ipos, 0).x;
#else
	return texture(DepthTexture, clamp(uv, vec2(0.0), vec2(1.0))).x;
#endif
}

float SampleLinearDepth(vec2 uv)
{
	return LinearizeDepth(SampleRawDepth(uv));
}

void AddStableDepthSample(vec2 uv, float centerDepth, inout float depthSum, inout float weightSum, float weight)
{
	float rawDepth = SampleRawDepth(uv);
	if (rawDepth >= 0.9995)
	{
		return;
	}

	float sampleDepth = LinearizeDepth(rawDepth);
	float acceptRange = max(DepthSoftenTolerance, max(abs(centerDepth) * 0.06, 32.0));
	if (abs(sampleDepth - centerDepth) <= acceptRange)
	{
		depthSum += sampleDepth * weight;
		weightSum += weight;
	}
}

float SampleStableLinearDepth(vec2 uv, float rawDepth)
{
	float centerDepth = LinearizeDepth(rawDepth);
	if (DepthSoften < 0.5 || DepthSoftenRadius <= 0.001 || rawDepth >= 0.9995)
	{
		return centerDepth;
	}

	vec2 texel = 1.0 / vec2(GetDepthTextureSize());
	vec2 r = texel * DepthSoftenRadius;
	float depthSum = centerDepth;
	float weightSum = 1.0;

	// Lightweight, independently written depth prefilter. This is not QeffectsGL code;
	// it simply reduces thin depth-edge flicker before the DOF amount is computed.
	AddStableDepthSample(uv + vec2( r.x, 0.0), centerDepth, depthSum, weightSum, 0.65);
	AddStableDepthSample(uv + vec2(-r.x, 0.0), centerDepth, depthSum, weightSum, 0.65);
	AddStableDepthSample(uv + vec2(0.0,  r.y), centerDepth, depthSum, weightSum, 0.65);
	AddStableDepthSample(uv + vec2(0.0, -r.y), centerDepth, depthSum, weightSum, 0.65);
	AddStableDepthSample(uv + vec2( r.x,  r.y), centerDepth, depthSum, weightSum, 0.35);
	AddStableDepthSample(uv + vec2(-r.x,  r.y), centerDepth, depthSum, weightSum, 0.35);
	AddStableDepthSample(uv + vec2( r.x, -r.y), centerDepth, depthSum, weightSum, 0.35);
	AddStableDepthSample(uv + vec2(-r.x, -r.y), centerDepth, depthSum, weightSum, 0.35);

	return depthSum / max(weightSum, 0.0001);
}

float AddFocusSample(vec2 uv, float centerDepth, inout float depthSum, inout float weightSum, float weight)
{
	float sampleDepth = SampleLinearDepth(uv);
	float rejectRange = max(abs(centerDepth) * 0.08, 96.0);

	// Do not let nearby pillars, sky holes, sprites with missing depth, or thin seams
	// pull the focus away from the exact crosshair depth. Pulling the focus this way
	// is what makes the player's gaze point blur even though autofocus is enabled.
	if (abs(sampleDepth - centerDepth) <= rejectRange)
	{
		depthSum += sampleDepth * weight;
		weightSum += weight;
	}
	return sampleDepth;
}

float SampleAutoFocusDepth(vec2 centerUV)
{
	float centerDepth = SampleLinearDepth(centerUV);
	float depthSum = centerDepth;
	float weightSum = 1.0;

	vec2 r = vec2(AutoFocusSampleRadius) * Scale;

	if (AutoFocusSampleRadius > 0.00001)
	{
		AddFocusSample(centerUV + vec2( r.x, 0.0), centerDepth, depthSum, weightSum, 0.20);
		AddFocusSample(centerUV + vec2(-r.x, 0.0), centerDepth, depthSum, weightSum, 0.20);
		AddFocusSample(centerUV + vec2(0.0,  r.y), centerDepth, depthSum, weightSum, 0.20);
		AddFocusSample(centerUV + vec2(0.0, -r.y), centerDepth, depthSum, weightSum, 0.20);
	}

	return depthSum / max(weightSum, 0.0001);
}

vec3 SampleBokehBlur(vec3 baseBlur, float dofAmount)
{
	if (BokehEnable < 0.5 || BokehRadius <= 0.0 || BokehMix <= 0.0 || dofAmount <= 0.001)
	{
		return baseBlur;
	}

	vec2 texelSize = 1.0 / vec2(textureSize(SharpTexture, 0));
	vec2 radius = texelSize * BokehRadius * clamp(dofAmount, 0.0, 1.0);

	vec3 colorSum = baseBlur;
	float weightSum = 1.0;

	// Two normalized aperture rings. Unlike the old highlight boost, this never
	// adds a copied scene color on top of the result; every tap contributes to a
	// weighted average and is divided by weightSum before the final mix.
	vec2 offsets[18] = vec2[18](
		vec2( 1.000,  0.000),
		vec2( 0.500,  0.866),
		vec2(-0.500,  0.866),
		vec2(-1.000,  0.000),
		vec2(-0.500, -0.866),
		vec2( 0.500, -0.866),
		vec2( 0.000,  1.000),
		vec2( 0.866,  0.500),
		vec2( 0.866, -0.500),
		vec2( 0.000, -1.000),
		vec2(-0.866, -0.500),
		vec2(-0.866,  0.500),
		vec2( 0.500,  0.000),
		vec2( 0.250,  0.433),
		vec2(-0.250,  0.433),
		vec2(-0.500,  0.000),
		vec2(-0.250, -0.433),
		vec2( 0.250, -0.433)
	);

	for (int i = 0; i < 18; i++)
	{
		float ringWeight = (i < 12) ? 1.0 : 0.75;
		vec2 sampleUV = clamp(TexCoord + offsets[i] * radius, vec2(0.0), vec2(1.0));
		colorSum += texture(SharpTexture, sampleUV).rgb * ringWeight;
		weightSum += ringWeight;
	}

	vec3 bokehBlur = colorSum / max(weightSum, 0.0001);
	return mix(baseBlur, bokehBlur, clamp(BokehMix, 0.0, 1.0));
}

void main()
{
	vec2 sceneUV = Offset + TexCoord * Scale;

	float rawDepth = SampleRawDepth(sceneUV);
	float linearDepth = SampleStableLinearDepth(sceneUV, rawDepth);

	// Auto focus samples the depth at the player's gaze/crosshair point and keeps that distance sharp.
	float focusDistance = FocusDistance;

	if (AutoFocus > 0.5)
	{
		if (FocusSource > 1.5)
		{
			// View-trace autofocus: C++ already measured and smoothed the distance.
			focusDistance = clamp(FocusDistance, min(AutoFocusMin, AutoFocusMax), max(AutoFocusMin, AutoFocusMax));
		}
		else if (UseSmoothedFocus > 0.5)
		{
			focusDistance = texelFetch(FocusTexture, ivec2(0, 0), 0).x;
		}
		else
		{
			vec2 centerUV = Offset + vec2(0.5, 0.5) * Scale;
			float centerDepth = SampleAutoFocusDepth(centerUV);
			focusDistance = clamp(centerDepth, min(AutoFocusMin, AutoFocusMax), max(AutoFocusMin, AutoFocusMax));
		}
	}

	float focusWidth = max(FocusRange, 1.0);
	if (RelativeFocus > 0.5)
	{
		// Large maps can easily exceed 10000 units. A fixed FocusRange makes those maps
		// hit full blur too quickly, so optionally scale the clear zone by focus distance.
		focusWidth = max(abs(focusDistance) * max(RelativeRange, 0.01), 1.0);
	}

	// Fail-safe for very close autofocus hits. Looking down at a nearby floor can make
	// the relative clear zone tiny, which turns almost the entire scene into full blur.
	focusWidth = max(focusWidth, max(MinFocusWidth, 1.0));
	if (FocusWidthMax > 1.0)
	{
		focusWidth = min(focusWidth, max(FocusWidthMax, MinFocusWidth));
	}

	// Separate foreground and background blur. In long corridors, the center focus can be
	// very far away while side walls/floor are much closer. Full near blur makes those
	// nearby surfaces smear heavily, so near blur gets its own weaker strength and wider range.
	float signedDistance = linearDepth - focusDistance;
	float nearDistance = max(-signedDistance, 0.0);
	float farDistance = max(signedDistance, 0.0);

	float nearWidth = max(focusWidth * max(NearRangeMul, 0.05), 1.0);
	float farWidth = max(focusWidth * max(FarRangeMul, 0.05), 1.0);

	float nearAmount = clamp(nearDistance / nearWidth, 0.0, 1.0);
	float farAmount = clamp(farDistance / farWidth, 0.0, 1.0);

	nearAmount = pow(nearAmount, max(Curve, 0.05));
	farAmount = pow(farAmount, max(Curve, 0.05));
	nearAmount = smoothstep(0.0, 1.0, nearAmount);
	farAmount = smoothstep(0.0, 1.0, farAmount);

	float effectiveNearStrength = NearStrength;
	if (AutoFocus > 0.5 && AutoFocusNear <= 0.5)
	{
		effectiveNearStrength = 0.0;
	}

	float dofAmount = max(nearAmount * effectiveNearStrength, farAmount * FarStrength);

	// v13 soft mix mode: keep the focus band truly sharp, then let blur fade in slowly.
	// This avoids the overly cloudy look that happens when the pre-blurred texture is mixed
	// too early around the focus distance.
	if (SoftMix > 0.5)
	{
		float deadzone = clamp(FocusDeadzone, 0.0, 0.95);
		dofAmount = max(dofAmount - deadzone, 0.0) / max(1.0 - deadzone, 0.001);
		dofAmount = pow(clamp(dofAmount, 0.0, 1.0), max(BlurGamma, 0.05));
	}

	dofAmount = clamp(dofAmount * Strength, 0.0, MaxAmount);

	// Extremely far depths are often sky, fog backdrop, water surface/ceiling, or other
	// large bright color fields. Letting those fully participate in DoF can wash the
	// image into a strong cyan/blue blur in underwater maps. This optional fade keeps
	// those distant background pixels closer to the original scene color.
	if (FarFadeEnd > FarFadeStart && FarFadeStart > 0.0)
	{
		float farFade = 1.0 - smoothstep(FarFadeStart, FarFadeEnd, linearDepth);
		dofAmount *= farFade;
	}

	// Sky/no-depth guard. Pixels that are effectively at the far plane are usually sky,
	// horizon, fog/backdrop, or other areas without useful scene depth. Blurring them
	// can turn the top half of the image into a soft cloudy sheet, especially with
	// normalized bokeh enabled. Keep those pixels mostly sharp.
	float noDepthFade = 1.0 - smoothstep(0.9990, 1.0, rawDepth);
	dofAmount *= noDepthFade;

	// Gameplay-friendly center protection. This keeps the crosshair/gaze area sharp
	// and lets blur fade in toward the screen edges, which also hides most
	// foreground-sprite depth problems near the player's aim point.
	if (CenterMask > 0.5)
	{
		vec2 centerDelta = TexCoord - vec2(0.5, 0.5);
		centerDelta.x *= max(Scale.x / max(Scale.y, 0.0001), 0.0001);
		float centerDist = length(centerDelta);
		float centerAmount = smoothstep(CenterRadius, CenterRadius + CenterFeather, centerDist);
		dofAmount *= centerAmount;
	}

	// SharpTexture is a copied scene-sized texture, so it uses TexCoord.
	// BlurTexture is the blurred/downscaled copy of the same scene area.
	vec4 sharpColor = texture(SharpTexture, TexCoord);
	vec4 blurColor = texture(BlurTexture, TexCoord);

	// Many gameplay sprites/translucent actors do not reliably write to scene depth.
	// If the sharp scene alpha is not fully opaque, keep it sharp to avoid blurred background
	// appearing through foreground sprites.
	if (AlphaProtect > 0.5)
	{
		dofAmount *= smoothstep(0.98, 1.0, sharpColor.a);
	}

	vec3 finalBlur = SampleBokehBlur(blurColor.rgb, dofAmount);

	if (DebugMode > 0.5)
	{
		FragColor = vec4(dofAmount, dofAmount, dofAmount, 1.0);
	}
	else
	{
		FragColor = vec4(mix(sharpColor.rgb, finalBlur, dofAmount), 1.0);
	}
}
