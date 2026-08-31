// postprocess fragment
// Stable focus mask + dedicated per-actor outline info version.
// Inputs:
//   FocusMaskTexture.a = focus mask from SceneNormal.a
//   FocusInfoTexture.rgb = outline color
//   FocusInfoTexture.a = outline thickness normalized to 0..1, where 1 = MaxOutlinePx

const int MaxOutlinePx = 8;

float SampleFocusMask(vec2 uv)
{
    return clamp(texture(FocusMaskTexture, uv).a, 0.0, 1.0);
}

vec4 SampleFocusInfo(vec2 uv)
{
    return texture(FocusInfoTexture, uv);
}

vec3 ResolveFocusOutline(vec2 uv, float selfMask, out float outlineMask)
{
    vec2 texel = 1.0 / vec2(textureSize(InputTexture, 0));

    vec3 bestColor = vec3(0.0);
    float bestMask = 0.0;

    for (int i = 1; i <= MaxOutlinePx; i++)
    {
        float d = float(i);

        vec2 offsets[8] = vec2[8](
            vec2( d,  0.0),
            vec2(-d,  0.0),
            vec2(0.0,  d ),
            vec2(0.0, -d ),
            vec2( d,  d ),
            vec2(-d,  d ),
            vec2( d, -d ),
            vec2(-d, -d )
        );

        for (int j = 0; j < 8; j++)
        {
            vec2 sampleUV = uv + offsets[j] * texel;

            float mask = SampleFocusMask(sampleUV);
            vec4 info = SampleFocusInfo(sampleUV);
            float thicknessPx = info.a * float(MaxOutlinePx);

            if (mask > 0.01 && thicknessPx >= d)
            {
                float edgeMask = mask * (1.0 - selfMask);

                if (edgeMask > bestMask)
                {
                    bestMask = edgeMask;
                    bestColor = info.rgb;
                }
            }
        }
    }

    outlineMask = clamp(bestMask, 0.0, 1.0);
    return bestColor;
}

void main()
{
    vec4 c = texture(InputTexture, TexCoord);

    float gray = dot(c.rgb, vec3(0.299, 0.587, 0.114));
    float clampedGray = max(gray, 0.04);
    vec3 effectColor = vec3(clampedGray * 0.75);

    float m0 = SampleFocusMask(TexCoord);

    float w = Strength * (1.0 - m0);
    vec3 base = mix(c.rgb, effectColor, clamp(w, 0.0, 1.0));

    float outlineMask;
    vec3 outlineColor = ResolveFocusOutline(TexCoord, m0, outlineMask);

    vec3 finalColor = clamp(
        base + outlineColor * outlineMask * Strength,
        0.0,
        1.0
    );

    FragColor = vec4(finalColor, c.a);
}
