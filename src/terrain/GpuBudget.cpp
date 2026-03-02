// src/terrain/GpuBudget.cpp
#include "terrain/GpuBudget.h"

#include <limits>

GpuBudget::GpuBudget(size_t budgetBytes)
    : m_budgetBytes(budgetBytes)
{}

void GpuBudget::Track(int tileIdx, size_t gpuBytes)
{
    auto [it, inserted] = m_entries.emplace(tileIdx, Entry{gpuBytes, ++m_tick});
    if (!inserted) {
        // Tile re-loaded (e.g. after LOD switch) — update bytes and refresh tick.
        m_usedBytes        -= it->second.gpuBytes;
        it->second.gpuBytes = gpuBytes;
        it->second.lruTick  = m_tick;
    }
    m_usedBytes += gpuBytes;
}

void GpuBudget::Untrack(int tileIdx)
{
    auto it = m_entries.find(tileIdx);
    if (it != m_entries.end()) {
        m_usedBytes -= it->second.gpuBytes;
        m_entries.erase(it);
    }
}

void GpuBudget::Touch(int tileIdx)
{
    auto it = m_entries.find(tileIdx);
    if (it != m_entries.end())
        it->second.lruTick = ++m_tick;
}

bool GpuBudget::HasRoom(size_t addBytes) const
{
    return m_usedBytes + addBytes <= m_budgetBytes;
}

int GpuBudget::ChooseEvictee(
    const std::vector<int>&                   culledGpuIndices,
    const std::vector<std::pair<int, float>>& visibleWithDist) const
{
    // ── Priority 1: LRU culled (invisible) tile ───────────────────────────────
    if (!culledGpuIndices.empty()) {
        int      best   = -1;
        uint64_t oldest = std::numeric_limits<uint64_t>::max();
        for (int idx : culledGpuIndices) {
            auto it = m_entries.find(idx);
            if (it != m_entries.end() && it->second.lruTick < oldest) {
                oldest = it->second.lruTick;
                best   = idx;
            }
        }
        if (best >= 0) return best;
    }

    // ── Priority 2: Farthest visible tile ────────────────────────────────────
    int   best     = -1;
    float farthest = -1.0f;
    for (const auto& [idx, dist] : visibleWithDist) {
        if (m_entries.count(idx) && dist > farthest) {
            farthest = dist;
            best     = idx;
        }
    }
    return best;
}
