layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SceneTexture;

void main()
{
	vec3 color = max(texture(SceneTexture, TexCoord).rgb, vec3(0.0));

	// ReShade AmbientLight-like source preparation:
	// reduce dark pixels softly without using a hard bloom threshold.
	vec3 source = pow(color, vec3(max(Darken, 0.0001)));
	FragColor = vec4(source, 0.0);
}
