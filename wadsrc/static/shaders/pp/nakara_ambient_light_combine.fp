layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D GlowTexture;
#if defined(MULTISAMPLE)
layout(binding=2) uniform sampler2DMS DepthTexture;
#else
layout(binding=2) uniform sampler2D DepthTexture;
#endif

float SampleRawDepth(vec2 uv)
{
#if defined(MULTISAMPLE)
	ivec2 texSize = textureSize(DepthTexture);
	ivec2 ipos = ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(texSize));
	ipos = clamp(ipos, ivec2(0), texSize - ivec2(1));
	return texelFetch(DepthTexture, ipos, 0).x;
#else
	return texture(DepthTexture, clamp(uv, vec2(0.0), vec2(1.0))).x;
#endif
}

vec3 ApplySaturation(vec3 color, float saturation)
{
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	return mix(vec3(luma), color, saturation);
}

void main()
{
	vec4 sceneSample = texture(SceneTexture, TexCoord);
	vec3 scene = sceneSample.rgb;

	vec3 glow = max(texture(GlowTexture, TexCoord).rgb, vec3(0.0));
	glow = ApplySaturation(glow, Saturation);

	// Screen-style blend. This gives a soft ambient lift without the harsh additive
	// look of traditional bloom: scene + glow is intentionally avoided here.
	vec3 screenGlow = 1.0 - (1.0 - scene) * (1.0 - clamp(glow * Strength, 0.0, 1.0));
	vec3 result = mix(scene, screenGlow, clamp(Mix, 0.0, 1.0));

	// Depth values near 1.0 are usually sky/no-depth/far fog. Keep them from smearing
	// a bright color over large parts of the image.
	if (SkyGuard > 0.5)
	{
		vec2 depthUV = Offset + TexCoord * Scale;
		float rawDepth = SampleRawDepth(depthUV);
		float skyFactor = smoothstep(0.9992, 1.0, rawDepth);
		result = mix(result, scene, skyFactor * 0.75);
	}

	FragColor = vec4(result, sceneSample.a);
}
