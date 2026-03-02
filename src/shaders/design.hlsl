// src/shaders/design.hlsl
// Design surface shader — Phase 4 Session 1
// Lambert shading + alpha opacity for two-pass translucent rendering.
// Identical geometry / lighting pipeline to terrain.hlsl; differs only in
// base colour (blue-grey) and the opacity field in TileData.

#pragma pack_matrix(row_major)

cbuffer MVP : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
};

cbuffer Light : register(b1)
{
    float3 lightDir;  // normalised world-space, pointing FROM surface TOWARD light
    float  _p0;
    float3 ambient;
    float  _p1;
    float3 diffuse;
    float  _p2;
};

// Per-tile data: LOD tint overlay + translucency.
// opacity replaces the unused _tp padding from terrain.hlsl.
cbuffer TileData : register(b2)
{
    float3 lodTint;  // (1,1,1) when overlay off; coloured when LOD overlay on
    float  opacity;  // 0.0=transparent, 1.0=opaque; hardcoded 0.6 in Phase 4
};

struct VSIn
{
    float3 pos      : POSITION;
    float3 nrm      : NORMAL;
    float  elevTint : COLOR;
};

struct PSIn
{
    float4 pos      : SV_POSITION;
    float3 nrm      : NORMAL;
    float  elevTint : COLOR;
};

PSIn VS(VSIn v)
{
    PSIn o;
    float4 wPos = mul(float4(v.pos, 1.0f), world);
    float4 vPos = mul(wPos, view);
    o.pos       = mul(vPos, proj);
    o.nrm       = normalize(mul(float4(v.nrm, 0.0f), world).xyz);
    o.elevTint  = v.elevTint;
    return o;
}

float4 PS(PSIn p) : SV_Target
{
    // Blue-grey base colour — visually distinct from the earthy-tan terrain.
    static const float3 kBase = float3(0.35f, 0.45f, 0.60f);
    float3 surfaceColor = saturate(kBase + p.elevTint);
    float  nDotL        = max(dot(normalize(p.nrm), lightDir), 0.0f);
    float3 lit          = saturate(surfaceColor * (ambient + nDotL * diffuse));
    return float4(lit * lodTint, opacity);
}
