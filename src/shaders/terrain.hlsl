// src/shaders/terrain.hlsl
// Lambert terrain shader — Phase 2 Session 1
// Row-major matrices, Z-up right-handed world space.

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

// Per-tile data: surface opacity and freshness overlay tint.
// overlayColor.a = 0 → no overlay; a > 0 → lerp toward overlayColor.rgb.
cbuffer TileData : register(b2)
{
    float3 lodTint;
    float  opacity;
    float4 overlayColor;  // rgb=tint, a=blend factor; (1,1,1,0)=no overlay
};

struct VSIn
{
    float3 pos      : POSITION;
    float3 nrm      : NORMAL;
    float  elevTint : COLOR;      // 0.0 in Phase 1; elevation tint in Phase 2+
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
    // Earthy tan base colour; elevTint adds a grey lift at higher elevations.
    static const float3 kBase = float3(0.55f, 0.50f, 0.40f);
    float3 surfaceColor = saturate(kBase + p.elevTint);
    float  nDotL        = max(dot(normalize(p.nrm), lightDir), 0.0f);
    float3 lit          = saturate(surfaceColor * (ambient + nDotL * diffuse));
    float3 tinted       = lit * lodTint;
    // Freshness overlay: lerp toward overlayColor.rgb by overlayColor.a.
    float3 final        = lerp(tinted, overlayColor.rgb, overlayColor.a);
    return float4(final, opacity);
}
