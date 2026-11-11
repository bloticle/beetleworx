// Inputs follow the original EM2 depth-fog pass: clip XY is interpolator0,
// independent of the depth UV in interpolator1. Rows are guest pixel c13..16.
// The native program temporarily stores Y/X/Z; this API returns world XYZ.
bool SceneUnproject(float2 clipXY, float sampledDepth, float4 rows[4], out float3 world) {
    world = 0;
    if (!all(isfinite(clipXY)) || !isfinite(sampledDepth) || sampledDepth < 0 || sampledDepth > 1) return false;
    float4 clip = float4(clipXY, 1 - sampledDepth, 1);
    float4 p = float4(dot(rows[0], clip), dot(rows[1], clip), dot(rows[2], clip), dot(rows[3], clip));
    if (!all(isfinite(p)) || abs(p.w) < 1e-7) return false;
    float3 candidate = p.xyz / p.w;
    if (!all(isfinite(candidate))) return false;
    world = candidate;
    return true;
}
