// postprocess fragment
// Focus mask + per-actor outline data version:
// - Whole screen is desaturated by Strength
// - Pixels marked in FocusMaskTexture.a are preserved
// - Outline color is sampled from FocusOutlineTexture.rgb
// - Outline thickness is sampled from FocusOutlineThicknessTexture.a

const float MaxFocusOutlinePx = 8.0;

vec4 SampleFocusInfo(vec2 uv)
{
    float mask = clamp(texture(FocusMaskTexture, uv).a, 0.0, 1.0);
    vec3 outlineColor = texture(FocusOutlineTexture, uv).rgb;
    float thickness = clamp(texture(FocusOutlineThicknessTexture, uv).a, 0.0, 1.0) * MaxFocusOutlinePx;

    return vec4(outlineColor, mask * thickness);
}

vec3 ResolveFocusOutline(vec2 uv, out float outlineMask)
{
    vec2 texel = 1.0 / vec2(textureSize(InputTexture, 0));

    vec3 bestColor = vec3(0.0);
    float bestMask = 0.0;

    for (int i = 1; i <= 8; i++)
    {
        float d = float(i);

        vec2 o0 = vec2( d,  0.0) * texel;
        vec2 o1 = vec2(-d,  0.0) * texel;
        vec2 o2 = vec2(0.0,  d ) * texel;
        vec2 o3 = vec2(0.0, -d ) * texel;
        vec2 o4 = vec2( d,  d ) * texel;
        vec2 o5 = vec2(-d,  d ) * texel;
        vec2 o6 = vec2( d, -d ) * texel;
        vec2 o7 = vec2(-d, -d ) * texel;

        vec4 s0 = SampleFocusInfo(uv + o0);
        vec4 s1 = SampleFocusInfo(uv + o1);
        vec4 s2 = SampleFocusInfo(uv + o2);
        vec4 s3 = SampleFocusInfo(uv + o3);
        vec4 s4 = SampleFocusInfo(uv + o4);
        vec4 s5 = SampleFocusInfo(uv + o5);
        vec4 s6 = SampleFocusInfo(uv + o6);
        vec4 s7 = SampleFocusInfo(uv + o7);

        if (s0.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s0.rgb; }
        if (s1.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s1.rgb; }
        if (s2.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s2.rgb; }
        if (s3.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s3.rgb; }
        if (s4.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s4.rgb; }
        if (s5.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s5.rgb; }
        if (s6.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s6.rgb; }
        if (s7.a >= d && bestMask < 1.0) { bestMask = 1.0; bestColor = s7.rgb; }
    }

    outlineMask = bestMask;
    return bestColor;
}

void main()
{
    vec4 c = texture(InputTexture, TexCoord);

    float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float clampedGray = max(gray, 0.04);
    vec3 effectColor = vec3(clampedGray * 0.75);

    float m0 = texture(FocusMaskTexture, TexCoord).a;
    m0 = clamp(m0, 0.0, 1.0);

    float w = Strength * (1.0 - m0);
    vec3 base = mix(c.rgb, effectColor, clamp(w, 0.0, 1.0));

    float outlineMask;
    vec3 outlineColor = ResolveFocusOutline(TexCoord, outlineMask);
    outlineMask = clamp(outlineMask - m0, 0.0, 1.0);

    vec3 finalColor = clamp(base + outlineColor * outlineMask * Strength, 0.0, 1.0);

    FragColor = vec4(finalColor, c.a);
}
