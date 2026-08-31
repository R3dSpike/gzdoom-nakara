layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;

void main()
{
	vec2 dimensions = vec2(textureSize(InputTexture, 0));
	vec2 pos = TexCoord - vec2(0.5);

	// 1.2 reproduces Achthon's horizontal 6/5 stretch:
	// pos.x *= 5.0 / 6.0 in source sampling space.
	pos.x /= max(HorizontalScale, 0.0001);
	pos += vec2(0.5);

	ivec2 nearestPos = clamp(ivec2(pos * dimensions), ivec2(0), ivec2(dimensions) - ivec2(1));
	vec4 nearestColor = texelFetch(InputTexture, nearestPos, 0);
	vec4 linearColor = texture(InputTexture, pos);

	FragColor = mix(nearestColor, linearColor, clamp(Smoothing, 0.0, 1.0));
}
