#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace lcs::cpu {

class DynarmicCore;

using HLEFunction = std::function<uint64_t(const uint64_t* args)>;

class HLEManager {
public:
    static constexpr uint64_t TRAMPOLINE_BASE = 0x7f00200000ULL;
    static constexpr size_t   MAX_TRAMPOLINES = 8192;

    static HLEManager& instance();

    bool initialize();

    // Register a known HLE function implementation
    uint64_t register_function(const std::string& name, HLEFunction handler);

    // Resolve an external symbol by name (auto-stubs if not yet registered)
    uint64_t resolve(const char* name);

    // Connect all registered handlers to DynarmicCore
    void wire_to_cpu(DynarmicCore& cpu);
    void set_cpu(DynarmicCore* cpu) { m_cpu = cpu; }
    DynarmicCore* get_cpu() const { return m_cpu; }

    // Register default stubs (Bionic, libc, pthreads, system, JNI, GLES, OpenAL)
    void register_default_stubs();

private:
    HLEManager();
    ~HLEManager();

    uint64_t allocate_trampoline(uint32_t id);

    void* m_trampoline_mem = nullptr;
    DynarmicCore* m_cpu = nullptr;
    std::unordered_map<std::string, uint32_t> m_symbol_to_id;
    std::vector<std::string> m_id_to_name;
    std::vector<HLEFunction> m_handlers;
};

} // namespace lcs::cpu
