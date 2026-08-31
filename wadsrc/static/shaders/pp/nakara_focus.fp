// Nakara focus grayscale postprocess.
// InputTexture      : current scene/postprocess color
// FocusMaskTexture  : engine-generated actor mask. 1 = keep full color, 0 = grayscale.

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D FocusMaskTexture;

void main()
{
    vec4 c = texture(SceneTexture, TexCoord);
    float m0 = texture(FocusMaskTexture, TexCoord).r;

    float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float clampedGray = max(gray, 0.04);
    vec3 effectColor = vec3(clampedGray * GrayScale);

    float w = Strength * (1.0 - m0);
    vec3 base = mix(c.rgb, effectColor, clamp(w, 0.0, 1.0));

    vec2 texel = fwidth(TexCoord);
    vec2 o = OutlinePixels * texel;

    float md = 0.0;
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( o.x, 0.0)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(-o.x, 0.0)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(0.0,  o.y)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(0.0, -o.y)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( o.x,  o.y)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(-o.x,  o.y)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( o.x, -o.y)).r);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(-o.x, -o.y)).r);

    float outlineMask = clamp(md - m0, 0.0, 1.0);
    vec3 outlineColor = vec3(0.18, 0.80, 1.0);

    vec3 finalColor = clamp(base + outlineColor * outlineMask * OutlineStrength * Strength, 0.0, 1.0);
    FragColor = vec4(finalColor, c.a);
}
