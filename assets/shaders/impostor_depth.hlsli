#ifndef IMPOSTOR_DEPTH_HLSLI
#define IMPOSTOR_DEPTH_HLSLI
float decodeImpostorDepth(float2 packed, float width) {
    const float2 bytes = floor(packed * 255.0 + 0.5);
    return ((bytes.x * 256.0 + bytes.y) / 65535.0 - 0.5) * width;
}
bool intersectImpostorDepth(float3 plane, float3 towardEye, float depth,
                           float2 size, float view, out float3 hit, out float2 uv) {
    const float angle = view * 0.7853981633974483;
    const float2 right = float2(cos(angle), sin(angle));
    const float2 eye = float2(-right.y, right.x);
    const float denominator = dot(towardEye.xy, eye);
    hit = plane; uv = 0.0;
    if (denominator <= 0.2 || any(size <= 0.0) || view < 0.0 || view > 7.0) return false;
    hit += towardEye * ((depth - dot(plane.xy, eye)) / denominator);
    uv = float2(dot(hit.xy, right) / size.x + 0.5, 1.0 - hit.z / size.y);
    return true;
}
#endif
