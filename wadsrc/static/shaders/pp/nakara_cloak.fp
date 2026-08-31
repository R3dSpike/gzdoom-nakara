// Nakara StarCraft-style refractive cloak postprocess.
// InputTexture : current scene color after +CLOAK actors were rendered normally.
// CloakTexture : R = cloak amount, where 0 = normal and 1 = fully cloaked.
//
// Actor.Alpha remains the visible amount. The postprocess does not fade the actor
// a second time; it only increases screen-space refraction as cloak amount rises.

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;
layout(binding=1) uniform sampler2D CloakTexture;

float CloakMask(vec2 uv)
{
    return clamp(texture(CloakTexture, clamp(uv, vec2(0.0), vec2(1.0))).r, 0.0, 1.0);
}

void main()
{
    vec4 base = texture(InputTexture, TexCoord);
    float mask = CloakMask(TexCoord);

    if (mask <= 0.0001)
    {
        FragColor = base;
        return;
    }

    vec2 texel = 1.0 / vec2(textureSize(InputTexture, 0));

    // Sample both a 1-pixel and a 2-pixel mask gradient. V2 only used the
    // immediate gradient, which made the refractive contour too thin to read
    // clearly on flat/cel-shaded sprites. The wider gradient creates a small
    // screen-space edge band without bringing the actor's original color back.
    float ml = CloakMask(TexCoord - vec2(texel.x, 0.0));
    float mr = CloakMask(TexCoord + vec2(texel.x, 0.0));
    float md = CloakMask(TexCoord - vec2(0.0, texel.y));
    float mu = CloakMask(TexCoord + vec2(0.0, texel.y));
    vec2 gradientNear = vec2(mr - ml, mu - md);

    vec2 texel2 = texel * 2.0;
    float ml2 = CloakMask(TexCoord - vec2(texel2.x, 0.0));
    float mr2 = CloakMask(TexCoord + vec2(texel2.x, 0.0));
    float md2 = CloakMask(TexCoord - vec2(0.0, texel2.y));
    float mu2 = CloakMask(TexCoord + vec2(0.0, texel2.y));
    vec2 gradientWide = vec2(mr2 - ml2, mu2 - md2);

    // Build an inward feather from the same 1px/2px samples used by the edge
    // detector. Dividing by the center mask makes the feather describe the
    // silhouette shape rather than squaring the Actor.Alpha/cloak transition.
    // At the boundary this approaches 0; two or more pixels inside it reaches 1.
    float centerSafe = max(mask, 0.0001);
    float innerNear = min(min(ml, mr), min(md, mu));
    float innerWide = min(min(ml2, mr2), min(md2, mu2));
    float nearCoverage = clamp(innerNear / centerSafe, 0.0, 1.0);
    float wideCoverage = clamp(innerWide / centerSafe, 0.0, 1.0);
    float darkFeather = smoothstep(0.0, 1.0, nearCoverage * 0.38 + wideCoverage * 0.62);

    vec2 edgeGradient = gradientNear + gradientWide * 0.72;
    float edgeStrength = smoothstep(0.035, 0.72, length(edgeGradient)) * mask;

    // Two low-amplitude waves break up the perfectly static silhouette. Their
    // frequencies are intentionally incommensurate to avoid an obvious grid.
    float waveA = sin(TexCoord.y * 173.0 + Time * 2.10 + sin(TexCoord.x * 41.0));
    float waveB = cos(TexCoord.x * 127.0 - Time * 1.73 + sin(TexCoord.y * 53.0));
    vec2 shimmer = vec2(waveA, waveB) * (0.35 * Shimmer);

    // Keep the interior distortion close to V2, but bend the scene noticeably
    // harder in the edge band. EdgeRefraction is independent from Shimmer so
    // the silhouette remains readable even with shimmer turned down.
    float edgeBoost = 2.8 * EdgeRefraction * edgeStrength;
    vec2 direction = edgeGradient * (2.4 + edgeBoost) + shimmer;
    vec2 offset = direction * texel * (2.25 * Distortion) * mask;
    vec2 refractUV = clamp(TexCoord + offset, vec2(0.0), vec2(1.0));
    vec4 refracted = texture(InputTexture, refractUV);

    // Use the refracted sample anywhere the cloak mask exists. Because offset
    // tends to zero with mask, this preserves Actor.Alpha as the visual fade
    // control instead of accidentally applying (1-Alpha) twice.
    float active = smoothstep(0.0001, 0.015, mask);
    vec3 color = mix(base.rgb, refracted.rgb, active);

    // V4: subtly darken the refracted *background*, never the actor texture.
    // The inward feather avoids a hard dark cutout and is especially useful for
    // flat/cel-shaded sprites whose shape can be difficult to read from
    // refraction alone. Darkening scales linearly with cloak amount.
    float darkAmount = clamp(Darkening, 0.0, 1.0) * mask * darkFeather;
    color *= 1.0 - darkAmount;

    // Very small silhouette glint, strongest at the cloak boundary. This keeps
    // the effect readable without turning it into a bright sci-fi shield.
    float edge = edgeStrength;
    color += vec3(0.018) * edge * Shimmer;

    FragColor = vec4(color, base.a);
}
