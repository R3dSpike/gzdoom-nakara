// Nakara focus mask postprocess sample
// Requires GLDEFS: texture FocusMaskTexture "$SceneNormal"

void main()
{
    vec4 c = texture(InputTexture, TexCoord);

    float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float clampedGray = max(gray, 0.04);
    vec3 effectColor = vec3(clampedGray * GrayScale);

    // Focus mask is stored in SceneNormal.a by the engine patch.
    float m0 = texture(FocusMaskTexture, TexCoord).a;

    float w = Strength * (1.0 - m0);
    vec3 base = mix(c.rgb, effectColor, clamp(w, 0.0, 1.0));

    float outlinePx = OutlinePixels;
    vec2 texel = fwidth(TexCoord);
    vec2 o = outlinePx * texel;

    float md = 0.0;
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( o.x, 0   )).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(-o.x, 0   )).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( 0  ,  o.y)).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( 0  , -o.y)).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( o.x,  o.y)).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(-o.x,  o.y)).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2( o.x, -o.y)).a);
    md = max(md, texture(FocusMaskTexture, TexCoord + vec2(-o.x, -o.y)).a);

    float outlineMask = clamp(md - m0, 0.0, 1.0);

    vec3 outlineColor = vec3(0.18, 0.80, 1.0);
    vec3 finalColor = clamp(base + outlineColor * outlineMask * OutlineStrength * Strength, 0.0, 1.0);

    FragColor = vec4(finalColor, c.a);
}
