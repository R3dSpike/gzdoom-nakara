layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D DepthTexture;

float SampleRawDepth(vec2 uv)
{
	return texture(DepthTexture, clamp(uv, vec2(0.0), vec2(1.0))).x;
}

vec2 PreviousFrameUV(vec2 uv, float rawDepth)
{
	// Reconstruct the current world position from the current screen position and depth.
	// Then project it with the previous frame's view-projection matrix.
	vec4 currentClip = vec4(uv * 2.0 - 1.0, rawDepth * 2.0 - 1.0, 1.0);
	vec4 world = InvCurrentViewProjection * currentClip;
	float worldW = (abs(world.w) < 0.000001) ? 0.000001 : world.w;
	world.xyz /= worldW;
	world.w = 1.0;

	vec4 previousClip = PreviousViewProjection * world;
	float previousW = (abs(previousClip.w) < 0.000001) ? 0.000001 : previousClip.w;
	vec2 previousNdc = previousClip.xy / previousW;
	return previousNdc * 0.5 + 0.5;
}

void main()
{
	vec4 scene = texture(SceneTexture, TexCoord);

	vec2 depthUV = Offset + TexCoord * Scale;
	float rawDepth = SampleRawDepth(depthUV);

	// Do not blur sky/no-depth/far fog pixels. Those are very unstable for reprojection
	// and tend to create a full-screen smear.
	if (rawDepth >= DepthCutoff)
	{
		FragColor = scene;
		return;
	}

	vec2 prevUV = PreviousFrameUV(TexCoord, rawDepth);
	vec2 velocity = (TexCoord - prevUV) * VelocityScale;

	float radius = length(velocity);
	if (radius <= MinVelocity || MaxRadius <= 0.0 || Strength <= 0.0)
	{
		FragColor = scene;
		return;
	}

	if (radius > MaxRadius)
	{
		velocity *= MaxRadius / radius;
		radius = MaxRadius;
	}

	// Optional center protection. This keeps the crosshair/view center clearer and avoids
	// the "everything smears as soon as the mouse moves" feeling.
	float centerFactor = 1.0;
	if (CenterFade > 0.0001)
	{
		centerFactor = smoothstep(0.0, CenterFade, length(TexCoord - vec2(0.5)));
	}

	float blurAmount = clamp((radius - MinVelocity) / max(MaxRadius - MinVelocity, 0.0001), 0.0, 1.0);
	blurAmount *= clamp(Strength, 0.0, 1.0) * centerFactor;

	int sampleCount = int(clamp(Samples, 2.0, 16.0));
	vec3 sum = scene.rgb;
	float weightSum = 1.0;

	for (int i = 1; i < 16; i++)
	{
		if (i >= sampleCount) break;

		float t = float(i) / float(sampleCount - 1);
		vec2 sampleUV = clamp(TexCoord - velocity * t, vec2(0.0), vec2(1.0));
		vec3 sampleColor = texture(SceneTexture, sampleUV).rgb;
		float w = 1.0 - t * 0.35;
		sum += sampleColor * w;
		weightSum += w;
	}

	vec3 blurred = sum / max(weightSum, 0.0001);
	vec3 result = mix(scene.rgb, blurred, blurAmount);
	FragColor = vec4(result, scene.a);
}
