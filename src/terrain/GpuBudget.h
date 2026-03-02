#pragma once
// src/terrain/GpuBudget.h
// Tracks GPU memory usage across all GPU-resident tiles and implements LRU eviction.
//
// Call Track/Untrack when a tile is loaded/evicted.
// Call Touch for every tile that appears in the draw list (updates LRU timestamp).
// Call HasRoom before loading; if false, call ChooseEvictee to select a victim.
//
// No DX11 dependency — usable from any translation unit.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>   // pair
#include <vector>

class GpuBudget
{
public:
    explicit GpuBudget(size_t budgetBytes);

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Record a newly uploaded tile.  gpuBytes = vertCount*28 + indexCount*4.
    void Track(int tileIdx, size_t gpuBytes);

    // Remove a tile from tracking (call after GPU buffers are released).
    void Untrack(int tileIdx);

    // Update the LRU timestamp for a tile used this frame (call inside draw loop).
    void Touch(int tileIdx);

    // ── Budget queries ────────────────────────────────────────────────────────

    // True if uploading addBytes would stay within the budget.
    bool HasRoom(size_t addBytes) const;

    // Choose the best eviction candidate and return its tile index (or -1 if none).
    // Priority:
    //   1. LRU tile among culledGpuIndices (invisible/out-of-frustum GPU tiles).
    //   2. Farthest tile among visibleWithDist if no culled tiles are tracked.
    int ChooseEvictee(
        const std::vector<int>&                    culledGpuIndices,
        const std::vector<std::pair<int, float>>&  visibleWithDist) const;

    // ── Stats ─────────────────────────────────────────────────────────────────
    size_t UsedBytes()   const { return m_usedBytes; }
    size_t BudgetBytes() const { return m_budgetBytes; }
    int    EvictCount()  const { return m_evictCount; }

    // Increment the eviction counter; call once per forced eviction.
    void RecordEviction() { ++m_evictCount; }

private:
    struct Entry
    {
        size_t   gpuBytes;
        uint64_t lruTick;   // higher = more recently used
    };

    std::unordered_map<int, Entry> m_entries;
    size_t   m_usedBytes   = 0;
    size_t   m_budgetBytes = 0;
    uint64_t m_tick        = 0;
    int      m_evictCount  = 0;
};
