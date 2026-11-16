#include "dynarmic_core.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <unordered_map>
#include <atomic>
#include <array>
#include <mutex>
#include <vector>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace lcs::cpu {

static constexpr uint64_t MAGIC_RETURN_PC = 0x7f00100000ULL;
static constexpr uint64_t GUEST_STACK_TOP = 0x7f601f0000ULL;
static constexpr uint32_t RETURN_SVC_ID   = 0x000000ff;

class CoreCallbacks : public Dynarmic::A64::UserCallbacks {
public:
    CoreCallbacks(DynarmicCore::Impl* parent) : m_parent(parent) {}

    std::uint64_t MemoryRead(Dynarmic::A64::VAddr vaddr, std::size_t size) override {
        if (vaddr < 0x1000) {
            std::cerr << "[CRASH GUARD] MemoryRead of " << size << " bytes from low vaddr 0x"
                      << std::hex << vaddr << std::dec << "\n";
            return 0;
        }
        std::uint64_t val = 0;
        std::memcpy(&val, reinterpret_cast<const void*>(vaddr), size);
        return val;
    }

    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr < 0x1000) {
            std::cerr << "[CRASH GUARD] MemoryRead128 from low vaddr 0x"
                      << std::hex << vaddr << std::dec << "\n";
            return Dynarmic::A64::Vector{};
        }
        Dynarmic::A64::Vector val{};
        std::memcpy(val.data(), reinterpret_cast<const void*>(vaddr), 16);
        return val;
    }

    void MemoryWrite(Dynarmic::A64::VAddr vaddr, std::uint64_t value, std::size_t size) override {
        if (vaddr < 0x1000) {
            std::cerr << "[CRASH GUARD] MemoryWrite of " << size << " bytes to low vaddr 0x"
                      << std::hex << vaddr << " (val: 0x" << value << ")" << std::dec << "\n";
            return;
        }
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, size);
    }

    void MemoryWrite128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value) override {
        if (vaddr < 0x1000) {
            std::cerr << "[CRASH GUARD] MemoryWrite128 to low vaddr 0x"
                      << std::hex << vaddr << std::dec << "\n";
            return;
        }
        std::memcpy(reinterpret_cast<void*>(vaddr), value.data(), 16);
    }

    bool MemoryWriteExclusive8(Dynarmic::A64::VAddr vaddr, std::uint8_t value, std::uint8_t expected) override {
        auto* ptr = reinterpret_cast<uint8_t*>(vaddr);
        uint8_t exp = expected;
        return std::atomic_ref<uint8_t>(*ptr).compare_exchange_strong(exp, value);
    }

    bool MemoryWriteExclusive16(Dynarmic::A64::VAddr vaddr, std::uint16_t value, std::uint16_t expected) override {
        auto* ptr = reinterpret_cast<uint16_t*>(vaddr);
        uint16_t exp = expected;
        return std::atomic_ref<uint16_t>(*ptr).compare_exchange_strong(exp, value);
    }

    bool MemoryWriteExclusive32(Dynarmic::A64::VAddr vaddr, std::uint32_t value, std::uint32_t expected) override {
        auto* ptr = reinterpret_cast<uint32_t*>(vaddr);
        uint32_t exp = expected;
        return std::atomic_ref<uint32_t>(*ptr).compare_exchange_strong(exp, value);
    }

    bool MemoryWriteExclusive64(Dynarmic::A64::VAddr vaddr, std::uint64_t value, std::uint64_t expected) override {
        auto* ptr = reinterpret_cast<uint64_t*>(vaddr);
        uint64_t exp = expected;
        return std::atomic_ref<uint64_t>(*ptr).compare_exchange_strong(exp, value);
    }

    bool MemoryWriteExclusive128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) override {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(vaddr);
        if (ptr[0] == expected[0] && ptr[1] == expected[1]) {
            ptr[0] = value[0];
            ptr[1] = value[1];
            return true;
        }
        return false;
    }

    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(Dynarmic::A64::VAddr pc, Dynarmic::A64::Exception exception) override;

    void AddTicks(std::uint64_t ticks) override {
        m_ticks += ticks;
    }

    std::uint64_t GetTicksRemaining() override {
        return 1000000;
    }

    std::uint64_t GetCNTPCT() override {
        return static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        );
    }

private:
    DynarmicCore::Impl* m_parent;
    std::uint64_t m_ticks = 0;
};

struct DynarmicCore::Impl {
    CoreCallbacks callbacks{this};
    Dynarmic::A64::UserConfig config;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::unordered_map<uint32_t, SVCHandler> svc_handlers;

    uint64_t tpidr_el0 = 0;
    uint64_t tpidrro_el0 = 0;
    bool in_call = false;
    bool call_returned = false;
    void* stack_mem = nullptr;
    void* tls_mem = nullptr;
    void* return_trampoline = nullptr;
    uint64_t stack_top = GUEST_STACK_TOP;
    size_t stack_size = 0;
    bool owns_dynamic_mem = false;
    int processor_id = -1;

    ~Impl() {
        jit.reset();
        if (processor_id >= 0) {
            shared_monitor().ClearProcessor(static_cast<size_t>(processor_id));
            release_processor_id(processor_id);
        }
        if (owns_dynamic_mem) {
#if defined(LCS_PLATFORM_WINDOWS)
            if (stack_mem) VirtualFree(stack_mem, 0, MEM_RELEASE);
            if (tls_mem) VirtualFree(tls_mem, 0, MEM_RELEASE);
#else
            if (stack_mem) munmap(stack_mem, stack_size);
            if (tls_mem) munmap(tls_mem, 0x10000);
#endif
        }
    }

    // One exclusive monitor shared by every core so LDXR/STXR pairs stay coherent
    // across guest threads. Each core owns a distinct processor slot.
    // Dynarmic's internal static_vector has capacity MAX_NUM_CPU_CORES = 6.
    static constexpr size_t MAX_PROCESSORS = 6;

    static Dynarmic::ExclusiveMonitor& shared_monitor() {
        static Dynarmic::ExclusiveMonitor s_monitor(MAX_PROCESSORS);
        return s_monitor;
    }

    static std::mutex& processor_mutex() {
        static std::mutex s_mutex;
        return s_mutex;
    }

    static std::vector<bool>& processor_slots() {
        static std::vector<bool> s_slots(MAX_PROCESSORS, false);
        return s_slots;
    }

    static int acquire_processor_id() {
        std::lock_guard<std::mutex> lock(processor_mutex());
        auto& slots = processor_slots();
        for (size_t i = 0; i < slots.size(); i++) {
            if (!slots[i]) {
                slots[i] = true;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    static void release_processor_id(int id) {
        std::lock_guard<std::mutex> lock(processor_mutex());
        auto& slots = processor_slots();
        if (id >= 0 && static_cast<size_t>(id) < slots.size()) slots[static_cast<size_t>(id)] = false;
    }

    static SVCHandler& fallback_handler() {
        static SVCHandler s_handler;
        return s_handler;
    }

    void setup_thread_memory(size_t size) {
        size = (size + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
        stack_size = size;
        owns_dynamic_mem = true;
#if defined(LCS_PLATFORM_WINDOWS)
        stack_mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        tls_mem = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        stack_mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack_mem == MAP_FAILED) stack_mem = nullptr;
        tls_mem = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tls_mem == MAP_FAILED) tls_mem = nullptr;
#endif
        if (tls_mem) {
            std::memset(tls_mem, 0, 0x10000);
            tpidr_el0 = reinterpret_cast<uint64_t>(tls_mem);
            tpidrro_el0 = reinterpret_cast<uint64_t>(tls_mem);
            uint8_t* tls = reinterpret_cast<uint8_t*>(tls_mem);
            *reinterpret_cast<uint64_t*>(tls + 0) = reinterpret_cast<uint64_t>(tls);
            *reinterpret_cast<uint64_t*>(tls + 0x130) = 0x7f60301000ULL; // JNI_GUEST_ENV_ADDR
        }
        if (stack_mem) {
            stack_top = (reinterpret_cast<uint64_t>(stack_mem) + size - 0x100) & ~15ULL;
        }
    }

    void setup_memory() {
#if defined(LCS_PLATFORM_WINDOWS)
        // Allocate guest stack (2 MB)
        stack_mem = VirtualAlloc(reinterpret_cast<void*>(0x7f60000000ULL), 0x200000,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

        // Allocate thread-local storage (TLS) block (64 KB)
        tls_mem = VirtualAlloc(reinterpret_cast<void*>(0x7f60200000ULL), 0x10000,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (tls_mem) {
            std::memset(tls_mem, 0, 0x10000);
            tpidr_el0 = reinterpret_cast<uint64_t>(tls_mem);
            tpidrro_el0 = reinterpret_cast<uint64_t>(tls_mem);
            uint8_t* tls = reinterpret_cast<uint8_t*>(tls_mem);
            *reinterpret_cast<uint64_t*>(tls + 0) = reinterpret_cast<uint64_t>(tls);
            *reinterpret_cast<uint64_t*>(tls + 0x130) = 0x7f60301000ULL; // JNI_GUEST_ENV_ADDR
        }

        // Allocate return trampoline page
        return_trampoline = VirtualAlloc(reinterpret_cast<void*>(MAGIC_RETURN_PC), 0x1000,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
        stack_mem = mmap(reinterpret_cast<void*>(0x7f60000000ULL), 0x200000,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        tls_mem = mmap(reinterpret_cast<void*>(0x7f60200000ULL), 0x10000,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (tls_mem != MAP_FAILED) {
            std::memset(tls_mem, 0, 0x10000);
            tpidr_el0 = reinterpret_cast<uint64_t>(tls_mem);
            tpidrro_el0 = reinterpret_cast<uint64_t>(tls_mem);
        }
        return_trampoline = mmap(reinterpret_cast<void*>(MAGIC_RETURN_PC), 0x1000,
                                 PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif

        if (return_trampoline) {
            // Write ARM64 instruction: SVC #0xffff
            // ARM64 SVC #imm16: 11010100 000 imm16 000 01
            // For imm16 = 0xffff: 0xd41fffe1
            uint32_t* code = reinterpret_cast<uint32_t*>(return_trampoline);
            code[0] = 0xd4000001 | (RETURN_SVC_ID << 5); // SVC #RETURN_SVC_ID
            code[1] = 0xd65f03c0;                         // RET
        }
    }

    void handle_svc(uint32_t swi) {
        if (swi == RETURN_SVC_ID) {
            call_returned = true;
            jit->HaltExecution();
            return;
        }

        auto it = svc_handlers.find(swi);
        const SVCHandler* handler = nullptr;
        if (it != svc_handlers.end()) {
            handler = &it->second;
        } else if (fallback_handler()) {
            handler = &fallback_handler();
        }
        if (handler) {
            uint64_t args[8];
            for (size_t i = 0; i < 8; i++) {
                args[i] = jit->GetRegister(i);
            }
            uint64_t ret = (*handler)(swi, args);
            jit->SetRegister(0, ret);
        } else {
            std::cerr << "[CPU] Unhandled SVC: 0x" << std::hex << swi << std::dec << "\n";
        }
    }
};

void CoreCallbacks::CallSVC(std::uint32_t swi) {
    m_parent->handle_svc(swi);
}

void CoreCallbacks::ExceptionRaised(Dynarmic::A64::VAddr pc, Dynarmic::A64::Exception exception) {
    std::cerr << "[CPU Exception] PC: 0x" << std::hex << pc
              << ", Exception ID: " << static_cast<int>(exception) << std::dec << "\n";
    m_parent->call_returned = true;
    if (m_parent->jit) {
        m_parent->jit->HaltExecution();
    }
}

DynarmicCore::DynarmicCore() : m_impl(std::make_unique<Impl>()) {}
DynarmicCore::~DynarmicCore() = default;

namespace {
thread_local DynarmicCore* t_current_core = nullptr;

// Marks the core executing guest code on this host thread for the duration of a call.
struct CurrentCoreScope {
    DynarmicCore* previous;
    explicit CurrentCoreScope(DynarmicCore* core) : previous(t_current_core) { t_current_core = core; }
    ~CurrentCoreScope() { t_current_core = previous; }
    CurrentCoreScope(const CurrentCoreScope&) = delete;
    CurrentCoreScope& operator=(const CurrentCoreScope&) = delete;
};
} // namespace

DynarmicCore* DynarmicCore::current() {
    return t_current_core;
}

void DynarmicCore::set_fallback_svc_handler(SVCHandler handler) {
    Impl::fallback_handler() = std::move(handler);
}

void DynarmicCore::request_exit() {
    m_impl->call_returned = true;
    if (m_impl->jit) m_impl->jit->HaltExecution();
}

static bool finish_jit_setup(DynarmicCore::Impl& impl) {
    impl.processor_id = DynarmicCore::Impl::acquire_processor_id();
    if (impl.processor_id < 0) {
        std::cerr << "[CPU] No free exclusive monitor processor slot (max "
                  << DynarmicCore::Impl::MAX_PROCESSORS << " guest threads).\n";
        return false;
    }
    impl.config.global_monitor = &DynarmicCore::Impl::shared_monitor();
    impl.config.processor_id = static_cast<std::uint8_t>(impl.processor_id);
    impl.config.callbacks = &impl.callbacks;
    impl.config.tpidr_el0 = &impl.tpidr_el0;
    impl.config.tpidrro_el0 = &impl.tpidrro_el0;

    impl.jit = std::make_unique<Dynarmic::A64::Jit>(impl.config);
    return impl.jit != nullptr;
}

bool DynarmicCore::initialize() {
    m_impl->setup_memory();
    return finish_jit_setup(*m_impl);
}

bool DynarmicCore::initialize_thread(size_t stack_size) {
    m_impl->setup_thread_memory(stack_size);
    if (!m_impl->stack_mem || !m_impl->tls_mem) {
        std::cerr << "[CPU] Failed to allocate guest thread stack/TLS.\n";
        return false;
    }
    return finish_jit_setup(*m_impl);
}

void DynarmicCore::set_reg(size_t index, uint64_t value) {
    if (m_impl->jit && index < 31) {
        m_impl->jit->SetRegister(index, value);
    }
}

uint64_t DynarmicCore::get_reg(size_t index) const {
    return (m_impl->jit && index < 31) ? m_impl->jit->GetRegister(index) : 0;
}

void DynarmicCore::set_pc(uint64_t pc) {
    if (m_impl->jit) m_impl->jit->SetPC(pc);
}

uint64_t DynarmicCore::get_pc() const {
    return m_impl->jit ? m_impl->jit->GetPC() : 0;
}

void DynarmicCore::set_sp(uint64_t sp) {
    if (m_impl->jit) m_impl->jit->SetSP(sp);
}

uint64_t DynarmicCore::get_sp() const {
    return m_impl->jit ? m_impl->jit->GetSP() : 0;
}

void DynarmicCore::set_fp_float(size_t index, float value) {
    if (!m_impl->jit) return;
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    m_impl->jit->SetVector(index, {raw, 0});
}

float DynarmicCore::get_fp_float(size_t index) const {
    if (!m_impl->jit) return 0.0f;
    auto vec = m_impl->jit->GetVector(index);
    uint32_t raw = static_cast<uint32_t>(vec[0]);
    float f = 0.0f;
    std::memcpy(&f, &raw, sizeof(f));
    return f;
}

void DynarmicCore::set_fp_double(size_t index, double value) {
    if (!m_impl->jit) return;
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    m_impl->jit->SetVector(index, {raw, 0});
}

double DynarmicCore::get_fp_double(size_t index) const {
    if (!m_impl->jit) return 0.0;
    auto vec = m_impl->jit->GetVector(index);
    double d = 0.0;
    std::memcpy(&d, &vec[0], sizeof(d));
    return d;
}

void DynarmicCore::set_tpidr_el0(uint64_t tls_addr) {
    m_impl->tpidr_el0 = tls_addr;
}

uint64_t DynarmicCore::get_tpidr_el0() const {
    return m_impl->tpidr_el0;
}

void DynarmicCore::run() {
    if (m_impl->jit) {
        m_impl->jit->Run();
    }
}

void DynarmicCore::step() {
    if (m_impl->jit) {
        m_impl->jit->Step();
    }
}

void DynarmicCore::halt() {
    if (m_impl->jit) {
        m_impl->jit->HaltExecution();
    }
}

void DynarmicCore::clear_cache() {
    if (m_impl->jit) {
        m_impl->jit->ClearCache();
    }
}

void DynarmicCore::invalidate_cache(uint64_t vaddr, size_t size) {
    if (m_impl->jit) {
        m_impl->jit->InvalidateCacheRange(vaddr, size);
    }
}

uint64_t DynarmicCore::call(uint64_t address, const std::vector<uint64_t>& args) {
    if (!m_impl->jit) return 0;
    CurrentCoreScope scope(this);

    // Setup arguments in registers X0..X7
    for (size_t i = 0; i < args.size() && i < 8; i++) {
        m_impl->jit->SetRegister(i, args[i]);
    }

    // Set return address LR (X30) to our return sentinel
    m_impl->jit->SetRegister(30, MAGIC_RETURN_PC);
    m_impl->jit->SetSP(m_impl->stack_top);
    m_impl->jit->SetPC(address);

    m_impl->call_returned = false;
    while (!m_impl->call_returned) {
        m_impl->jit->Run();
    }

    return m_impl->jit->GetRegister(0);
}

uint64_t DynarmicCore::call_nested(uint64_t address, const std::vector<uint64_t>& args) {
    if (!m_impl->jit) return 0;
    CurrentCoreScope scope(this);

    std::array<uint64_t, 31> saved_regs;
    for (size_t i = 0; i < 31; i++) {
        saved_regs[i] = m_impl->jit->GetRegister(i);
    }
    uint64_t saved_sp = m_impl->jit->GetSP();
    uint64_t saved_pc = m_impl->jit->GetPC();
    bool saved_call_returned = m_impl->call_returned;

    // Use current stack, reserving 64 bytes for nested frame
    uint64_t nested_sp = (saved_sp - 64) & ~15ULL;
    m_impl->jit->SetSP(nested_sp);

    for (size_t i = 0; i < args.size() && i < 8; i++) {
        m_impl->jit->SetRegister(i, args[i]);
    }
    m_impl->jit->SetRegister(30, MAGIC_RETURN_PC);
    m_impl->jit->SetPC(address);

    m_impl->call_returned = false;
    while (!m_impl->call_returned) {
        m_impl->jit->Run();
    }
    uint64_t result = m_impl->jit->GetRegister(0);

    for (size_t i = 0; i < 31; i++) {
        m_impl->jit->SetRegister(i, saved_regs[i]);
    }
    m_impl->jit->SetSP(saved_sp);
    m_impl->jit->SetPC(saved_pc);
    m_impl->call_returned = saved_call_returned;

    return result;
}

void DynarmicCore::register_svc_handler(uint32_t swi, SVCHandler handler) {
    m_impl->svc_handlers[swi] = std::move(handler);
}

} // namespace lcs::cpu
