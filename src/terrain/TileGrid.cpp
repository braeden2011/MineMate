// src/terrain/TileGrid.cpp
#include "terrain/TileGrid.h"
#include "terrain/Config.h"
#include "terrain/GpuBudget.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>   // sscanf_s
#include <fstream>
#include <string>
#include <system_error>   // std::error_code for fs::file_size

using namespace DirectX;
namespace fs = std::filesystem;

// ── Frustum plane extraction ──────────────────────────────────────────────────
//
// For DirectXMath row-major matrices: clip position P' = P_world * VP.
// The combined matrix VP = XMMatrixMultiply(view, proj).
// Using vpT = transpose(VP): vpT.r[j] == column j of VP.
//
// D3D NDC range: -1 <= x/w <= 1, -1 <= y/w <= 1, 0 <= z/w <= 1.
// Inside tests (w > 0):
//   Left:   P'.x + P'.w >= 0  →  (col[0] + col[3]) · P >= 0
//   Right:  P'.w - P'.x >= 0  →  (col[3] - col[0]) · P >= 0
//   Bottom: P'.y + P'.w >= 0  →  (col[1] + col[3]) · P >= 0
//   Top:    P'.w - P'.y >= 0  →  (col[3] - col[1]) · P >= 0
//   Near:   P'.z       >= 0   →   col[2]            · P >= 0
//   Far:    P'.w - P'.z >= 0  →  (col[3] - col[2]) · P >= 0

std::array<XMFLOAT4, 6> ExtractFrustumPlanes(
    const XMMATRIX& view, const XMMATRIX& proj)
{
    const XMMATRIX vp  = XMMatrixMultiply(view, proj);
    const XMMATRIX vpT = XMMatrixTranspose(vp);  // vpT.r[j] = col[j] of vp

    const XMVECTOR pv[6] = {
        XMPlaneNormalize(vpT.r[3] + vpT.r[0]),  // Left
        XMPlaneNormalize(vpT.r[3] - vpT.r[0]),  // Right
        XMPlaneNormalize(vpT.r[3] + vpT.r[1]),  // Bottom
        XMPlaneNormalize(vpT.r[3] - vpT.r[1]),  // Top
        XMPlaneNormalize(vpT.r[2]),              // Near (D3D: z in [0,1])
        XMPlaneNormalize(vpT.r[3] - vpT.r[2]),  // Far
    };

    std::array<XMFLOAT4, 6> out;
    for (int i = 0; i < 6; ++i)
        XMStoreFloat4(&out[i], pv[i]);
    return out;
}

// ── AABB-vs-frustum test ──────────────────────────────────────────────────────
// Returns false if the AABB is entirely outside any frustum plane (cull).
// Uses the p-vertex method: most positive vertex along each plane normal.

static bool AabbInsideFrustum(
    const XMFLOAT3& mn, const XMFLOAT3& mx,
    const std::array<XMFLOAT4, 6>& planes)
{
    for (const auto& p : planes) {
        const float px = p.x >= 0.0f ? mx.x : mn.x;
        const float py = p.y >= 0.0f ? mx.y : mn.y;
        const float pz = p.z >= 0.0f ? mx.z : mn.z;
        if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f)
            return false;   // entirely on the negative side of this plane
    }
    return true;
}

// ── LOD distance helper ───────────────────────────────────────────────────────
// Returns the desired LOD level (0/1/2) based on 3D distance from camera to
// tile AABB centre.  Uses Config.h constants — no magic numbers.

static int ComputeDesiredLod(const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax,
                              const XMFLOAT3& camPos)
{
    const float cx = (aabbMin.x + aabbMax.x) * 0.5f;
    const float cy = (aabbMin.y + aabbMax.y) * 0.5f;
    const float cz = (aabbMin.z + aabbMax.z) * 0.5f;
    const float dist = sqrtf((cx - camPos.x) * (cx - camPos.x) +
                             (cy - camPos.y) * (cy - camPos.y) +
                             (cz - camPos.z) * (cz - camPos.z));
    if (dist < terrain::LOD_DISTANCES_M[0]) return 0;
    if (dist < terrain::LOD_DISTANCES_M[1]) return 1;
    return 2;
}

// ── TileGrid::Init ────────────────────────────────────────────────────────────

bool TileGrid::Init(const fs::path& cacheDir)
{
    m_tiles.clear();
    m_loadQueue.clear();

    for (auto& entry : fs::directory_iterator(cacheDir)) {
        const auto name = entry.path().filename().string();

        // Accept only tile_{tx}_{ty}_lod0.bin
        if (name.size() < 5 || name.rfind("tile_", 0) != 0) continue;
        if (name.find("_lod0.bin") == std::string::npos)     continue;

        int tx = 0, ty = 0;
        if (sscanf_s(name.c_str(), "tile_%d_%d_lod0.bin", &tx, &ty) != 2) continue;

        TileEntry e;
        e.tx = tx;
        e.ty = ty;

        // Populate paths for all three LOD levels.
        // lodPaths[n] is left empty (default-constructed) if the file doesn't exist.
        const fs::path dir  = entry.path().parent_path();
        const std::string stem = "tile_" + std::to_string(tx) + "_" + std::to_string(ty);
        for (int lod = 0; lod < 3; ++lod) {
            fs::path p = dir / (stem + "_lod" + std::to_string(lod) + ".bin");
            if (fs::exists(p)) e.lodPaths[lod] = std::move(p);
        }

        // Initial AABB: XY from tile grid index, conservative Z.
        // +1 m XY margin accounts for centroid-binned vertices straying outside
        // the nominal tile boundary (current Phase 1 parser limitation).
        const float x0 = static_cast<float>(tx)     * terrain::TILE_SIZE_M - 1.0f;
        const float y0 = static_cast<float>(ty)     * terrain::TILE_SIZE_M - 1.0f;
        const float x1 = static_cast<float>(tx + 1) * terrain::TILE_SIZE_M + 1.0f;
        const float y1 = static_cast<float>(ty + 1) * terrain::TILE_SIZE_M + 1.0f;
        e.aabbMin = { x0, y0, -500.0f };
        e.aabbMax = { x1, y1, 2000.0f };

        m_tiles.push_back(std::move(e));
    }

    return !m_tiles.empty();
}

// ── TileGrid::UpdateVisibility ────────────────────────────────────────────────

void TileGrid::UpdateVisibility(const std::array<XMFLOAT4, 6>& planes,
                                 const XMFLOAT3& cameraPos)
{
    m_lastCamPos = cameraPos;   // cached for FlushLoads eviction candidates

    for (int i = 0; i < static_cast<int>(m_tiles.size()); ++i) {
        auto& t = m_tiles[i];
        t.visible = AabbInsideFrustum(t.aabbMin, t.aabbMax, planes);

        if (!t.visible) continue;

        // Visible EVICTED tiles: reset to EMPTY so they can be re-queued.
        // Without this, tiles evicted by budget pressure become permanent holes —
        // the old !wasVisible guard never fires again for a tile that was already
        // visible when evicted.
        if (t.state == TileState::EVICTED)
            t.state = TileState::EMPTY;

        // Queue any visible EMPTY tile (not just newly-entered-frustum ones).
        // RequestLoad is a no-op if state != EMPTY, so this is safe every frame.
        if (t.state == TileState::EMPTY) {
            const int lod = m_forceLod0 ? 0
                          : ComputeDesiredLod(t.aabbMin, t.aabbMax, cameraPos);
            RequestLoad(i, lod);
        }
    }
}

// ── TileGrid::RequestLoad ─────────────────────────────────────────────────────

void TileGrid::RequestLoad(int idx, int lod)
{
    auto& t = m_tiles[idx];
    if (t.state != TileState::EMPTY) return;
    // Fall back to LOD0 if the requested LOD file doesn't exist on disk.
    while (lod > 0 && t.lodPaths[lod].empty()) --lod;
    t.targetLod = lod;
    t.state     = TileState::LOADING;
    m_loadQueue.push_back(idx);
}

// ── TileGrid::FlushLoads ──────────────────────────────────────────────────────

// ── File-size helper: estimate GPU bytes before loading ───────────────────────
// TLET layout: 5×uint32 header (20 bytes) + VB (vertCount×28) + IB (indexCount×4).
// fileSize − 20  ==  GPU bytes needed.

static size_t PeekGpuBytes(const fs::path& p)
{
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    return (!ec && sz > 20) ? static_cast<size_t>(sz - 20) : 0;
}

void TileGrid::FlushLoads(ID3D11Device* device, int maxLoads)
{
    // Sort the load queue each frame so the best tiles load first:
    //   Primary key:   currently-visible (in frustum) tiles before non-visible.
    //                  Tiles that were visible when enqueued may have left the
    //                  frustum since; don't let them block what's on screen now.
    //   Secondary key: closer tiles before farther ones (squared dist, no sqrt).
    // Re-sorted every call: camera position and t.visible both change each frame.
    if (m_loadQueue.size() > 1) {
        const XMFLOAT3 cam = m_lastCamPos;
        std::sort(m_loadQueue.begin(), m_loadQueue.end(),
            [&](int a, int b) {
                const auto& ta = m_tiles[a];
                const auto& tb = m_tiles[b];
                // Primary: visible tiles first
                if (ta.visible != tb.visible) return ta.visible;
                // Secondary: closer tile first
                const float cax = (ta.aabbMin.x + ta.aabbMax.x) * 0.5f;
                const float cay = (ta.aabbMin.y + ta.aabbMax.y) * 0.5f;
                const float caz = (ta.aabbMin.z + ta.aabbMax.z) * 0.5f;
                const float cbx = (tb.aabbMin.x + tb.aabbMax.x) * 0.5f;
                const float cby = (tb.aabbMin.y + tb.aabbMax.y) * 0.5f;
                const float cbz = (tb.aabbMin.z + tb.aabbMax.z) * 0.5f;
                const float da2 = (cax-cam.x)*(cax-cam.x)
                                + (cay-cam.y)*(cay-cam.y)
                                + (caz-cam.z)*(caz-cam.z);
                const float db2 = (cbx-cam.x)*(cbx-cam.x)
                                + (cby-cam.y)*(cby-cam.y)
                                + (cbz-cam.z)*(cbz-cam.z);
                return da2 < db2;
            });
    }

    int loaded = 0;
    auto it = m_loadQueue.begin();
    while (it != m_loadQueue.end() && loaded < maxLoads) {
        const int idx = *it;
        auto& t = m_tiles[idx];

        // ── Budget enforcement: evict LRU tiles until there is room ───────────
        if (m_budget) {
            const size_t needed = PeekGpuBytes(t.lodPaths[t.targetLod]);

            while (!m_budget->HasRoom(needed)) {
                // Build eviction candidate lists from the current tile set.
                std::vector<int>                    culled;
                std::vector<std::pair<int, float>>  visible;
                for (int j = 0; j < static_cast<int>(m_tiles.size()); ++j) {
                    const auto& tj = m_tiles[j];
                    if (tj.state != TileState::GPU) continue;
                    const float cx = (tj.aabbMin.x + tj.aabbMax.x) * 0.5f;
                    const float cy = (tj.aabbMin.y + tj.aabbMax.y) * 0.5f;
                    const float cz = (tj.aabbMin.z + tj.aabbMax.z) * 0.5f;
                    const float dist = sqrtf(
                        (cx - m_lastCamPos.x) * (cx - m_lastCamPos.x) +
                        (cy - m_lastCamPos.y) * (cy - m_lastCamPos.y) +
                        (cz - m_lastCamPos.z) * (cz - m_lastCamPos.z));
                    if (!tj.visible) culled.push_back(m_budgetBase + j);
                    else             visible.emplace_back(m_budgetBase + j, dist);
                }

                const int victim = m_budget->ChooseEvictee(culled, visible);
                if (victim < 0) break;  // nothing to evict — give up this tile
                m_budget->RecordEviction();
                Evict(victim - m_budgetBase);  // convert budget key → local index
            }

            // If budget still full after eviction, skip tile this frame.
            if (!m_budget->HasRoom(needed)) {
                ++it;
                continue;
            }
        }

        // ── GPU upload ────────────────────────────────────────────────────────
        if (t.mesh.Load(device, t.lodPaths[t.targetLod])) {
            t.activeLod = t.targetLod;
            t.state     = TileState::GPU;
            // Refine Z range from actual vertex AABB now that we have it.
            // Add m_originOffset.z so the result is in scene space rather than
            // disk space (important when this grid has been shifted via ApplyOriginOffset).
            t.aabbMin.z = t.mesh.AabbMin().z + m_originOffset.z;
            t.aabbMax.z = t.mesh.AabbMax().z + m_originOffset.z;
            // Record GPU footprint.
            if (m_budget) {
                const size_t actualBytes =
                    static_cast<size_t>(t.mesh.VertCount())  * 28 +
                    static_cast<size_t>(t.mesh.IndexCount()) * 4;
                m_budget->Track(m_budgetBase + idx, actualBytes);
            }
            ++loaded;
        } else {
            // Failed (bad tile or out of VRAM) — mark EVICTED to stop retrying.
            t.state = TileState::EVICTED;
        }

        it = m_loadQueue.erase(it);
    }
}

// ── TileGrid::Evict ───────────────────────────────────────────────────────────

void TileGrid::Evict(int idx)
{
    auto& t = m_tiles[idx];
    if (m_budget) m_budget->Untrack(m_budgetBase + idx);
    t.mesh      = Mesh{};   // releases ComPtr VB + IB
    t.activeLod = -1;
    t.state     = TileState::EVICTED;
}

// ── TileGrid::GetDrawList ─────────────────────────────────────────────────────
// Performs per-tile LOD selection from camera distance.
// GPU tiles at the wrong LOD are evicted and re-queued; they are absent for one
// frame while reloading.

std::vector<TileGrid::DrawItem> TileGrid::GetDrawList(const XMFLOAT3& cameraPos)
{
    std::vector<DrawItem> list;
    list.reserve(m_tiles.size());

    for (int i = 0; i < static_cast<int>(m_tiles.size()); ++i) {
        auto& t = m_tiles[i];
        if (t.state != TileState::GPU) continue;

        const int desired = m_forceLod0 ? 0
                          : ComputeDesiredLod(t.aabbMin, t.aabbMax, cameraPos);

        if (desired != t.activeLod) {
            // LOD transition: release current buffers, re-enqueue at new LOD.
            t.mesh      = Mesh{};
            t.activeLod = -1;
            t.state     = TileState::EMPTY;
            RequestLoad(i, desired);
            // Tile absent this frame; will appear next frame at the correct LOD.
            continue;
        }

        if (m_budget) m_budget->Touch(m_budgetBase + i);
        list.push_back({ i, t.activeLod, &t.mesh });
    }

    return list;
}

// ── TileGrid::RayCast ─────────────────────────────────────────────────────────
// Slab method (Smits, 1994): divides by ray direction components, handles
// parallel rays correctly via IEEE-754 ±INF.  Tests only GPU-resident tiles.

bool TileGrid::RayCast(const XMFLOAT3& rayOriginF, const XMFLOAT3& rayDirF,
                       XMFLOAT3& hitOut) const
{
    const XMVECTOR ro = XMLoadFloat3(&rayOriginF);
    const XMVECTOR rd = XMLoadFloat3(&rayDirF);

    float bestT = FLT_MAX;
    bool  hit   = false;

    for (const auto& t : m_tiles) {
        if (t.state != TileState::GPU) continue;

        const XMVECTOR mn   = XMLoadFloat3(&t.aabbMin);
        const XMVECTOR mx   = XMLoadFloat3(&t.aabbMax);
        const XMVECTOR invD = XMVectorReciprocal(rd);

        const XMVECTOR t1   = XMVectorMultiply(mn - ro, invD);
        const XMVECTOR t2   = XMVectorMultiply(mx - ro, invD);
        const XMVECTOR tMin = XMVectorMin(t1, t2);
        const XMVECTOR tMax = XMVectorMax(t1, t2);

        // tNear = max component of tMin; tFar = min component of tMax
        const float tNear = std::max({ XMVectorGetX(tMin),
                                       XMVectorGetY(tMin),
                                       XMVectorGetZ(tMin) });
        const float tFar  = std::min({ XMVectorGetX(tMax),
                                       XMVectorGetY(tMax),
                                       XMVectorGetZ(tMax) });

        if (tFar < tNear || tFar < 0.0f) continue;   // miss or behind camera
        const float tHit = tNear >= 0.0f ? tNear : tFar;
        if (tHit < bestT) { bestT = tHit; hit = true; }
    }

    if (!hit) return false;
    XMStoreFloat3(&hitOut, ro + rd * bestT);
    return true;
}

// ── TileGrid::RayCastDetailed ─────────────────────────────────────────────────
// Collects ALL GPU-resident tiles the ray hits (AABB test), sorts them by entry
// distance, then runs Möller–Trumbore on each in order with early-exit once the
// global best triangle hit is closer than the next tile's AABB entry.
// Falls back to the nearest AABB entry point only if no triangle is found at all.

bool TileGrid::RayCastDetailed(const XMFLOAT3& rayOriginF, const XMFLOAT3& rayDirF,
                                XMFLOAT3& hitOut) const
{
    const XMVECTOR ro  = XMLoadFloat3(&rayOriginF);
    const XMVECTOR rd  = XMLoadFloat3(&rayDirF);
    const XMVECTOR off = XMLoadFloat3(&m_originOffset);

    // ── Phase 1: AABB pass — collect all hit tiles ────────────────────────
    struct Candidate { float tNear; const TileEntry* tile; };
    std::vector<Candidate> cands;
    cands.reserve(8);

    for (const auto& t : m_tiles) {
        if (t.state != TileState::GPU) continue;

        const XMVECTOR invD = XMVectorReciprocal(rd);
        const XMVECTOR t1   = XMVectorMultiply(XMLoadFloat3(&t.aabbMin) - ro, invD);
        const XMVECTOR t2   = XMVectorMultiply(XMLoadFloat3(&t.aabbMax) - ro, invD);
        const XMVECTOR tMin = XMVectorMin(t1, t2);
        const XMVECTOR tMax = XMVectorMax(t1, t2);

        const float tNear = std::max({ XMVectorGetX(tMin),
                                       XMVectorGetY(tMin),
                                       XMVectorGetZ(tMin) });
        const float tFar  = std::min({ XMVectorGetX(tMax),
                                       XMVectorGetY(tMax),
                                       XMVectorGetZ(tMax) });

        if (tFar < tNear || tFar < 0.0f) continue;
        // Camera inside AABB (tNear<0): use 0 so this tile sorts first and is
        // never skipped by the early-exit check (using tFar caused the bug).
        cands.push_back({ tNear >= 0.0f ? tNear : 0.0f, &t });
    }

    if (cands.empty()) return false;

    // Sort front-to-back so early-exit fires as soon as possible.
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.tNear < b.tNear; });

    // AABB fallback = entry point of the nearest tile.
    XMFLOAT3 aabbHit{};
    XMStoreFloat3(&aabbHit, ro + rd * cands[0].tNear);

    // ── Phase 2: Möller–Trumbore across all candidate tiles ───────────────
    constexpr float kEps   = 1e-7f;
    float           bestT  = FLT_MAX;

    // Helper: read one tile's LOD0 .bin and update bestT.
    auto testTile = [&](const TileEntry* tile) {
        const fs::path& binPath = tile->lodPaths[0];
        if (binPath.empty()) return;

        std::ifstream f(binPath, std::ios::binary);
        if (!f) return;

        uint32_t magic = 0, version = 0, lod = 0, vertCount = 0, indexCount = 0;
        f.read(reinterpret_cast<char*>(&magic),      4);
        f.read(reinterpret_cast<char*>(&version),    4);
        f.read(reinterpret_cast<char*>(&lod),        4);
        f.read(reinterpret_cast<char*>(&vertCount),  4);
        f.read(reinterpret_cast<char*>(&indexCount), 4);
        if (!f || magic != 0x544C4554U || vertCount == 0 || indexCount == 0) return;

        // Positions: first 12 bytes of each 28-byte TerrainVertex; skip remaining 16.
        std::vector<XMFLOAT3> pos(vertCount);
        for (uint32_t i = 0; i < vertCount; ++i) {
            f.read(reinterpret_cast<char*>(&pos[i]), 12);
            f.seekg(16, std::ios::cur);
        }
        std::vector<uint32_t> idx(indexCount);
        f.read(reinterpret_cast<char*>(idx.data()),
               static_cast<std::streamsize>(indexCount * 4));
        if (!f) return;

        const uint32_t triCount = indexCount / 3;
        for (uint32_t i = 0; i < triCount; ++i) {
            const uint32_t i0 = idx[i * 3];
            const uint32_t i1 = idx[i * 3 + 1];
            const uint32_t i2 = idx[i * 3 + 2];
            if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;

            // Disk vertices are in disk/design space; add offset → scene space.
            const XMVECTOR v0 = XMLoadFloat3(&pos[i0]) + off;
            const XMVECTOR v1 = XMLoadFloat3(&pos[i1]) + off;
            const XMVECTOR v2 = XMLoadFloat3(&pos[i2]) + off;

            const XMVECTOR e1 = v1 - v0;
            const XMVECTOR e2 = v2 - v0;
            const XMVECTOR h  = XMVector3Cross(rd, e2);
            const float    a  = XMVectorGetX(XMVector3Dot(e1, h));
            if (a > -kEps && a < kEps) continue;

            const float    fInv = 1.0f / a;
            const XMVECTOR s    = ro - v0;
            const float    u    = fInv * XMVectorGetX(XMVector3Dot(s, h));
            if (u < 0.0f || u > 1.0f) continue;

            const XMVECTOR q = XMVector3Cross(s, e1);
            const float    v = fInv * XMVectorGetX(XMVector3Dot(rd, q));
            if (v < 0.0f || u + v > 1.0f) continue;

            const float t = fInv * XMVectorGetX(XMVector3Dot(e2, q));
            if (t > kEps && t < bestT) bestT = t;
        }
    };

    for (const auto& c : cands) {
        // Early-exit: any closer tile already found a triangle hit.
        if (bestT < c.tNear) break;
        testTile(c.tile);
    }

    if (bestT < FLT_MAX)
        XMStoreFloat3(&hitOut, ro + rd * bestT);
    else
        hitOut = aabbHit;   // no triangle hit — fall back to nearest AABB entry

    return true;
}

// ── TileGrid spatial helpers ──────────────────────────────────────────────────

XMFLOAT3 TileGrid::SceneCentre() const
{
    if (m_tiles.empty()) return { 0.0f, 0.0f, 0.0f };

    float xMin =  FLT_MAX, xMax = -FLT_MAX;
    float yMin =  FLT_MAX, yMax = -FLT_MAX;
    for (const auto& t : m_tiles) {
        if (t.aabbMin.x < xMin) xMin = t.aabbMin.x;
        if (t.aabbMax.x > xMax) xMax = t.aabbMax.x;
        if (t.aabbMin.y < yMin) yMin = t.aabbMin.y;
        if (t.aabbMax.y > yMax) yMax = t.aabbMax.y;
    }
    return { (xMin + xMax) * 0.5f, (yMin + yMax) * 0.5f, 0.0f };
}

float TileGrid::SceneRadius() const
{
    if (m_tiles.empty()) return 360.6f;

    float xMin =  FLT_MAX, xMax = -FLT_MAX;
    float yMin =  FLT_MAX, yMax = -FLT_MAX;
    for (const auto& t : m_tiles) {
        if (t.aabbMin.x < xMin) xMin = t.aabbMin.x;
        if (t.aabbMax.x > xMax) xMax = t.aabbMax.x;
        if (t.aabbMin.y < yMin) yMin = t.aabbMin.y;
        if (t.aabbMax.y > yMax) yMax = t.aabbMax.y;
    }
    const float dx = xMax - xMin;
    const float dy = yMax - yMin;
    return sqrtf(dx * dx + dy * dy) * 0.5f;
}

// ── TileGrid::ApplyOriginOffset ───────────────────────────────────────────────

void TileGrid::ApplyOriginOffset(float dx, float dy, float dz)
{
    m_originOffset.x += dx;
    m_originOffset.y += dy;
    m_originOffset.z += dz;
    for (auto& t : m_tiles) {
        t.aabbMin.x += dx; t.aabbMin.y += dy; t.aabbMin.z += dz;
        t.aabbMax.x += dx; t.aabbMax.y += dy; t.aabbMax.z += dz;
    }
}

// ── TileGrid stats ────────────────────────────────────────────────────────────

int TileGrid::VisibleCount() const
{
    int n = 0;
    for (const auto& t : m_tiles) if (t.visible) ++n;
    return n;
}

int TileGrid::GpuCount() const
{
    int n = 0;
    for (const auto& t : m_tiles) if (t.state == TileState::GPU) ++n;
    return n;
}
