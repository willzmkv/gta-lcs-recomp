#pragma once

#include <cstdint>

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
} // namespace lcs::cpu

namespace lcs::game {

// Native x64 host fast-path replacement for BatchedWorld::C_WorldRenderManager::Render
// and BatchedModel::C_DeferredBatchManager::Render. Eliminates Dynarmic JIT execution,
// per-draw RB-tree lookups, Rockstar's redundant VAO glBindBuffer bug, and redundant
// texture/shader-state calls while dispatching directly into RenderBridge.
class LcsBatchedFast {
public:
    static LcsBatchedFast& instance();

    void initialize(const lcs::cpu::ElfImage& image_game,
                    lcs::cpu::DynarmicCore& cpu,
                    uint64_t load_bias);

private:
    LcsBatchedFast() = default;
    ~LcsBatchedFast();
    LcsBatchedFast(const LcsBatchedFast&) = delete;
    LcsBatchedFast& operator=(const LcsBatchedFast&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace lcs::game
