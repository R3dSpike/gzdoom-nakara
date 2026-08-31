// postprocess fragment
// Stable FocusMask version.
// - Whole screen is desaturated by Strength.
// - Pixels marked in FocusMaskTexture.a are preserved.
// - Uses only FocusMaskTexture. Per-actor outline data is intentionally disabled here.

void main()
{
    vec4 c = texture(InputTexture, TexCoord);

    float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float clampedGray = max(gray, 0.04);
    vec3 effectColor = vec3(clampedGray * 0.75);

    float m0 = texture(FocusMaskTexture, TexCoord).a;
    m0 = clamp(m0, 0.0, 1.0);

    float w = Strength * (1.0 - m0);
    vec3 finalColor = mix(c.rgb, effectColor, clamp(w, 0.0, 1.0));

    FragColor = vec4(finalColor, c.a);
}
