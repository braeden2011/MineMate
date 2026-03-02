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

    // Update per-tile visibility from 6 frustum planes (see ExtractFrustumPlanes).
    // Automatically calls RequestLoad for tiles that newly enter the frustum.
    void UpdateVisibility(const std::array<DirectX::XMFLOAT4, 6>& planes);

    // Enqueue tile idx for GPU upload (sets state LOADING).
    // No-op if tile is not in EMPTY state.
    void RequestLoad(int idx);

    // Load up to maxLoads queued tiles from disk to GPU.
    // Pass INT_MAX (the default) to flush the entire queue with no budget limit.
    void FlushLoads(ID3D11Device* device, int maxLoads = 0x7fffffff);

    // Release GPU buffers for tile idx (sets state EVICTED).
    void Evict(int idx);

    // Returns all GPU-resident tiles as draw items (always LOD 0 in Phase 3).
    std::vector<DrawItem> GetDrawList() const;

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
        TileState state   = TileState::EMPTY;
        bool      visible = false;
        Mesh      mesh;
        std::filesystem::path lod0Path;
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
