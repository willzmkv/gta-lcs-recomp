#pragma once

#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace lcs::cpu {

using SVCHandler = std::function<uint64_t(uint32_t swi, const uint64_t* args)>;

class DynarmicCore {
public:
    DynarmicCore();
    ~DynarmicCore();

    DynarmicCore(const DynarmicCore&) = delete;
    DynarmicCore& operator=(const DynarmicCore&) = delete;

    bool initialize();

    // Register inspection & modification
    void set_reg(size_t index, uint64_t value);
    uint64_t get_reg(size_t index) const;

    void set_pc(uint64_t pc);
    uint64_t get_pc() const;

    void set_sp(uint64_t sp);
    uint64_t get_sp() const;

    void set_tpidr_el0(uint64_t tls_addr);
    uint64_t get_tpidr_el0() const;

    // Execution
    void run();
    void step();
    void halt();
    void clear_cache();
    void invalidate_cache(uint64_t vaddr, size_t size);

    // Call a function at guest address with arguments
    uint64_t call(uint64_t address, const std::vector<uint64_t>& args = {});
    uint64_t call_nested(uint64_t address, const std::vector<uint64_t>& args = {});

    // SVC Handling
    void register_svc_handler(uint32_t swi, SVCHandler handler);

    struct Impl;
private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcs::cpu
