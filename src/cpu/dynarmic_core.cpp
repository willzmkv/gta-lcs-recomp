#include "dynarmic_core.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <unordered_map>
#include <atomic>

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
        std::uint64_t val = 0;
        std::memcpy(&val, reinterpret_cast<const void*>(vaddr), size);
        return val;
    }

    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr vaddr) override {
        Dynarmic::A64::Vector val{};
        std::memcpy(val.data(), reinterpret_cast<const void*>(vaddr), 16);
        return val;
    }

    void MemoryWrite(Dynarmic::A64::VAddr vaddr, std::uint64_t value, std::size_t size) override {
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, size);
    }

    void MemoryWrite128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value) override {
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
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::unordered_map<uint32_t, SVCHandler> svc_handlers;

    uint64_t tpidr_el0 = 0;
    uint64_t tpidrro_el0 = 0;
    bool in_call = false;
    bool call_returned = false;
    void* stack_mem = nullptr;
    void* return_trampoline = nullptr;

    void setup_memory() {
#if defined(LCS_PLATFORM_WINDOWS)
        // Allocate guest stack (2 MB)
        stack_mem = VirtualAlloc(reinterpret_cast<void*>(0x7f60000000ULL), 0x200000,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

        // Allocate thread-local storage (TLS) block (64 KB)
        void* tls_mem = VirtualAlloc(reinterpret_cast<void*>(0x7f60200000ULL), 0x10000,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (tls_mem) {
            std::memset(tls_mem, 0, 0x10000);
            tpidr_el0 = reinterpret_cast<uint64_t>(tls_mem);
            tpidrro_el0 = reinterpret_cast<uint64_t>(tls_mem);
        }

        // Allocate return trampoline page
        return_trampoline = VirtualAlloc(reinterpret_cast<void*>(MAGIC_RETURN_PC), 0x1000,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
        stack_mem = mmap(reinterpret_cast<void*>(0x7f60000000ULL), 0x200000,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        void* tls_mem = mmap(reinterpret_cast<void*>(0x7f60200000ULL), 0x10000,
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
        if (it != svc_handlers.end()) {
            uint64_t args[8];
            for (size_t i = 0; i < 8; i++) {
                args[i] = jit->GetRegister(i);
            }
            uint64_t ret = it->second(swi, args);
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

bool DynarmicCore::initialize() {
    m_impl->setup_memory();

    m_impl->monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(1);
    m_impl->config.global_monitor = m_impl->monitor.get();
    m_impl->config.callbacks = &m_impl->callbacks;
    m_impl->config.tpidr_el0 = &m_impl->tpidr_el0;
    m_impl->config.tpidrro_el0 = &m_impl->tpidrro_el0;

    m_impl->jit = std::make_unique<Dynarmic::A64::Jit>(m_impl->config);
    return m_impl->jit != nullptr;
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

    // Setup arguments in registers X0..X7
    for (size_t i = 0; i < args.size() && i < 8; i++) {
        m_impl->jit->SetRegister(i, args[i]);
    }

    // Set return address LR (X30) to our return sentinel
    m_impl->jit->SetRegister(30, MAGIC_RETURN_PC);
    m_impl->jit->SetSP(GUEST_STACK_TOP);
    m_impl->jit->SetPC(address);

    m_impl->call_returned = false;
    while (!m_impl->call_returned) {
        m_impl->jit->Run();
    }

    return m_impl->jit->GetRegister(0);
}

uint64_t DynarmicCore::call_nested(uint64_t address, const std::vector<uint64_t>& args) {
    if (!m_impl->jit) return 0;

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
