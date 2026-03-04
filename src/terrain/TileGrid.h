#pragma once
// src/terrain/TileGrid.h
// Manages all terrain tiles: per-tile AABB, state machine, frustum culling,
// load queue, and GPU-resident draw list.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <vector>

#include "terrain/Mesh.h"

using Microsoft::WRL::ComPtr;

class GpuBudget;   // forward declaration — implementation in GpuBudget.h

// ── Per-tile GPU state ────────────────────────────────────────────────────────

enum class TileState : uint8_t
{
    EMPTY,    // on disk, never requested
    LOADING,  // queued for GPU upload
    GPU,      // VB + IB resident on GPU
    EVICTED,  // was GPU, buffers released
};

// ── TileGrid ──────────────────────────────────────────────────────────────────

class TileGrid
{
public:
    struct DrawItem
    {
        int         tileIdx;
        int         lod;
        const Mesh* mesh;   // non-owning; valid while tile is in GPU state
    };

    // Scan cacheDir for tile_*_lod0.bin files and build the internal tile list.
    // Returns false if no tiles are found.
    bool Init(const std::filesystem::path& cacheDir);

    // Update per-tile visibility from frustum planes and compute initial LOD from
    // camera position. Automatically calls RequestLoad for newly-visible EMPTY tiles.
    void UpdateVisibility(const std::array<DirectX::XMFLOAT4, 6>& planes,
                          const DirectX::XMFLOAT3& cameraPos);

    // Enqueue tile idx to load at the given LOD (sets state LOADING).
    // No-op if tile is not in EMPTY state.
    void RequestLoad(int idx, int lod);

    // Attach a GpuBudget tracker.  Must be called before the first FlushLoads.
    // TileGrid does NOT own the budget object; caller manages its lifetime.
    void SetBudget(GpuBudget* budget) { m_budget = budget; }

    // Set a base offset added to all tile indices passed to GpuBudget.
    // Required when two TileGrids share one GpuBudget to avoid key collisions.
    // terrain grid: base=0 (default).  design grid: base=100000.
    void SetBudgetIndexBase(int base) { m_budgetBase = base; }

    // Shift all tile AABBs by (dx, dy, dz).
    // Used to align a design/linework grid whose DXF $EXTMIN differs from the
    // authoritative terrain scene origin.  Call once after Init(), before the
    // first UpdateVisibility().
    void ApplyOriginOffset(float dx, float dy, float dz);

    // When enabled, all LOD selection returns LOD0 (full detail).
    // Useful for diagnosing LOD-related rendering artefacts.
    void SetForceLod0(bool force) { m_forceLod0 = force; }
    bool GetForceLod0()    const  { return m_forceLod0; }

    // Load up to maxLoads queued tiles from disk to GPU.
    // Pass INT_MAX (the default) to flush the entire queue with no budget limit.
    // When a GpuBudget is attached, enforces memory budget via LRU eviction before
    // each upload; tiles that cannot be loaded even after eviction are skipped this frame.
    void FlushLoads(ID3D11Device* device, int maxLoads = 0x7fffffff);

    // Release GPU buffers for tile idx (sets state EVICTED, tile will not auto-reload).
    void Evict(int idx);

    // Evict all GPU-resident tiles and untrack them from the GpuBudget.
    // Call before destroying this grid so the shared budget stays accurate.
    void EvictAll();

    // Select per-tile LOD from camera distance and return all GPU-resident draw items.
    // Tiles whose active LOD no longer matches the desired LOD are evicted and
    // re-queued; they will be absent from the list for one frame while reloading.
    std::vector<DrawItem> GetDrawList(const DirectX::XMFLOAT3& cameraPos);

    // Ray-cast against all GPU-resident tile AABBs.
    // Returns the scene-space hit point on the nearest intersecting AABB,
    // or false if no GPU tile is hit.  Used for LMB double-click pivot set.
    bool RayCast(const DirectX::XMFLOAT3& rayOrigin,
                 const DirectX::XMFLOAT3& rayDir,
                 DirectX::XMFLOAT3&       hitOut) const;

    // Triangle-level surface pick.  Reads the hit tile's LOD0 .bin from disk;
    // slower than RayCast() but returns the exact intersection point on the
    // rendered surface.  Falls back to the AABB hit point if the .bin read
    // fails or no triangle is intersected.  Returns false if no GPU tile is hit.
    bool RayCastDetailed(const DirectX::XMFLOAT3& rayOrigin,
                         const DirectX::XMFLOAT3& rayDir,
                         DirectX::XMFLOAT3&       hitOut) const;

    // ── Spatial helpers for camera initialisation ─────────────────────────
    // XY centre of all populated tiles, z = 0.
    DirectX::XMFLOAT3 SceneCentre() const;
    // Half-diagonal of the XY extent of all populated tiles.
    float SceneRadius() const;

    // ── Stats for ImGui ───────────────────────────────────────────────────
    int TileCount()    const { return static_cast<int>(m_tiles.size()); }
    int VisibleCount() const;
    int GpuCount()     const;

private:
    struct TileEntry
    {
        int   tx, ty;
        DirectX::XMFLOAT3 aabbMin;
        DirectX::XMFLOAT3 aabbMax;
        TileState state     = TileState::EMPTY;
        bool      visible   = false;
        int       activeLod = -1;   // which LOD is in m_mesh (-1 = none)
        int       targetLod =  0;   // which LOD is being loaded (valid in LOADING state)
        Mesh      mesh;
        std::filesystem::path lodPaths[3];  // paths to lod0/1/2 .bin (empty = not on disk)
    };

    std::vector<TileEntry> m_tiles;
    std::vector<int>       m_loadQueue;   // indices into m_tiles

    GpuBudget*               m_budget       = nullptr;   // non-owning; may be null
    int                      m_budgetBase   = 0;         // offset for shared GpuBudget
    DirectX::XMFLOAT3        m_lastCamPos   = {0.0f, 0.0f, 0.0f};
    bool                     m_forceLod0    = true;
    // Cumulative offset applied by ApplyOriginOffset.  Used to convert raw
    // disk-space vertex positions back to scene space in FlushLoads and RayCastDetailed.
    DirectX::XMFLOAT3        m_originOffset = {0.0f, 0.0f, 0.0f};
};

// ── Frustum plane extraction ──────────────────────────────────────────────────
// Extracts 6 normalised clip planes from the combined view-projection matrix.
// Plane equation: dot(plane.xyz, point) + plane.w >= 0  means point is inside.
// Uses the Gribb-Hartmann method adapted for DirectXMath row-major convention.
std::array<DirectX::XMFLOAT4, 6> ExtractFrustumPlanes(
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& proj);
