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

    // Load up to maxLoads queued tiles from disk to GPU.
    // Pass INT_MAX (the default) to flush the entire queue with no budget limit.
    void FlushLoads(ID3D11Device* device, int maxLoads = 0x7fffffff);

    // Release GPU buffers for tile idx (sets state EVICTED, tile will not auto-reload).
    void Evict(int idx);

    // Select per-tile LOD from camera distance and return all GPU-resident draw items.
    // Tiles whose active LOD no longer matches the desired LOD are evicted and
    // re-queued; they will be absent from the list for one frame while reloading.
    std::vector<DrawItem> GetDrawList(const DirectX::XMFLOAT3& cameraPos);

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
};

// ── Frustum plane extraction ──────────────────────────────────────────────────
// Extracts 6 normalised clip planes from the combined view-projection matrix.
// Plane equation: dot(plane.xyz, point) + plane.w >= 0  means point is inside.
// Uses the Gribb-Hartmann method adapted for DirectXMath row-major convention.
std::array<DirectX::XMFLOAT4, 6> ExtractFrustumPlanes(
    const DirectX::XMMATRIX& view,
    const DirectX::XMMATRIX& proj);
