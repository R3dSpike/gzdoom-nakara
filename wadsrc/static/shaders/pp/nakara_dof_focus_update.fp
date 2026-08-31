layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D PreviousFocusTexture;
#if defined(MULTISAMPLE)
layout(binding=1) uniform sampler2DMS DepthTexture;
#else
layout(binding=1) uniform sampler2D DepthTexture;
#endif

float LinearizeDepth(float depth)
{
	return 1.0 / (depth * LinearizeDepthA + LinearizeDepthB);
}

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

float SampleLinearDepth(vec2 uv)
{
	return LinearizeDepth(SampleRawDepth(uv));
}

void AddFocusSample(vec2 uv, float centerDepth, inout float depthSum, inout float weightSum, float weight)
{
	float sampleDepth = SampleLinearDepth(uv);
	float rejectRange = max(abs(centerDepth) * 0.08, 96.0);

	// Keep the QeffectsGL idea of sampling the crosshair depth, but reject small seams
	// and nearby accidental hits so focus does not jump wildly near thin geometry.
	if (abs(sampleDepth - centerDepth) <= rejectRange)
	{
		depthSum += sampleDepth * weight;
		weightSum += weight;
	}
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

void main()
{
	vec2 centerUV = Offset + vec2(0.5, 0.5) * Scale;
	float targetFocus = SampleAutoFocusDepth(centerUV);
	targetFocus = clamp(targetFocus, min(AutoFocusMin, AutoFocusMax), max(AutoFocusMin, AutoFocusMax));

	float smoothing = clamp(FocusSmoothing, 0.0, 0.99);
	float smoothedFocus = targetFocus;

	if (HasHistory > 0.5)
	{
		float previousFocus = texelFetch(PreviousFocusTexture, ivec2(0, 0), 0).x;
		if (previousFocus > 0.0)
		{
			smoothedFocus = mix(targetFocus, previousFocus, smoothing);
		}
	}

	FragColor = vec4(smoothedFocus, 0.0, 0.0, 1.0);
}
