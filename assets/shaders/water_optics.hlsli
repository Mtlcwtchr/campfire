#ifndef WATER_OPTICS_HLSLI
#define WATER_OPTICS_HLSLI
// How water looks, as a medium rather than as a paint.
//
// The model is the one Unreal's Single Layer Water material is built on:
// a participating medium with an absorption and a scattering coefficient per
// colour channel, a dielectric interface with a Schlick fresnel (F0 = 0.02),
// and a microfacet (GGX) highlight for the sun. What the eye gets from a
// pixel of water is
//
//     reflected sky + sun highlight              (the interface, weight F)
//   + light scattered back out of the water      (the medium, weight 1 - F)
//   + the bed, attenuated along the view path    (transmittance T)
//
// Nothing behind the water is sampled here - the bed is whatever the blend
// finds in the target - so the last term is carried by alpha: alpha is how
// much of the bed does NOT come through, and the colour is everything else
// divided by it. That is exact for a grey transmittance; the per-channel
// part of T (red dying first, why deep water is blue) is carried by the
// scattered term instead, which is where the eye actually sees it.
//
// Needs landscape_look.hlsli (sun, sky, eye) and the PS constant buffers.

struct WaterMedium {
    float3 absorption;   // per metre
    float3 scattering;   // per metre
};

// Clear sea, green lake, turbid river. Numbers are pure water's own
// absorption (red goes in a couple of metres, blue in tens) with a little
// scattering for the sea, chlorophyll and dissolved organics for a lake, and
// suspended silt for running water.
WaterMedium waterMedium(float river, float lake, float ocean)
{
    WaterMedium m;
    const float3 seaAbsorb = float3(0.45, 0.070, 0.040);
    const float3 seaScatter = float3(0.0060, 0.0180, 0.0240);
    const float3 lakeAbsorb = float3(0.50, 0.100, 0.160);
    const float3 lakeScatter = float3(0.0120, 0.0300, 0.0180);
    const float3 riverAbsorb = float3(0.55, 0.190, 0.240);
    const float3 riverScatter = float3(0.0550, 0.0600, 0.0400);
    // Mixed through a matrix rather than as three scaled vectors: `ocean` is a
    // scalar select upstream, and DXC folds a vector scaled by it into an
    // OpSelect that its own SPIR-V validator rejects.
    const float3 w = float3(ocean, lake, river) / max(river + lake + ocean, 1e-4);
    const float3x3 absorb = float3x3(seaAbsorb, lakeAbsorb, riverAbsorb);
    const float3x3 scatter = float3x3(seaScatter, lakeScatter, riverScatter);
    m.absorption = mul(w, absorb);
    m.scattering = mul(w, scatter);
    return m;
}

float waterFresnel(float cosine)
{
    const float f = 1.0 - saturate(cosine);
    const float f2 = f * f;
    return 0.02 + 0.98 * f2 * f2 * f;
}

// GGX distribution with the Smith-Schlick visibility folded in, times N.L.
float waterSunHighlight(float3 n, float3 v, float3 l, float roughness)
{
    const float3 h = normalize(v + l);
    const float nl = saturate(dot(n, l)), nv = max(dot(n, v), 1e-3), nh = saturate(dot(n, h));
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    const float distribution = a2 / (3.14159265 * d * d);
    const float k = a * 0.5;
    const float visibility = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
    return distribution * visibility * nl * waterFresnel(saturate(dot(v, h)));
}

struct WaterShade {
    float3 colour;   // straight (not premultiplied) colour
    float alpha;     // how much of the bed is hidden
    float fresnel;
};

// `depth` is metres of water under this pixel, `lift` the crest height the
// vertex stage added, `metresPerPixel` the footprint (for specular
// anti-aliasing), `sunshine` 1 under a clear sky and less under cloud.
WaterShade shadeWater(float3 position, float3 surface, float depth, float lift,
                      float river, float lake, float ocean, float metresPerPixel,
                      float sunshine, float wind)
{
    WaterShade o;
    const float3 eye = landscapeEye(position);
    const float3 sun = landscapeSun();
    const WaterMedium medium = waterMedium(river, lake, ocean);
    const float3 extinction = medium.absorption + medium.scattering;

    // The bed is seen along the view ray through the water, and was lit along
    // the sun's; both are longer the flatter the angle. Held off the horizon
    // so water seen edge-on is opaque rather than infinitely so.
    const float viewCos = max(abs(eye.z), 0.12), sunCos = max(sun.z, 0.12);
    const float wet = max(depth, 0.0);
    const float3 transmittance = exp(-extinction * wet * (1.0 / viewCos + 1.0 / sunCos));
    const float3 viewTransmittance = exp(-extinction * wet / viewCos);

    const float nv = saturate(dot(surface, eye));
    o.fresnel = waterFresnel(nv);

    // What the interface reflects: the sky in the mirrored direction, kept
    // above the horizon (a ripple cannot reflect the ground it is not facing).
    float3 mirrored = reflect(-eye, surface);
    mirrored.z = max(mirrored.z, 0.02);
    const float3 sky = landscapeSkyRay(normalize(mirrored));

    // Specular anti-aliasing: a footprint wider than the ripples widens the
    // lobe (what the ripples would have averaged to) instead of sparkling.
    const float roughness = clamp(0.06 + 0.05 * wind +
                                  0.22 * smoothstep(0.25, 12.0, metresPerPixel), 0.04, 0.45);
    const float3 sunColour = float3(1.04, 0.99, 0.90) * sunshine;
    const float highlight = min(waterSunHighlight(surface, eye, sun, roughness), 24.0);

    // Single scattering, integrated over an infinitely deep column and cut by
    // what is left of it: the albedo of the medium, lit by sun and sky.
    const float3 albedo = medium.scattering / max(extinction, 1e-4);
    const float3 ambient = landscapeSky(0.8) * 0.9;
    const float3 lightIn = sunColour * (0.35 + 0.65 * saturate(sun.z)) + ambient;
    float3 scattered = albedo * lightIn * (1.0 - viewTransmittance) * 2.2;

    // Light through a crest from behind it: the green-blue glow on the back
    // of a swell when looking toward the sun (UE's water "subsurface" term).
    const float towardSun = pow(saturate(dot(-eye.xy, sun.xy) * 0.5 + 0.5), 4.0);
    const float crest = saturate(lift * 1.4) * saturate(1.0 - nv * 0.6);
    scattered += albedo * sunColour * towardSun * crest * 3.0 * ocean;

    const float3 reflected = sky * o.fresnel + sunColour * highlight;
    const float bedShows = (1.0 - o.fresnel) * dot(transmittance, float3(0.30, 0.50, 0.20));
    const float3 premultiplied = reflected + (1.0 - o.fresnel) * scattered;
    // A UNORM target cannot hold a straight colour above one, so a highlight
    // brighter than the alpha allows takes the alpha it needs instead.
    o.alpha = saturate(max(1.0 - bedShows,
                           max(premultiplied.r, max(premultiplied.g, premultiplied.b))));
    o.colour = premultiplied / max(o.alpha, 0.02);
    return o;
}
#endif

