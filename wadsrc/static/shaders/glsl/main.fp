

layout(location = 0) in vec4 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 pixelpos;
layout(location = 3) in vec3 glowdist;
layout(location = 4) in vec3 gradientdist;
layout(location = 5) in vec4 vWorldNormal;
layout(location = 6) in vec4 vEyeNormal;
layout(location = 9) in vec3 vLightmap;

#ifdef NO_CLIPDISTANCE_SUPPORT
layout(location = 7) in vec4 ClipDistanceA;
layout(location = 8) in vec4 ClipDistanceB;
#endif

layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
layout(location=3) out vec4 FragFocusInfo;
layout(location=4) out vec4 FragCloak;
#endif

struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
const int TEXF_Brightmap = 0x10000;
const int TEXF_Detailmap = 0x20000;
const int TEXF_Glowmap = 0x40000;
const int TEXF_ClampY = 0x80000;

//===========================================================================
//
// RGB to HSV
//
//===========================================================================

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

//===========================================================================
//
// Color to grayscale
//
//===========================================================================

float grayscale(vec4 color)
{
	return dot(color.rgb, vec3(0.3, 0.56, 0.14));
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 dodesaturate(vec4 texel, float factor)
{
	if (factor != 0.0)
	{
		float gray = grayscale(texel);
		return mix (texel, vec4(gray,gray,gray,texel.a), factor);
	}
	else
	{
		return texel;
	}
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 desaturate(vec4 texel)
{
	return dodesaturate(texel, uDesaturationFactor);
}

//===========================================================================
//
// Texture tinting code originally from JFDuke but with a few more options
//
//===========================================================================

const int Tex_Blend_Alpha = 1;
const int Tex_Blend_Screen = 2;
const int Tex_Blend_Overlay = 3;
const int Tex_Blend_Hardlight = 4;

 vec4 ApplyTextureManipulation(vec4 texel, int blendflags)
 {
	// Step 1: desaturate according to the material's desaturation factor. 
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested
	if ((blendflags & 8) != 0)
	{
		texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
	if ((blendflags & 7) != 0)
	{
		vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
		vec4 tint = uTextureBlendColor * 255.0;

		switch (blendflags & 7)
		{
			default:
				tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
				tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
				tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;
				break;
			// The following 3 are taken 1:1 from the Build engine
			case Tex_Blend_Screen:
				tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
				tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
				tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);
				break;
			case Tex_Blend_Overlay:
				tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
			case Tex_Blend_Hardlight:
				tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
		}
		texel.rgb = tcol / 255.0;
	}
	return texel;
}

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	vec4 texel = texture(tex, st);

	//
	// Apply texture modes
	//
	switch (uTextureMode & 0xffff)
	{
		case 1:	// TM_STENCIL
			texel.rgb = vec3(1.0,1.0,1.0);
			break;

		case 2:	// TM_OPAQUE
			texel.a = 1.0;
			break;

		case 3:	// TM_INVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);
			break;

		case 4:	// TM_ALPHATEXTURE
		{
			float gray = grayscale(texel);
			texel = vec4(1.0, 1.0, 1.0, gray*texel.a);
			break;
		}

		case 5:	// TM_CLAMPY
			if (st.t < 0.0 || st.t > 1.0)
			{
				texel.a = 0.0;
			}
			break;

		case 6: // TM_OPAQUEINVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);
			break;

		case 7: //TM_FOGLAYER 
			return texel;

	}

	if ((uTextureMode & TEXF_ClampY) != 0)
	{
		if (st.t < 0.0 || st.t > 1.0)
		{
			texel.a = 0.0;
		}
	}

	// Apply the texture modification colors.
	int blendflags = int(uTextureAddColor.a);	// this alpha is unused otherwise
	if (blendflags != 0)	
	{
		// only apply the texture manipulation if it contains something.
		texel = ApplyTextureManipulation(texel, blendflags);
	}

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.
	texel.rgb += uAddColor.rgb;
	if (uObjectColor2.a == 0.0) texel *= uObjectColor;
	else texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);

	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}

//===========================================================================
//
// Vanilla Doom wall colormap equation
//
//===========================================================================
float R_WallColormap(float lightnum, float z, vec3 normal)
{
	// R_ScaleFromGlobalAngle calculation
	float projection = 160.0; // projection depends on SCREENBLOCKS!! 160 is the fullscreen value
	vec2 line_v1 = pixelpos.xz; // in vanilla this is the first curline vertex
	vec2 line_normal = normal.xz;
	float texscale = projection * clamp(dot(normalize(uCameraPos.xz - line_v1), line_normal), 0.0, 1.0) / z;

	float lightz = clamp(16.0 * texscale, 0.0, 47.0);

	// scalelight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	return startmap - lightz * 0.5;
}

//===========================================================================
//
// Vanilla Doom plane colormap equation
//
//===========================================================================
float R_PlaneColormap(float lightnum, float z)
{
	float lightz = clamp(z / 16.0f, 0.0, 127.0);

	// zlight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}

//===========================================================================
//
// zdoom colormap equation
//
//===========================================================================
float R_ZDoomColormap(float light, float z)
{
	float L = light * 255.0;
	float vis = min(uGlobVis / z, 24.0 / 32.0);
	float shade = 2.0 - (L + 12.0) / 128.0;
	float lightscale = shade - vis;
	return lightscale * 31.0;
}

float R_DoomColormap(float light, float z)
{
	if ((uPalLightLevels >> 16) == 16) // gl_lightmode 16
	{
		float lightnum = clamp(light * 15.0, 0.0, 15.0);

		if (dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
		{
			vec3 normal = normalize(vWorldNormal.xyz);
			return mix(R_WallColormap(lightnum, z, normal), R_PlaneColormap(lightnum, z), abs(normal.y));
		}
		else // vWorldNormal is not set on sprites
		{
			return R_PlaneColormap(lightnum, z);
		}
	}
	else
	{
		return R_ZDoomColormap(light, z);
	}
}

//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================
float R_DoomLightingEquation(float light)
{
	// z is the depth in view space, positive going into the screen
	float z;
	if (((uPalLightLevels >> 8)  & 0xff) == 2)
	{
		z = distance(pixelpos.xyz, uCameraPos.xyz);
	}
	else 
	{
		z = pixelpos.w;
	}

	if ((uPalLightLevels >> 16) == 5) // gl_lightmode 5: Build software lighting emulation.
	{
		// This is a lot more primitive than Doom's lighting...
		float numShades = float(uPalLightLevels & 255);
		float curshade = (1.0 - light) * (numShades - 1.0);
		float visibility = max(uGlobVis * uLightFactor * z, 0.0);
		float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
		return clamp(shade * uLightDist, 0.0, 1.0);
	}

	float colormap = R_DoomColormap(light, z);

	if ((uPalLightLevels & 0xff) != 0)
		colormap = floor(colormap) + 0.5;

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

//===========================================================================
//
// Check if light is in shadow
//
//===========================================================================

#ifdef SUPPORTS_RAYTRACING

bool traceHit(vec3 origin, vec3 direction, float dist)
{
	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TopLevelAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.01f, direction, dist);
	while(rayQueryProceedEXT(rayQuery)) { }
	return rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec2 softshadow[9 * 3] = vec2[](
	vec2( 0.0, 0.0),
	vec2(-2.0,-2.0),
	vec2( 2.0, 2.0),
	vec2( 2.0,-2.0),
	vec2(-2.0, 2.0),
	vec2(-1.0,-1.0),
	vec2( 1.0, 1.0),
	vec2( 1.0,-1.0),
	vec2(-1.0, 1.0),

	vec2( 0.0, 0.0),
	vec2(-1.5,-1.5),
	vec2( 1.5, 1.5),
	vec2( 1.5,-1.5),
	vec2(-1.5, 1.5),
	vec2(-0.5,-0.5),
	vec2( 0.5, 0.5),
	vec2( 0.5,-0.5),
	vec2(-0.5, 0.5),

	vec2( 0.0, 0.0),
	vec2(-1.25,-1.75),
	vec2( 1.75, 1.25),
	vec2( 1.25,-1.75),
	vec2(-1.75, 1.75),
	vec2(-0.75,-0.25),
	vec2( 0.25, 0.75),
	vec2( 0.75,-0.25),
	vec2(-0.25, 0.75)
);

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	if (shadowIndex >= 1024.0)
		return 1.0; // Don't cast rays for this light

	vec3 origin = pixelpos.xzy;
	vec3 target = lightpos.xzy + 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	vec3 direction = normalize(target - origin);
	float dist = distance(origin, target);

	if (uShadowmapFilter <= 0)
	{
		return traceHit(origin, direction, dist) ? 0.0 : 1.0;
	}
	else
	{
		vec3 v = (abs(direction.x) > abs(direction.y)) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 xdir = normalize(cross(direction, v));
		vec3 ydir = cross(direction, xdir);

		float sum = 0.0;
		int step_count = uShadowmapFilter * 9;
		for (int i = 0; i <= step_count; i++)
		{
			vec3 pos = target + xdir * softshadow[i].x + ydir * softshadow[i].y;
			sum += traceHit(origin, normalize(pos - origin), dist) ? 0.0 : 1.0;
		}
		return sum / step_count;
	}
}

#else
#ifdef SUPPORTS_SHADOWMAPS

float shadowDirToU(vec2 dir)
{
	if (abs(dir.y) > abs(dir.x))
	{
		float x = dir.x / dir.y * 0.125;
		if (dir.y >= 0.0)
			return 0.125 + x;
		else
			return (0.50 + 0.125) + x;
	}
	else
	{
		float y = dir.y / dir.x * 0.125;
		if (dir.x >= 0.0)
			return (0.25 + 0.125) - y;
		else
			return (0.75 + 0.125) - y;
	}
}

vec2 shadowUToDir(float u)
{
	u *= 4.0;
	vec2 raydir;
	switch (int(u))
	{
	case 0: raydir = vec2(u * 2.0 - 1.0, 1.0); break;
	case 1: raydir = vec2(1.0, 1.0 - (u - 1.0) * 2.0); break;
	case 2: raydir = vec2(1.0 - (u - 2.0) * 2.0, -1.0); break;
	case 3: raydir = vec2(-1.0, (u - 3.0) * 2.0 - 1.0); break;
	}
	return raydir;
}

float sampleShadowmap(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	vec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x) * 0.25;

	// Snap to shadow map texel grid
	if (abs(ray.z) > abs(ray.x))
	{
		ray.y = ray.y / abs(ray.z);
		ray.x = ray.x / abs(ray.z);
		ray.x = (floor((ray.x + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.z = sign(ray.z);
	}
	else
	{
		ray.y = ray.y / abs(ray.x);
		ray.z = ray.z / abs(ray.x);
		ray.z = (floor((ray.z + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.x = sign(ray.x);
	}

	float t = negD / dot(vWorldNormal.xyz, ray) - bias;
	vec2 dir = ray.xz * t;

	float u = shadowDirToU(dir);
	float dist2 = dot(dir, dir);
	return step(dist2, texture(ShadowMap, vec2(u, v)).x);
}

float sampleShadowmapPCF(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	if (abs(ray.z) > abs(ray.x))
		ray.y = ray.y / abs(ray.z);
	else
		ray.y = ray.y / abs(ray.x);

	vec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x);
	float texelPos = floor(shadowDirToU(ray.xz) * scale);

	float sum = 0.0;
	float step_count = uShadowmapFilter;

	texelPos -= step_count + 0.5;
	for (float x = -step_count; x <= step_count; x++)
	{
		float u = fract(texelPos / scale);
		vec2 dir = shadowUToDir(u);

		ray.x = dir.x;
		ray.z = dir.y;
		float t = negD / dot(vWorldNormal.xyz, ray) - bias;
		dir = ray.xz * t;

		float dist2 = dot(dir, dir);
		sum += step(dist2, texture(ShadowMap, vec2(u, v)).x);
		texelPos++;
	}
	return sum / (uShadowmapFilter * 2.0 + 1.0);
}

float shadowmapAttenuation(vec4 lightpos, float shadowIndex)
{
	if (shadowIndex >= 1024.0)
		return 1.0; // No shadowmap available for this light

	vec3 planePoint = pixelpos.xyz - lightpos.xyz;
	planePoint += 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	if (dot(planePoint.xz, planePoint.xz) < 1.0)
		return 1.0; // Light is too close

	float v = (shadowIndex + 0.5) / 1024.0;

	if (uShadowmapFilter <= 0)
	{
		return sampleShadowmap(planePoint, v);
	}
	else
	{
		return sampleShadowmapPCF(planePoint, v);
	}
}

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	return shadowmapAttenuation(lightpos, shadowIndex);
}

#else

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}

#endif
#endif

float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

//===========================================================================
//
// Adjust normal vector according to the normal map
//
//===========================================================================

#if defined(NORMALMAP)
mat3 cotangent_frame(vec3 n, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	// solve the linear system
	vec3 dp2perp = cross(n, dp2); // cross(dp2, n);
	vec3 dp1perp = cross(dp1, n); // cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = inversesqrt(max(dot(t,t), dot(b,b)));
	return mat3(t * invmax, b * invmax, n);
}

vec3 ApplyNormalMap(vec2 texcoord)
{
	#define WITH_NORMALMAP_UNSIGNED
	#define WITH_NORMALMAP_GREEN_UP
	//#define WITH_NORMALMAP_2CHANNEL

	vec3 interpolatedNormal = normalize(vWorldNormal.xyz);

	vec3 map = texture(normaltexture, texcoord).xyz;
	#if defined(WITH_NORMALMAP_UNSIGNED)
	map = map * 255./127. - 128./127.; // Math so "odd" because 0.5 cannot be precisely described in an unsigned format
	#endif
	#if defined(WITH_NORMALMAP_2CHANNEL)
	map.z = sqrt(1 - dot(map.xy, map.xy));
	#endif
	#if defined(WITH_NORMALMAP_GREEN_UP)
	map.y = -map.y;
	#endif

	mat3 tbn = cotangent_frame(interpolatedNormal, pixelpos.xyz, vTexCoord.st);
	vec3 bumpedNormal = normalize(tbn * map);
	return bumpedNormal;
}
#else
vec3 ApplyNormalMap(vec2 texcoord)
{
	return normalize(vWorldNormal.xyz);
}
#endif

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{
#ifdef NPOT_EMULATION
	if (uNpotEmulation.y != 0.0)
	{
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
	}
#endif	
	material.Base = getTexel(texCoord.st); 
	material.Normal = ApplyNormalMap(texCoord.st);

// OpenGL doesn't care, but Vulkan pukes all over the place if these texture samplings are included in no-texture shaders, even though never called.
#ifndef NO_LAYERS
	if ((uTextureMode & TEXF_Brightmap) != 0)
		material.Bright = desaturate(texture(brighttexture, texCoord.st));

	if ((uTextureMode & TEXF_Detailmap) != 0)
	{
		vec4 Detail = texture(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base.rgb *= Detail.rgb;
	}

	if ((uTextureMode & TEXF_Glowmap) != 0)
		material.Glow = desaturate(texture(glowtexture, texCoord.st));
#endif
}

//===========================================================================
//
// Calculate light
//
// It is important to note that the light color is not desaturated
// due to ZDoom's implementation weirdness. Everything that's added
// on top of it, e.g. dynamic lights and glows are, though, because
// the objects emitting these lights are also.
//
// This is making this a bit more complicated than it needs to
// because we can't just desaturate the final fragment color.
//
//===========================================================================

vec4 getLightColor(Material material, float fogdist, float fogfactor)
{
	vec4 color = vColor;

	if (uLightLevel >= 0.0)
	{
		float newlightlevel = 1.0 - R_DoomLightingEquation(uLightLevel);
		color.rgb *= newlightlevel;
	}
	else if (uFogEnabled > 0)
	{
		// brightening around the player for light mode 2
		if (fogdist < uLightDist)
		{
			color.rgb *= uLightFactor - (fogdist / uLightDist) * (uLightFactor - 1.0);
		}

		//
		// apply light diminishing through fog equation
		//
		color.rgb = mix(vec3(0.0, 0.0, 0.0), color.rgb, fogfactor);
	}

	//
	// handle glowing walls
	//
	if (uGlowTopColor.a > 0.0 && glowdist.x < uGlowTopColor.a)
	{
		color.rgb += desaturate(uGlowTopColor * (1.0 - glowdist.x / uGlowTopColor.a)).rgb;
	}
	if (uGlowBottomColor.a > 0.0 && glowdist.y < uGlowBottomColor.a)
	{
		color.rgb += desaturate(uGlowBottomColor * (1.0 - glowdist.y / uGlowBottomColor.a)).rgb;
	}
	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#if !defined LEGACY_USER_SHADER && !defined NO_LAYERS
	//
	// apply glow 
	//
	color.rgb = mix(color.rgb, material.Glow.rgb, material.Glow.a);

	//
	// apply brightmaps 
	//
	color.rgb = min(color.rgb + material.Bright.rgb, 1.0);
#endif

	//
	// apply other light manipulation by custom shaders, default is a NOP.
	//
	color = ProcessLight(material, color);

	//
	// apply lightmaps
	//
	if (vLightmap.z >= 0.0)
	{
		color.rgb += texture(LightMap, vLightmap).rgb;
	}

	//
	// apply dynamic lights
	//
	return vec4(ProcessMaterialLight(material, color.rgb), material.Base.a * vColor.a);
}

//===========================================================================
//
// Applies colored fog
//
//===========================================================================

vec4 applyFog(vec4 frag, float fogfactor)
{
	return vec4(mix(uFogColor.rgb, frag.rgb, fogfactor), frag.a);
}

//===========================================================================
//
// The color of the fragment if it is fully occluded by ambient lighting
//
//===========================================================================

vec3 AmbientOcclusionColor()
{
	float fogdist;
	float fogfactor;

	//
	// calculate fog factor
	//
	if (uFogEnabled == -1) 
	{
		fogdist = max(16.0, pixelpos.w);
	}
	else 
	{
		fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
	}
	fogfactor = exp2 (uFogDensity * fogdist);

	return mix(uFogColor.rgb, vec3(0.0), fogfactor);
}


//===========================================================================
//
// Nakara receiver-shader sprite alpha projector
//
// This is different from the previous floor/wall quad prototype. It evaluates
// each world receiver fragment directly: light -> receiver ray intersects the
// sprite plane, then a small CPU-built alpha mask decides whether this
// fragment should be darkened. This lets floors, walls, and ceilings receive
// the same projected silhouette without spawning actor/thinker shadows.
//
//===========================================================================

#ifdef NK_RECEIVER_SPRITE_SHADOWS_SUPPORTED

const int NK_SPRITE_PROJECTOR_MASK_SIZE = 32;
const int NK_SPRITE_PROJECTOR_MAX_SHADER_COUNT = 8;

float NkSpriteProjectorMaskFetch(int pi, int x, int y)
{
	x = clamp(x, 0, NK_SPRITE_PROJECTOR_MASK_SIZE - 1);
	y = clamp(y, 0, NK_SPRITE_PROJECTOR_MASK_SIZE - 1);
	int index = y * NK_SPRITE_PROJECTOR_MASK_SIZE + x;
	int vecIndex = index / 4;
	int compIndex = index - vecIndex * 4;
	vec4 maskVec = nkSpriteProjectors[pi].mask[vecIndex];
	if (compIndex == 0) return maskVec.x;
	if (compIndex == 1) return maskVec.y;
	if (compIndex == 2) return maskVec.z;
	return maskVec.w;
}

float NkSpriteProjectorMaskSample(int pi, vec2 uv)
{
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
	{
		return 0.0;
	}

	// v16: 32x32 8-bit alpha mask with bilinear sampling. The earlier 16x16
	// 1-bit row mask was only a diagnostic path and could look like block/slice
	// approximation. This still uses an SSBO mask, not vertical line blockers.
	vec2 p = uv * float(NK_SPRITE_PROJECTOR_MASK_SIZE - 1);
	ivec2 p0 = ivec2(floor(p));
	ivec2 p1 = p0 + ivec2(1);
	vec2 f = fract(p);

	float a00 = NkSpriteProjectorMaskFetch(pi, p0.x, p0.y);
	float a10 = NkSpriteProjectorMaskFetch(pi, p1.x, p0.y);
	float a01 = NkSpriteProjectorMaskFetch(pi, p0.x, p1.y);
	float a11 = NkSpriteProjectorMaskFetch(pi, p1.x, p1.y);
	return mix(mix(a00, a10, f.x), mix(a01, a11, f.x), f.y);
}

float NkSpriteProjectorMaskSampleSoft(int pi, vec2 uv, float softness, float fastPath)
{
	float center = NkSpriteProjectorMaskSample(pi, uv);
	softness = clamp(softness, 0.0, 1.0);
	if (softness <= 0.0)
	{
		return center;
	}

	// v29: fast path. Near-contact shadow pixels should remain crisp and are the
	// most numerous case, so avoid the expensive multi-tap mask blur until the
	// requested softness is visually meaningful. This keeps the v9-anchor single
	// projector look while cutting a large amount of per-fragment SSBO sampling.
	if (fastPath > 0.5 && softness < 0.18)
	{
		return center;
	}

	vec2 stepUV = vec2(1.0 / float(NK_SPRITE_PROJECTOR_MASK_SIZE)) * mix(1.0, 3.0, softness);
	float sum = center * 4.0;
	sum += NkSpriteProjectorMaskSample(pi, uv + vec2( stepUV.x, 0.0));
	sum += NkSpriteProjectorMaskSample(pi, uv + vec2(-stepUV.x, 0.0));
	sum += NkSpriteProjectorMaskSample(pi, uv + vec2(0.0,  stepUV.y));
	sum += NkSpriteProjectorMaskSample(pi, uv + vec2(0.0, -stepUV.y));
	return mix(center, sum / 8.0, softness);
}

float NkSpriteProjectorShadowForOne(int pi, vec3 receiverPos)
{
	float strength = clamp(nkSpriteProjectors[pi].originStrength.w, 0.0, 1.0);
	if (strength <= 0.0)
	{
		return 0.0;
	}

	// Diagnostic modes are encoded as a negative radius by the CPU side.
	// 0 = normal.
	// 1 = shader/SSBO path test: darken all world receiver surfaces.
	// 2 = projector plane test: darken only if light->receiver crosses the sprite plane.
	// 3 = UV rectangle test: darken only if the projected hit falls inside the sprite rect.
	int nkDebugMode = int(max(0.0, -nkSpriteProjectors[pi].lightRadius.w));
	if (nkDebugMode == 1)
	{
		return strength;
	}

	vec3 lightPos = nkSpriteProjectors[pi].lightRadius.xyz;
	vec3 origin = nkSpriteProjectors[pi].originStrength.xyz;
	vec3 rightVec = nkSpriteProjectors[pi].rightSoftness.xyz;
	vec3 upVec = nkSpriteProjectors[pi].upFade.xyz;
	float halfW = length(rightVec);
	float halfH = length(upVec);
	if (halfW <= 0.001 || halfH <= 0.001)
	{
		return 0.0;
	}

	vec3 rightN = rightVec / halfW;
	vec3 upN = upVec / halfH;
	vec3 planeN = normalize(cross(rightN, upN));

	vec3 ray = receiverPos - lightPos;
	float denom = dot(ray, planeN);
	if (abs(denom) <= 0.0001)
	{
		return 0.0;
	}

	float t = dot(origin - lightPos, planeN) / denom;
	// The sprite plane must be between the light and the receiver surface.
	if (t <= 0.0 || t >= 1.0)
	{
		return 0.0;
	}

	if (nkDebugMode == 2)
	{
		return strength;
	}

	vec3 hit = lightPos + ray * t;
	vec3 local = hit - origin;
	vec2 uv;
	uv.x = dot(local, rightN) / halfW * 0.5 + 0.5;
	uv.y = 0.5 - dot(local, upN) / halfH * 0.5;

	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
	{
		return 0.0;
	}

	float receiverLen = length(receiverPos - hit);
	float maxLen = max(nkSpriteProjectors[pi].upFade.w, 1.0);
	float fade = 1.0 - smoothstep(maxLen * 0.75, maxLen, receiverLen);

	// v27: distance-based opacity. The existing maxLength fade only prevents
	// extremely long shadows from hard-clamping at the end. This additional
	// progressive fade makes distant portions weaker so the receiver shadow
	// reads less like a uniformly stamped decal while keeping the single
	// v9-anchor projector path.
	float distanceOpacity = clamp(nkSpriteProjectors[pi].fadeParams.w, 0.0, 1.0);
	if (distanceOpacity > 0.0001)
	{
		float distT = smoothstep(maxLen * 0.15, maxLen * 0.95, receiverLen);
		fade *= mix(1.0, 1.0 - distanceOpacity, distT);
	}

	// v28: receiver surface angle fade. A point light casting onto a surface at a
	// grazing angle should read weaker/softer than a surface facing the light.
	// Use abs(N dot L) to avoid depending on GZDoom front/back normal orientation
	// for one-sided walls while still suppressing grazing receiver pixels.
	float surfaceAngleFade = clamp(nkSpriteProjectors[pi].angleParams.x, 0.0, 1.0);
	if (surfaceAngleFade > 0.0001)
	{
		vec3 receiverN = normalize(vWorldNormal.xyz);
		vec3 toLight = normalize(lightPos - receiverPos);
		float ndl = abs(dot(receiverN, toLight));
		float minDot = clamp(nkSpriteProjectors[pi].angleParams.y, 0.0, 0.95);
		float anglePower = max(nkSpriteProjectors[pi].angleParams.z, 0.05);
		float angleT = smoothstep(minDot, 1.0, ndl);
		angleT = pow(angleT, anglePower);
		fade *= mix(1.0, angleT, surfaceAngleFade);
	}

	// Early-out before the mask fetch. This is a small optimization win for long
	// projected shadows because distance/angle fade often makes far/grazing areas
	// invisible before the 32x32 alpha mask needs to be bilinear-sampled.
	if (fade * strength <= 0.001)
	{
		return 0.0;
	}

	if (nkDebugMode == 3)
	{
		return strength * fade;
	}

	// v26: contact-hardening style softness. A receiver point close to the
	// caster intersection stays crisp; farther receiver points sample a wider
	// penumbra, reducing the stamped/decal-like feeling without adding slices.
	float contactExtraSoftness = clamp(nkSpriteProjectors[pi].fadeParams.y, 0.0, 1.0);
	float contactDistance = max(nkSpriteProjectors[pi].fadeParams.z, 1.0);
	float contactT = smoothstep(0.0, contactDistance, receiverLen);
	float dynamicSoftness = clamp(nkSpriteProjectors[pi].rightSoftness.w + contactExtraSoftness * contactT, 0.0, 1.0);
	float mask = NkSpriteProjectorMaskSampleSoft(pi, uv, dynamicSoftness, nkSpriteProjectors[pi].angleParams.w);
	if (mask <= 0.0)
	{
		return 0.0;
	}

	// v25: when the caster quad is artistically widened, the left/right edges can
	// look like a hard rectangular border. Fade only the edge region in UV space;
	// this keeps the v23/v24 anchor and single receiver-projector approach intact.
	float sideFadeWidth = clamp(nkSpriteProjectors[pi].fadeParams.x, 0.0, 0.45);
	if (sideFadeWidth > 0.0001)
	{
		float leftEdge = smoothstep(0.0, sideFadeWidth, uv.x);
		float rightEdge = 1.0 - smoothstep(1.0 - sideFadeWidth, 1.0, uv.x);
		mask *= clamp(min(leftEdge, rightEdge), 0.0, 1.0);
	}

	// v24: soften the top UV border so the far/top end of the projected
	// silhouette does not terminate with a hard card-like cap. In normal mode
	// lightRadius.w carries the top fade width; debug modes keep this negative.
	float topFadeWidth = max(nkSpriteProjectors[pi].lightRadius.w, 0.0);
	if (topFadeWidth > 0.0001)
	{
		mask *= smoothstep(0.0, topFadeWidth, uv.y);
	}

	return mask * strength * fade;
}

float NkSpriteProjectorShadowAttenuation()
{
	// Only world surfaces should receive this. Do not affect 2D/UI/fog-layer paths.
	if (uFogEnabled == -3 || (uTextureMode & 0xffff) == 7)
	{
		return 1.0;
	}

	// Sprites/models normally have no useful vWorldNormal here, and applying the
	// receiver projector to them causes self-shadow artifacts.
	if (dot(vWorldNormal.xyz, vWorldNormal.xyz) < 0.25)
	{
		return 1.0;
	}

	float shadow = 0.0;
	int count = min(int(nkSpriteProjectors.length()), NK_SPRITE_PROJECTOR_MAX_SHADER_COUNT);
	for (int i = 0; i < count; ++i)
	{
		shadow = max(shadow, NkSpriteProjectorShadowForOne(i, pixelpos.xyz));
		if (shadow >= 0.98)
		{
			break;
		}
	}
	return clamp(1.0 - shadow, 0.0, 1.0);
}

#endif

//===========================================================================
//
// Main shader routine
//
//===========================================================================



float NkVisThruWallOutlineMask(vec2 st, float thickness)
{
	if (thickness <= 0.0)
	{
		return 0.0;
	}

	vec2 texSize = vec2(textureSize(tex, 0));
	if (texSize.x <= 0.0 || texSize.y <= 0.0)
	{
		return 0.0;
	}

	vec2 px = vec2(thickness) / texSize;
	float centerAlpha = texture(tex, st).a;
	float aroundAlpha = 0.0;

	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2( px.x,  0.0)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2(-px.x,  0.0)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2( 0.0,  px.y)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2( 0.0, -px.y)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2( px.x,  px.y)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2(-px.x,  px.y)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2( px.x, -px.y)).a);
	aroundAlpha = max(aroundAlpha, texture(tex, st + vec2(-px.x, -px.y)).a);

	float edge = max(aroundAlpha - centerAlpha, 0.0);
	return smoothstep(0.05, 0.35, edge);
}

vec3 ApplyFocusTint(vec3 baseColor, vec3 tintColor, float amount, int style)
{
	vec3 tinted;

	if (style == 0)
	{
		// Multiply: preserves the original shading but can darken already-dark sprites.
		tinted = baseColor * tintColor;
	}
	else if (style == 1)
	{
		// Add: strong bright color emphasis.
		tinted = clamp(baseColor + tintColor, 0.0, 1.0);
	}
	else if (style == 2)
	{
		// Screen: brightens with the tint color without burning as hard as Add.
		tinted = 1.0 - (1.0 - baseColor) * (1.0 - tintColor);
	}
	else if (style == 3)
	{
		// Overlay: higher contrast, useful for colored emphasis while keeping detail.
		vec3 low = 2.0 * baseColor * tintColor;
		vec3 high = 1.0 - 2.0 * (1.0 - baseColor) * (1.0 - tintColor);
		tinted = mix(low, high, step(vec3(0.5), baseColor));
	}
	else
	{
		// Solid: directly mixes toward the tint color.
		tinted = tintColor;
	}

	return mix(baseColor, tinted, clamp(amount, 0.0, 1.0));
}

void main()
{
#ifdef NO_CLIPDISTANCE_SUPPORT
	if (ClipDistanceA.x < 0 || ClipDistanceA.y < 0 || ClipDistanceA.z < 0 || ClipDistanceA.w < 0 || ClipDistanceB.x < 0) discard;
#endif

#ifndef LEGACY_USER_SHADER
	Material material;

	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif
	vec4 frag = material.Base;
	float nkVisThruWallSpriteAlpha = clamp(uNkVisThruWallSpriteAlpha, 0.0, 1.0);
	float nkVisThruWallOutlineAlpha = clamp(uNkVisThruWallOutlineAlpha, 0.0, 1.0);
	float nkVisThruWallOutlineMask = 0.0;
	if (uNkVisThruWallTint.a > 0.0 && uNkVisThruWallOutlineThickness > 0.0)
	{
		nkVisThruWallOutlineMask = NkVisThruWallOutlineMask(vTexCoord.st, uNkVisThruWallOutlineThickness);
	}

	// [Nakara] FocusHighlight direct sprite outline.
	// When the runtime FocusTint/Zawarudo strength is active, replace the old
	// SceneFocusInfo/postprocess outline with the same texture-alpha outline
	// method used by VisThruWall. This avoids missing outlines on attack frames
	// while also preventing the old outline and this direct outline from stacking.
	bool nkFocusDirectOutlineActive = (uFocusHighlight > 0.0 && uFocusOutlineThickness > 0.0 && uFocusTintAmount > 0.0);
	float nkFocusDirectOutlineMask = 0.0;
	if (nkFocusDirectOutlineActive)
	{
		float nkFocusDirectOutlineStrength = clamp(uFocusTintAmount, 0.0, 1.0);
		nkFocusDirectOutlineMask = NkVisThruWallOutlineMask(vTexCoord.st, uFocusOutlineThickness) * nkFocusDirectOutlineStrength;
	}

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold && nkVisThruWallOutlineMask <= 0.0 && nkFocusDirectOutlineMask <= 0.0) discard;
#endif

	if (uFogEnabled != -3)	// check for special 2D 'fog' mode.
	{
		float fogdist = 0.0;
		float fogfactor = 0.0;

		//
		// calculate fog factor
		//
		if (uFogEnabled != 0)
		{
			if (uFogEnabled == 1 || uFogEnabled == -1) 
			{
				fogdist = max(16.0, pixelpos.w);
			}
			else 
			{
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			}
			fogfactor = exp2 (uFogDensity * fogdist);
		}

		if ((uTextureMode & 0xffff) != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);

			//
			// colored fog
			//
			if (uFogEnabled < 0) 
			{
				frag = applyFog(frag, fogfactor);
			}
		}
		else
		{
			frag = vec4(uFogColor.rgb, (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
	}
	else // simple 2D (uses the fog color to add a color overlay)
	{
		if ((uTextureMode & 0xffff) == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}
			frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	FragColor = frag;

#ifdef NK_RECEIVER_SPRITE_SHADOWS_SUPPORTED
	// [Nakara] Receiver-shader sprite projector shadows.
	// Multiplies the already-lit receiver surface, so it behaves like a dark projected mask
	// on floors, walls and ceilings rather than as a separate quad/decal actor.
	FragColor.rgb *= NkSpriteProjectorShadowAttenuation();
#endif

	if (uNkVisThruWallTint.a > 0.0)
	{
		float nkVisThruWallTintAmount = clamp(uNkVisThruWallTint.a, 0.0, 1.0);
		FragColor.rgb = mix(FragColor.rgb, uNkVisThruWallTint.rgb, nkVisThruWallTintAmount);
	}

	if (nkVisThruWallOutlineMask > 0.0)
	{
		FragColor.rgb = mix(FragColor.rgb, uNkVisThruWallTint.rgb, nkVisThruWallOutlineMask);
		FragColor.a = max(FragColor.a, nkVisThruWallOutlineMask * nkVisThruWallOutlineAlpha);
	}

	if (uFocusTintAmount > 0.0)
	{
		FragColor.rgb = ApplyFocusTint(FragColor.rgb, uFocusTintColor.rgb, uFocusTintAmount, uFocusTintStyle);
	}

	if (nkFocusDirectOutlineMask > 0.0)
	{
		// Match the old postprocess outline feel more closely: add the outline color
		// on top instead of replacing/mixing the sprite color.
		FragColor.rgb = clamp(FragColor.rgb + uFocusOutlineColor.rgb * nkFocusDirectOutlineMask, 0.0, 1.0);
		FragColor.a = max(FragColor.a, nkFocusDirectOutlineMask);
	}

#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 8) * 8 + int(pixelpos.y) % 8;
	const float DITHER_THRESHOLDS[64] =
	float[64](
		1.0 / 65.0, 33.0 / 65.0, 9.0 / 65.0, 41.0 / 65.0, 3.0 / 65.0, 35.0 / 65.0, 11.0 / 65.0, 43.0 / 65.0,
		49.0 / 65.0, 17.0 / 65.0, 57.0 / 65.0, 25.0 / 65.0, 51.0 / 65.0, 19.0 / 65.0, 59.0 / 65.0, 27.0 / 65.0,
		13.0 / 65.0, 45.0 / 65.0, 5.0 / 65.0, 37.0 / 65.0, 15.0 / 65.0, 47.0 / 65.0, 7.0 / 65.0, 39.0 / 65.0,
		61.0 / 65.0, 29.0 / 65.0, 53.0 / 65.0, 21.0 / 65.0, 63.0 / 65.0, 31.0 / 65.0, 55.0 / 65.0, 23.0 / 65.0,
		4.0 / 65.0, 36.0 / 65.0, 12.0 / 65.0, 44.0 / 65.0, 2.0 / 65.0, 34.0 / 65.0, 10.0 / 65.0, 42.0 / 65.0,
		52.0 / 65.0, 20.0 / 65.0, 60.0 / 65.0, 28.0 / 65.0, 50.0 / 65.0, 18.0 / 65.0, 58.0 / 65.0, 26.0 / 65.0,
		16.0 / 65.0, 48.0 / 65.0, 8.0 / 65.0, 40.0 / 65.0, 14.0 / 65.0, 46.0 / 65.0, 6.0 / 65.0, 38.0 / 65.0,
		64.0 / 65.0, 32.0 / 65.0, 56.0 / 65.0, 24.0 / 65.0, 62.0 / 65.0, 30.0 / 65.0, 54.0 / 65.0, 22.0 /65.0
	);

	vec3 fragHSV = rgb2hsv(FragColor.rgb);
	float brightness = clamp(1.5*fragHSV.z, 0.1, 1.0);
	if (DITHER_THRESHOLDS[index] < brightness) discard;
	else FragColor *= 0.5;
#endif

#ifdef GBUFFER_PASS
	// Keep SceneNormal.a reserved only for the focus mask.
	// Do not store outline data in SceneNormal.rgb or SceneFog.a here; reusing those buffers
	// can break the mask/postprocess path and can interfere with normal/fog data.
	float focusMask = clamp(uFocusHighlight * FragColor.a, 0.0, 1.0);

	float focusPostOutlineMask = nkFocusDirectOutlineActive ? 0.0 : focusMask;

	FragFog = vec4(AmbientOcclusionColor(), 1.0);
	FragNormal = vec4(vEyeNormal.xyz * 0.5 + 0.5, focusMask);
	FragFocusInfo = vec4(uFocusOutlineColor.rgb, clamp(uFocusOutlineThickness / 8.0, 0.0, 1.0) * focusPostOutlineMask);

	// [Nakara] Dedicated cloak mask. Use the texture alpha before Actor.Alpha is
	// applied so a +CLOAK actor still writes its complete silhouette at Alpha 0.
	// A cloaked fragment writes the mask with source alpha 1.0 so it still works
	// when Actor.Alpha is zero. Ordinary translucent fragments use their normal
	// output alpha, so they only attenuate a cloak mask behind them proportionally.
	float nkCloakMask = clamp(uNkCloakAmount * material.Base.a, 0.0, 1.0);
	float nkCloakWriteAlpha = uNkCloakAmount > 0.0001 ? 1.0 : FragColor.a;
	FragCloak = vec4(nkCloakMask, 0.0, 0.0, nkCloakWriteAlpha);
#endif
}
