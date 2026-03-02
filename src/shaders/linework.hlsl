// src/shaders/linework.hlsl
// Linework geometry shader — Phase 5
//
// Pipeline:
//   VS — MVP transform; passes clip-space pos (SV_POSITION) + world-space pos to GS.
//   GS — expands each line segment into a screen-aligned quad (triangle strip,
//         max 4 vertices, 2 triangles).  Width = kLineWidthPx pixels (must match
//         terrain::LINEWORK_WIDTH_PX = 2.0f defined in Config.h).
//   PS — solid white for now; per-layer colour added in Phase 8.
//
// Topology: LINELIST in, TRIANGLESTRIP out.

#pragma pack_matrix(row_major)

cbuffer MVP : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
};

// viewportSize in pixels — passed by LineworkPass::Begin from g_renderer.Width/Height.
cbuffer LineData : register(b1)
{
    float2 viewportSize;  // (width, height) in pixels
    float2 _lp;
};

// ── Vertex shader ──────────────────────────────────────────────────────────────

struct VSIn
{
    float3 pos : POSITION;
};

struct GSIn
{
    float4 clipPos  : SV_POSITION;
    float3 worldPos : TEXCOORD0;   // unused in Phase 5; reserved for future lighting
};

GSIn VS(VSIn v)
{
    GSIn o;
    float4 wPos = mul(float4(v.pos, 1.0f), world);
    float4 vPos = mul(wPos, view);
    o.clipPos   = mul(vPos, proj);
    o.worldPos  = wPos.xyz;
    return o;
}

// ── Geometry shader ────────────────────────────────────────────────────────────
//
// Width derivation:
//   - (n1 - n0) * viewportSize converts NDC diff → approximate pixel-space diff
//     (both axes scaled by the same viewport dimension, so normalize gives
//      a true unit vector in screen pixel space).
//   - off = perp * kLineWidthPx * float2(1/W, 1/H) is the NDC half-width offset.
//     Pixel check: off.x * W/2 = perp.x * kLineWidthPx / 2  →  kLineWidthPx pixels
//                  total width.  ✓
//   - Multiply NDC offset by w before adding to clip-space (undo the implicit /w).

struct PSIn
{
    float4 pos : SV_POSITION;
};

[maxvertexcount(4)]
void GS(line GSIn input[2], inout TriangleStream<PSIn> stream)
{
    // Must match terrain::LINEWORK_WIDTH_PX in Config.h.
    static const float kLineWidthPx = 2.0f;

    float4 p0 = input[0].clipPos;
    float4 p1 = input[1].clipPos;

    // Skip segments where either endpoint is behind the near plane.
    if (p0.w < 1e-4f || p1.w < 1e-4f) return;

    // Perspective-divide to NDC.
    float2 n0 = p0.xy / p0.w;
    float2 n1 = p1.xy / p1.w;

    // Direction in pixel space (viewport scale converts NDC diff to pixel diff;
    // normalising cancels the common factor so result is a unit screen vector).
    float2 diff = (n1 - n0) * viewportSize;
    if (dot(diff, diff) < 1e-6f) return;  // zero-length segment — skip

    float2 dir  = normalize(diff);
    float2 perp = float2(-dir.y, dir.x);  // unit vector perpendicular in screen space

    // NDC half-width offset (kLineWidthPx total → kLineWidthPx/2 half-width pixels).
    // off.x pixel check: off.x * (W/2) = perp.x * kLineWidthPx * (1/W) * (W/2)
    //                                  = perp.x * kLineWidthPx / 2  ✓
    float2 off = perp * kLineWidthPx * float2(1.0f / viewportSize.x,
                                              1.0f / viewportSize.y);

    // Emit quad as triangle strip.
    // Multiply NDC offset by clip-space w to recover the clip-space offset.
    PSIn o;
    o.pos = p0 + float4(off * p0.w, 0.0f, 0.0f);  stream.Append(o);
    o.pos = p0 - float4(off * p0.w, 0.0f, 0.0f);  stream.Append(o);
    o.pos = p1 + float4(off * p1.w, 0.0f, 0.0f);  stream.Append(o);
    o.pos = p1 - float4(off * p1.w, 0.0f, 0.0f);  stream.Append(o);
    stream.RestartStrip();
}

// ── Pixel shader ───────────────────────────────────────────────────────────────

float4 PS(PSIn p) : SV_Target
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);   // solid white; per-layer colour in Phase 8
}
