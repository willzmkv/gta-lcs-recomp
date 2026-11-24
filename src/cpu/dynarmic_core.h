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

    // Initialize a secondary core for a guest thread. The stack and TLS block are
    // allocated at any free host address (guest and host share one address space)
    // and released when the core is destroyed.
    bool initialize_thread(size_t stack_size = 4ULL * 1024 * 1024);

    // Core currently executing guest code on the calling host thread (nullptr if none).
    static DynarmicCore* current();

    // Handler used for any SVC number not registered on the executing core. This lets
    // every guest thread core dispatch the shared HLE trampoline table.
    static void set_fallback_svc_handler(SVCHandler handler);

    // Terminate the active top-level call() (used by pthread_exit). The value returned
    // by the current SVC handler becomes the call result.
    void request_exit();

    // Register inspection & modification
    void set_reg(size_t index, uint64_t value);
    uint64_t get_reg(size_t index) const;

    void set_pc(uint64_t pc);
    uint64_t get_pc() const;

    void set_sp(uint64_t sp);
    uint64_t get_sp() const;

    void set_fp_float(size_t index, float value);
    float get_fp_float(size_t index) const;

    void set_fp_double(size_t index, double value);
    double get_fp_double(size_t index) const;

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

    // Read guest memory directly (guest and host share address space)
    static bool read_guest_memory(uint64_t vaddr, void* out, size_t size);

    struct Impl;
private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace lcs::cpu
