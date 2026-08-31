layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SceneTexture;

void main()
{
	vec4 sceneSample = texture(SceneTexture, TexCoord);
	vec3 scene = sceneSample.rgb;

	int sampleCount = int(clamp(floor(Samples + 0.5), 1.0, 16.0));
	float amount = clamp(Strength * MotionAmount, 0.0, 1.0);
	vec2 motion = MotionVector;

	// A small center-out fallback for pure forward/back movement. It is intentionally
	// weak because position-only screen-space blur has no true per-pixel velocity.
	vec2 centerDir = TexCoord - vec2(0.5, 0.5);
	motion += centerDir * clamp(PositionAmount * 0.25, 0.0, 0.01);

	float motionLen = length(motion);
	if (amount <= 0.0001 || motionLen <= 0.000001 || sampleCount <= 1)
	{
		FragColor = sceneSample;
		return;
	}

	vec3 colorSum = vec3(0.0);
	float weightSum = 0.0;

	for (int i = 0; i < 16; i++)
	{
		if (i >= sampleCount)
		{
			break;
		}

		float denom = max(float(sampleCount - 1), 1.0);
		float t = (float(i) / denom) - 0.5;

		// Slightly favor the center sample so the image does not become smeared too easily.
		float weight = 1.0 - abs(t) * 0.55;
		vec2 uv = clamp(TexCoord + motion * t, vec2(0.0), vec2(1.0));
		colorSum += texture(SceneTexture, uv).rgb * weight;
		weightSum += weight;
	}

	vec3 blurred = colorSum / max(weightSum, 0.0001);
	vec3 result = mix(scene, blurred, amount);
	FragColor = vec4(result, sceneSample.a);
}
