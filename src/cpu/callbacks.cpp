#include "callbacks.h"
#include "dynarmic_core.h"
#include "graphics/render_bridge.h"
#include "os/bionic_libc.h"
#include "os/thread_manager.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

struct Arm64VaList {
    uint8_t* stack;
    uint8_t* gr_top;
    uint8_t* vr_top;
    int32_t  gr_offs;
    int32_t  vr_offs;

    uint64_t next_gpr() {
        if (gr_offs < 0 && gr_top) {
            uint64_t val = *reinterpret_cast<const uint64_t*>(gr_top + gr_offs);
            gr_offs += 8;
            return val;
        } else if (stack) {
            uint64_t val = *reinterpret_cast<const uint64_t*>(stack);
            stack += 8;
            return val;
        }
        return 0;
    }
};

static int format_arm64_vsnprintf(char* s, size_t maxlen, const char* fmt, uint64_t ap_addr) {
    if (!s || maxlen == 0) return 0;
    if (!fmt) { s[0] = '\0'; return 0; }
    if (!ap_addr) { return std::snprintf(s, maxlen, "%s", fmt); }

    auto* ap = reinterpret_cast<Arm64VaList*>(ap_addr);
    Arm64VaList va = *ap;

    uint64_t a0 = va.next_gpr();
    uint64_t a1 = va.next_gpr();
    uint64_t a2 = va.next_gpr();
    uint64_t a3 = va.next_gpr();
    uint64_t a4 = va.next_gpr();
    uint64_t a5 = va.next_gpr();
    uint64_t a6 = va.next_gpr();
    uint64_t a7 = va.next_gpr();

    return std::snprintf(s, maxlen, fmt, a0, a1, a2, a3, a4, a5, a6, a7);
}

namespace lcs::cpu {

HLEManager& HLEManager::instance() {
    static HLEManager s_instance;
    return s_instance;
}

HLEManager::HLEManager() = default;

HLEManager::~HLEManager() {
    if (m_trampoline_mem) {
#if defined(LCS_PLATFORM_WINDOWS)
        VirtualFree(m_trampoline_mem, 0, MEM_RELEASE);
#else
        munmap(m_trampoline_mem, MAX_TRAMPOLINES * 8);
#endif
        m_trampoline_mem = nullptr;
    }
}

bool HLEManager::initialize() {
    if (m_trampoline_mem) return true;

#if defined(LCS_PLATFORM_WINDOWS)
    m_trampoline_mem = VirtualAlloc(reinterpret_cast<void*>(TRAMPOLINE_BASE),
                                    MAX_TRAMPOLINES * 8,
                                    MEM_RESERVE | MEM_COMMIT,
                                    PAGE_EXECUTE_READWRITE);
#else
    m_trampoline_mem = mmap(reinterpret_cast<void*>(TRAMPOLINE_BASE),
                            MAX_TRAMPOLINES * 8,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif

    if (!m_trampoline_mem) {
        std::cerr << "[HLE] Failed to allocate trampoline table at 0x"
                  << std::hex << TRAMPOLINE_BASE << std::dec << "\n";
        return false;
    }

    DynarmicCore::set_fallback_svc_handler([](uint32_t swi, const uint64_t* args) -> uint64_t {
        auto& hle = HLEManager::instance();
        if (swi < hle.handler_count()) {
            const auto& fn = hle.get_handler(swi);
            if (fn) return fn(args);
        }
        return 0;
    });

    register_default_stubs();
    lcs::os::register_thread_hle(*this);
    return true;
}

DynarmicCore* HLEManager::get_active_cpu() const {
    DynarmicCore* cur = DynarmicCore::current();
    return cur ? cur : m_cpu;
}

uint64_t HLEManager::allocate_trampoline(uint32_t id) {
    if (id >= MAX_TRAMPOLINES) return 0;

    uint32_t* slot = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uintptr_t>(m_trampoline_mem) + (id * 8)
    );

    // ARM64 instructions:
    // SVC #id (0xd4000001 | (id << 5))
    // RET     (0xd65f03c0)
    slot[0] = 0xd4000001 | ((id & 0xffff) << 5);
    slot[1] = 0xd65f03c0;

    return TRAMPOLINE_BASE + (id * 8);
}

uint64_t HLEManager::register_function(const std::string& name, HLEFunction handler) {
    auto it = m_symbol_to_id.find(name);
    if (it != m_symbol_to_id.end()) {
        uint32_t id = it->second;
        m_handlers[id] = std::move(handler);
        return TRAMPOLINE_BASE + (id * 8);
    }

    uint32_t id = static_cast<uint32_t>(m_handlers.size());
    if (id >= MAX_TRAMPOLINES) {
        std::cerr << "[HLE] Trampoline table full!\n";
        return 0;
    }

    m_symbol_to_id[name] = id;
    m_id_to_name.push_back(name);
    m_handlers.push_back(std::move(handler));

    if (m_cpu) {
        m_cpu->register_svc_handler(id, [this](uint32_t swi, const uint64_t* args) -> uint64_t {
            if (swi < m_handlers.size() && m_handlers[swi]) {
                return m_handlers[swi](args);
            }
            return 0;
        });
    }

    return allocate_trampoline(id);
}

uint64_t HLEManager::resolve(const char* name) {
    if (!name || *name == '\0') return 0;

    uint64_t bionic_data = lcs::os::resolve_bionic_libc_data(name);
    if (bionic_data != 0) {
        return bionic_data;
    }

    std::string s(name);
    auto it = m_symbol_to_id.find(s);
    if (it != m_symbol_to_id.end()) {
        return TRAMPOLINE_BASE + (it->second * 8);
    }

    // Auto-stub unknown symbol (log each unique symbol once)
    return register_function(s, [s](const uint64_t* args) -> uint64_t {
        static std::unordered_map<std::string, bool> s_logged_stubs;
        static std::mutex s_stub_mutex;
        bool log_now = false;
        {
            std::lock_guard<std::mutex> lock(s_stub_mutex);
            if (!s_logged_stubs[s]) {
                s_logged_stubs[s] = true;
                log_now = true;
            }
        }
        if (log_now) {
            std::cout << "[HLE STUB] Called unimplemented (once): " << s
                      << "(0x" << std::hex << args[0] << ", 0x" << args[1]
                      << ", 0x" << args[2] << ", 0x" << args[3] << std::dec << ")\n";
        }
        return 0;
    });
}

void HLEManager::wire_to_cpu(DynarmicCore& cpu) {
    if (!m_cpu) m_cpu = &cpu;
    for (size_t i = 0; i < m_handlers.size(); i++) {
        uint32_t id = static_cast<uint32_t>(i);
        cpu.register_svc_handler(id, [this](uint32_t swi, const uint64_t* args) -> uint64_t {
            if (swi < m_handlers.size() && m_handlers[swi]) {
                return m_handlers[swi](args);
            }
            return 0;
        });
    }
}

void HLEManager::register_default_stubs() {
    // C++ ABI
    register_function("__cxa_atexit", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0;
    });

    register_function("__cxa_finalize", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0;
    });

    register_function("__cxa_guard_acquire", [](const uint64_t* args) -> uint64_t {
        uint8_t* guard = reinterpret_cast<uint8_t*>(args[0]);
        if (*guard == 0) {
            *guard = 1;
            return 1; // Needs init
        }
        return 0; // Already initialized
    });

    register_function("__cxa_guard_release", [](const uint64_t* args) -> uint64_t {
        uint8_t* guard = reinterpret_cast<uint8_t*>(args[0]);
        *guard = 2; // Init complete
        return 0;
    });

    register_function("__cxa_guard_abort", [](const uint64_t* args) -> uint64_t {
        uint8_t* guard = reinterpret_cast<uint8_t*>(args[0]);
        *guard = 0;
        return 0;
    });

    // C++ operator new and delete
    register_function("_Znwm", [](const uint64_t* args) -> uint64_t {
        void* ptr = std::malloc(args[0]);
        std::cout << "[HLE] _Znwm(" << args[0] << ") -> " << ptr << "\n";
        return reinterpret_cast<uint64_t>(ptr);
    });
    register_function("_Znam", [](const uint64_t* args) -> uint64_t {
        void* ptr = std::malloc(args[0]);
        std::cout << "[HLE] _Znam(" << args[0] << ") -> " << ptr << "\n";
        return reinterpret_cast<uint64_t>(ptr);
    });
    register_function("_ZdlPv", [](const uint64_t* args) -> uint64_t {
        std::free(reinterpret_cast<void*>(args[0]));
        return 0;
    });
    register_function("_ZdaPv", [](const uint64_t* args) -> uint64_t {
        std::free(reinterpret_cast<void*>(args[0]));
        return 0;
    });
    register_function("_ZdlPvm", [](const uint64_t* args) -> uint64_t {
        std::free(reinterpret_cast<void*>(args[0]));
        return 0;
    });
    register_function("_ZdaPvm", [](const uint64_t* args) -> uint64_t {
        std::free(reinterpret_cast<void*>(args[0]));
        return 0;
    });

    // Pthreads
    register_function("pthread_mutex_init", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_mutex_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_mutex_lock", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_mutex_trylock", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_mutex_unlock", [](const uint64_t*) -> uint64_t { return 0; });

    register_function("pthread_cond_init", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_cond_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_cond_signal", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_cond_broadcast", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_cond_wait", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_cond_timedwait", [](const uint64_t*) -> uint64_t { return 0; });

    register_function("pthread_once", [this](const uint64_t* args) -> uint64_t {
        int* once_control = reinterpret_cast<int*>(args[0]);
        uint64_t init_routine = args[1];
        if (once_control && *once_control == 0) {
            *once_control = 1;
            auto* cpu = get_active_cpu();
            if (init_routine && cpu) {
                std::cout << "[HLE] pthread_once invoking guest callback 0x" << std::hex << init_routine << std::dec << "...\n";
                cpu->call_nested(init_routine);
                std::cout << "[HLE] pthread_once guest callback completed.\n";
            }
        }
        return 0;
    });

    register_function("pthread_self", [](const uint64_t*) -> uint64_t { return 1; });
    register_function("pthread_equal", [](const uint64_t* args) -> uint64_t { return args[0] == args[1]; });
    // Pthread TLS (Thread Local Storage)
    static std::unordered_map<uint32_t, uint64_t> s_tls_keys;
    static uint32_t s_next_tls_key = 1;

    register_function("pthread_key_create", [](const uint64_t* args) -> uint64_t {
        uint32_t* key_out = reinterpret_cast<uint32_t*>(args[0]);
        if (key_out) {
            *key_out = s_next_tls_key++;
            s_tls_keys[*key_out] = 0;
        }
        return 0;
    });
    register_function("pthread_key_delete", [](const uint64_t* args) -> uint64_t {
        uint32_t key = static_cast<uint32_t>(args[0]);
        s_tls_keys.erase(key);
        return 0;
    });
    register_function("pthread_setspecific", [](const uint64_t* args) -> uint64_t {
        uint32_t key = static_cast<uint32_t>(args[0]);
        uint64_t val = args[1];
        s_tls_keys[key] = val;
        return 0;
    });
    register_function("pthread_getspecific", [](const uint64_t* args) -> uint64_t {
        uint32_t key = static_cast<uint32_t>(args[0]);
        auto it = s_tls_keys.find(key);
        return it != s_tls_keys.end() ? it->second : 0;
    });

    // Memory management with heap corruption guard
    static std::unordered_map<void*, void*> s_allocated_blocks; // aligned_ptr -> raw_ptr
    static std::mutex s_mem_mutex;

    auto record_alloc = [](void* aligned_ptr, void* raw_ptr) {
        if (!aligned_ptr) return;
        std::lock_guard<std::mutex> lock(s_mem_mutex);
        s_allocated_blocks[aligned_ptr] = raw_ptr;
    };

    auto remove_alloc = [](void* ptr) -> void* {
        if (!ptr) return nullptr;
        std::lock_guard<std::mutex> lock(s_mem_mutex);
        auto it = s_allocated_blocks.find(ptr);
        if (it != s_allocated_blocks.end()) {
            void* raw = it->second;
            s_allocated_blocks.erase(it);
            return raw;
        }
        return nullptr;
    };

    register_function("malloc", [record_alloc](const uint64_t* args) -> uint64_t {
        size_t size = static_cast<size_t>(args[0]);
        void* ptr = std::malloc(size ? size : 1);
        record_alloc(ptr, ptr);
        return reinterpret_cast<uint64_t>(ptr);
    });

    register_function("free", [remove_alloc](const uint64_t* args) -> uint64_t {
        void* ptr = reinterpret_cast<void*>(args[0]);
        void* raw = remove_alloc(ptr);
        if (raw) {
            std::free(raw);
        }
        return 0;
    });

    register_function("calloc", [record_alloc](const uint64_t* args) -> uint64_t {
        size_t nmemb = static_cast<size_t>(args[0]);
        size_t size = static_cast<size_t>(args[1]);
        void* ptr = std::calloc(nmemb ? nmemb : 1, size ? size : 1);
        record_alloc(ptr, ptr);
        return reinterpret_cast<uint64_t>(ptr);
    });

    register_function("realloc", [record_alloc, remove_alloc](const uint64_t* args) -> uint64_t {
        void* ptr = reinterpret_cast<void*>(args[0]);
        size_t size = static_cast<size_t>(args[1]);
        void* old_raw = remove_alloc(ptr);
        void* new_ptr = std::realloc(old_raw ? old_raw : ptr, size ? size : 1);
        record_alloc(new_ptr, new_ptr);
        return reinterpret_cast<uint64_t>(new_ptr);
    });

    register_function("posix_memalign", [record_alloc](const uint64_t* args) -> uint64_t {
        void** memptr = reinterpret_cast<void**>(args[0]);
        size_t alignment = static_cast<size_t>(args[1]);
        size_t size = static_cast<size_t>(args[2]);
        if (!memptr) return 22; // EINVAL
        if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) return 22;

        if (alignment <= alignof(std::max_align_t)) {
            void* ptr = std::malloc(size ? size : 1);
            if (!ptr) return 12; // ENOMEM
            record_alloc(ptr, ptr);
            *memptr = ptr;
            return 0;
        }

        size_t total = size + alignment + sizeof(void*);
        void* raw = std::malloc(total);
        if (!raw) return 12; // ENOMEM
        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
        uintptr_t aligned_addr = (raw_addr + sizeof(void*) + (alignment - 1)) & ~(alignment - 1);
        void* aligned_ptr = reinterpret_cast<void*>(aligned_addr);
        record_alloc(aligned_ptr, raw);
        *memptr = aligned_ptr;
        return 0;
    });

    register_function("memalign", [record_alloc](const uint64_t* args) -> uint64_t {
        size_t alignment = static_cast<size_t>(args[0]);
        size_t size = static_cast<size_t>(args[1]);
        if (alignment <= alignof(std::max_align_t)) {
            void* ptr = std::malloc(size ? size : 1);
            record_alloc(ptr, ptr);
            return reinterpret_cast<uint64_t>(ptr);
        }
        size_t total = size + alignment + sizeof(void*);
        void* raw = std::malloc(total);
        if (!raw) return 0;
        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
        uintptr_t aligned_addr = (raw_addr + sizeof(void*) + (alignment - 1)) & ~(alignment - 1);
        void* aligned_ptr = reinterpret_cast<void*>(aligned_addr);
        record_alloc(aligned_ptr, raw);
        return static_cast<uint64_t>(aligned_addr);
    });

    // Memory operations
    register_function("memcpy", [](const uint64_t* args) -> uint64_t {
        std::memcpy(reinterpret_cast<void*>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
        return args[0];
    });
    register_function("__memcpy_chk", [](const uint64_t* args) -> uint64_t {
        std::memcpy(reinterpret_cast<void*>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
        return args[0];
    });

    register_function("memset", [](const uint64_t* args) -> uint64_t {
        std::memset(reinterpret_cast<void*>(args[0]), static_cast<int>(args[1]), args[2]);
        return args[0];
    });
    register_function("__memset_chk", [](const uint64_t* args) -> uint64_t {
        std::memset(reinterpret_cast<void*>(args[0]), static_cast<int>(args[1]), args[2]);
        return args[0];
    });

    register_function("memmove", [](const uint64_t* args) -> uint64_t {
        std::memmove(reinterpret_cast<void*>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
        return args[0];
    });
    register_function("__memmove_chk", [](const uint64_t* args) -> uint64_t {
        std::memmove(reinterpret_cast<void*>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
        return args[0];
    });

    register_function("memcmp", [](const uint64_t* args) -> uint64_t {
        return std::memcmp(reinterpret_cast<const void*>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
    });

    // String operations
    register_function("strlen", [](const uint64_t* args) -> uint64_t {
        return args[0] ? std::strlen(reinterpret_cast<const char*>(args[0])) : 0;
    });
    register_function("__strlen_chk", [](const uint64_t* args) -> uint64_t {
        return args[0] ? std::strlen(reinterpret_cast<const char*>(args[0])) : 0;
    });

    register_function("strcmp", [](const uint64_t* args) -> uint64_t {
        const char* s1 = reinterpret_cast<const char*>(args[0]);
        const char* s2 = reinterpret_cast<const char*>(args[1]);
        if (!s1 && !s2) return 0;
        if (!s1) return static_cast<uint64_t>(-1);
        if (!s2) return 1;
        return static_cast<uint64_t>(std::strcmp(s1, s2));
    });

    register_function("strncmp", [](const uint64_t* args) -> uint64_t {
        const char* s1 = reinterpret_cast<const char*>(args[0]);
        const char* s2 = reinterpret_cast<const char*>(args[1]);
        size_t n = args[2];
        if (n == 0) return 0;
        if (!s1 && !s2) return 0;
        if (!s1) return static_cast<uint64_t>(-1);
        if (!s2) return 1;
        return static_cast<uint64_t>(std::strncmp(s1, s2, n));
    });

    register_function("strcasecmp", [](const uint64_t* args) -> uint64_t {
        const char* s1 = reinterpret_cast<const char*>(args[0]);
        const char* s2 = reinterpret_cast<const char*>(args[1]);
        if (!s1 && !s2) return 0;
        if (!s1) return static_cast<uint64_t>(-1);
        if (!s2) return 1;
#if defined(LCS_PLATFORM_WINDOWS)
        return static_cast<uint64_t>(_stricmp(s1, s2));
#else
        return static_cast<uint64_t>(strcasecmp(s1, s2));
#endif
    });

    register_function("strncasecmp", [](const uint64_t* args) -> uint64_t {
        const char* s1 = reinterpret_cast<const char*>(args[0]);
        const char* s2 = reinterpret_cast<const char*>(args[1]);
        size_t n = args[2];
        if (n == 0) return 0;
        if (!s1 && !s2) return 0;
        if (!s1) return static_cast<uint64_t>(-1);
        if (!s2) return 1;
#if defined(LCS_PLATFORM_WINDOWS)
        return static_cast<uint64_t>(_strnicmp(s1, s2, n));
#else
        return static_cast<uint64_t>(strncasecmp(s1, s2, n));
#endif
    });

    register_function("strchr", [](const uint64_t* args) -> uint64_t {
        const char* s = reinterpret_cast<const char*>(args[0]);
        int c = static_cast<int>(args[1]);
        if (!s) return 0;
        const char* ret = std::strchr(s, c);
        return reinterpret_cast<uint64_t>(ret);
    });

    register_function("__strchr_chk", [](const uint64_t* args) -> uint64_t {
        const char* s = reinterpret_cast<const char*>(args[0]);
        int c = static_cast<int>(args[1]);
        if (!s) return 0;
        const char* ret = std::strchr(s, c);
        return reinterpret_cast<uint64_t>(ret);
    });

    register_function("strrchr", [](const uint64_t* args) -> uint64_t {
        const char* s = reinterpret_cast<const char*>(args[0]);
        int c = static_cast<int>(args[1]);
        if (!s) return 0;
        const char* ret = std::strrchr(s, c);
        return reinterpret_cast<uint64_t>(ret);
    });

    register_function("__strrchr_chk", [](const uint64_t* args) -> uint64_t {
        const char* s = reinterpret_cast<const char*>(args[0]);
        int c = static_cast<int>(args[1]);
        if (!s) return 0;
        const char* ret = std::strrchr(s, c);
        return reinterpret_cast<uint64_t>(ret);
    });

    register_function("memchr", [](const uint64_t* args) -> uint64_t {
        const void* s = reinterpret_cast<const void*>(args[0]);
        int c = static_cast<int>(args[1]);
        size_t n = args[2];
        if (!s || n == 0) return 0;
        const void* ret = std::memchr(s, c, n);
        return reinterpret_cast<uint64_t>(ret);
    });

    register_function("strtok", [](const uint64_t* args) -> uint64_t {
        char* str = reinterpret_cast<char*>(args[0]);
        const char* delim = reinterpret_cast<const char*>(args[1]);
        if (!delim) return 0;
        char* ret = std::strtok(str, delim);
        return reinterpret_cast<uint64_t>(ret);
    });

    register_function("strcat", [](const uint64_t* args) -> uint64_t {
        char* dest = reinterpret_cast<char*>(args[0]);
        const char* src = reinterpret_cast<const char*>(args[1]);
        if (dest && src) {
            std::strcat(dest, src);
        }
        return args[0];
    });

    register_function("vsnprintf", [](const uint64_t* args) -> uint64_t {
        char* buf = reinterpret_cast<char*>(args[0]);
        size_t maxlen = static_cast<size_t>(args[1]);
        const char* fmt = reinterpret_cast<const char*>(args[2]);
        uint64_t ap = args[3];
        return static_cast<uint64_t>(format_arm64_vsnprintf(buf, maxlen, fmt, ap));
    });

    register_function("snprintf", [](const uint64_t* args) -> uint64_t {
        char* buf = reinterpret_cast<char*>(args[0]);
        size_t maxlen = static_cast<size_t>(args[1]);
        const char* fmt = reinterpret_cast<const char*>(args[2]);
        if (!buf || maxlen == 0) return 0;
        if (!fmt) { buf[0] = '\0'; return 0; }
        return static_cast<uint64_t>(std::snprintf(buf, maxlen, fmt, args[3], args[4], args[5], args[6], args[7]));
    });

    register_function("strtoul", [](const uint64_t* args) -> uint64_t {
        const char* nptr = reinterpret_cast<const char*>(args[0]);
        char** endptr = reinterpret_cast<char**>(args[1]);
        int base = static_cast<int>(args[2]);
        return nptr ? std::strtoul(nptr, endptr, base) : 0;
    });

    register_function("atoi", [](const uint64_t* args) -> uint64_t {
        const char* str = reinterpret_cast<const char*>(args[0]);
        return str ? static_cast<uint64_t>(std::atoi(str)) : 0;
    });

    register_function("atof", [](const uint64_t* args) -> uint64_t {
        const char* str = reinterpret_cast<const char*>(args[0]);
        double d = str ? std::atof(str) : 0.0;
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        return bits;
    });

    register_function("sscanf", [this](const uint64_t* args) -> uint64_t {
        const char* s = reinterpret_cast<const char*>(args[0]);
        const char* fmt = reinterpret_cast<const char*>(args[1]);
        if (!s || !fmt) return 0;

        uint64_t sp = get_active_cpu() ? get_active_cpu()->get_sp() : 0;
        alignas(16) static uint8_t s_dummy_scratch[1024] = {0};
        auto get_arg = [&](size_t idx) -> void* {
            uint64_t ptr = 0;
            if (idx < 8) {
                ptr = args[idx];
            } else if (sp) {
                ptr = *reinterpret_cast<const uint64_t*>(sp + (idx - 8) * 8);
            }
            if (ptr == 0) return s_dummy_scratch;
            return reinterpret_cast<void*>(ptr);
        };

        return static_cast<uint64_t>(std::sscanf(s, fmt,
            get_arg(2), get_arg(3), get_arg(4), get_arg(5),
            get_arg(6), get_arg(7), get_arg(8), get_arg(9),
            get_arg(10), get_arg(11), get_arg(12), get_arg(13),
            get_arg(14), get_arg(15), get_arg(16), get_arg(17),
            get_arg(18), get_arg(19), get_arg(20), get_arg(21),
            get_arg(22), get_arg(23), get_arg(24), get_arg(25),
            get_arg(26), get_arg(27), get_arg(28), get_arg(29),
            get_arg(30), get_arg(31), get_arg(32), get_arg(33),
            get_arg(34), get_arg(35), get_arg(36), get_arg(37),
            get_arg(38), get_arg(39), get_arg(40), get_arg(41),
            get_arg(42), get_arg(43), get_arg(44), get_arg(45),
            get_arg(46), get_arg(47), get_arg(48), get_arg(49),
            get_arg(50), get_arg(51), get_arg(52), get_arg(53),
            get_arg(54), get_arg(55), get_arg(56), get_arg(57),
            get_arg(58), get_arg(59), get_arg(60), get_arg(61),
            get_arg(62), get_arg(63), get_arg(64), get_arg(65)
        ));
    });

    // Math (libm)
    register_function("sinf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::sin(x));
        }
        return 0;
    });

    register_function("cosf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::cos(x));
        }
        return 0;
    });

    register_function("sincosf", [this](const uint64_t* args) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            float* sinptr = reinterpret_cast<float*>(args[0]);
            float* cosptr = reinterpret_cast<float*>(args[1]);
            if (sinptr) *sinptr = std::sin(x);
            if (cosptr) *cosptr = std::cos(x);
        }
        return 0;
    });

    register_function("tanf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::tan(x));
        }
        return 0;
    });

    register_function("atanf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::atan(x));
        }
        return 0;
    });

    register_function("atan2f", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float y = cpu->get_fp_float(0);
            float x = cpu->get_fp_float(1);
            cpu->set_fp_float(0, std::atan2(y, x));
        }
        return 0;
    });

    register_function("asinf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::asin(x));
        }
        return 0;
    });

    register_function("acosf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::acos(x));
        }
        return 0;
    });

    register_function("expf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::exp(x));
        }
        return 0;
    });

    register_function("exp2f", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::exp2(x));
        }
        return 0;
    });

    register_function("logf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            cpu->set_fp_float(0, std::log(x));
        }
        return 0;
    });

    register_function("powf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            float y = cpu->get_fp_float(1);
            cpu->set_fp_float(0, std::pow(x, y));
        }
        return 0;
    });

    register_function("pow", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            double x = cpu->get_fp_double(0);
            double y = cpu->get_fp_double(1);
            cpu->set_fp_double(0, std::pow(x, y));
        }
        return 0;
    });

    register_function("fmodf", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) {
            float x = cpu->get_fp_float(0);
            float y = cpu->get_fp_float(1);
            cpu->set_fp_float(0, std::fmod(x, y));
        }
        return 0;
    });

    register_function("qsort", [this](const uint64_t* args) -> uint64_t {
        char* base = reinterpret_cast<char*>(args[0]);
        size_t nmemb = args[1];
        size_t size = args[2];
        uint64_t compar = args[3];

        auto* cpu = get_active_cpu();
        if (!base || nmemb <= 1 || size == 0 || !compar || !cpu) return 0;

        std::vector<char> temp(size);
        for (size_t i = 1; i < nmemb; i++) {
            std::memcpy(temp.data(), base + i * size, size);
            size_t j = i;
            while (j > 0) {
                int32_t res = static_cast<int32_t>(cpu->call_nested(compar, {
                    reinterpret_cast<uint64_t>(base + (j - 1) * size),
                    reinterpret_cast<uint64_t>(temp.data())
                }));
                if (res <= 0) break;
                std::memcpy(base + j * size, base + (j - 1) * size, size);
                j--;
            }
            std::memcpy(base + j * size, temp.data(), size);
        }
        return 0;
    });

    register_function("strcpy", [](const uint64_t* args) -> uint64_t {
        std::strcpy(reinterpret_cast<char*>(args[0]), reinterpret_cast<const char*>(args[1]));
        return args[0];
    });
    register_function("__strcpy_chk", [](const uint64_t* args) -> uint64_t {
        std::strcpy(reinterpret_cast<char*>(args[0]), reinterpret_cast<const char*>(args[1]));
        return args[0];
    });

    register_function("strncpy", [](const uint64_t* args) -> uint64_t {
        std::strncpy(reinterpret_cast<char*>(args[0]), reinterpret_cast<const char*>(args[1]), args[2]);
        return args[0];
    });
    register_function("__strncpy_chk", [](const uint64_t* args) -> uint64_t {
        std::strncpy(reinterpret_cast<char*>(args[0]), reinterpret_cast<const char*>(args[1]), args[2]);
        return args[0];
    });

    // Android NDK System & Logging
    register_function("__android_log_print", [this](const uint64_t* args) -> uint64_t {
        const char* tag = reinterpret_cast<const char*>(args[1]);
        const char* fmt = reinterpret_cast<const char*>(args[2]);
        uint64_t sp = get_active_cpu() ? get_active_cpu()->get_sp() : 0;
        auto get_arg = [&](size_t idx) -> uint64_t {
            if (idx < 8) return args[idx];
            if (sp) return *reinterpret_cast<const uint64_t*>(sp + (idx - 8) * 8);
            return 0;
        };
        char buf[1024];
        if (fmt) {
            std::snprintf(buf, sizeof(buf), fmt,
                get_arg(3), get_arg(4), get_arg(5), get_arg(6),
                get_arg(7), get_arg(8), get_arg(9), get_arg(10));
        } else {
            buf[0] = '\0';
        }
        std::cout << "[Android " << (tag ? tag : "LCS") << "] " << buf << std::endl;
        return 0;
    });

    register_function("__android_log_write", [](const uint64_t* args) -> uint64_t {
        const char* tag = reinterpret_cast<const char*>(args[1]);
        const char* msg = reinterpret_cast<const char*>(args[2]);
        std::cout << "[Android " << (tag ? tag : "LCS") << "] " << (msg ? msg : "") << "\n";
        return 0;
    });

    register_function("__system_property_get", [](const uint64_t* args) -> uint64_t {
        const char* name = reinterpret_cast<const char*>(args[0]);
        char* value = reinterpret_cast<char*>(args[1]);
        if (!name || !value) return 0;

        const char* result = "";
        if (std::strcmp(name, "ro.build.version.sdk") == 0) result = "28";
        else if (std::strcmp(name, "ro.product.cpu.abi") == 0) result = "arm64-v8a";
        else if (std::strcmp(name, "ro.product.manufacturer") == 0) result = "PC";
        else if (std::strcmp(name, "ro.product.model") == 0) result = "PC";
        else if (std::strcmp(name, "ro.arch") == 0) result = "aarch64";

        std::strcpy(value, result);
        return std::strlen(result);
    });

    register_function("sysconf", [](const uint64_t* args) -> uint64_t {
        int name = static_cast<int>(args[0]);
        if (name == 30) return 4096; // _SC_PAGESIZE
        if (name == 97) return 8;    // _SC_NPROCESSORS_CONF
        if (name == 98) return 8;    // _SC_NPROCESSORS_ONLN
        return 0;
    });

    register_function("getenv", [](const uint64_t*) -> uint64_t {
        return 0;
    });

    register_function("gettid", [](const uint64_t*) -> uint64_t {
        return 1000;
    });

    register_function("getpid", [](const uint64_t*) -> uint64_t {
        return 1000;
    });

    register_function("gettimeofday", [](const uint64_t* args) -> uint64_t {
        uint64_t* tv = reinterpret_cast<uint64_t*>(args[0]);
        if (tv) {
            auto now = std::chrono::system_clock::now();
            auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            tv[0] = static_cast<uint64_t>(micros / 1000000); // seconds
            tv[1] = static_cast<uint64_t>(micros % 1000000); // microseconds
        }
        return 0;
    });

    register_function("usleep", [](const uint64_t* args) -> uint64_t {
        uint64_t usec = args[0];
#if defined(_WIN32)
        if (usec >= 1000) {
            Sleep(static_cast<DWORD>(usec / 1000));
        } else if (usec > 0) {
            Sleep(1);
        } else {
            Sleep(0);
        }
#else
        usleep(usec);
#endif
        return 0;
    });

    register_function("newlocale", [](const uint64_t*) -> uint64_t {
        return 0x7f00001000ULL;
    });
    register_function("uselocale", [](const uint64_t*) -> uint64_t {
        return 0x7f00001000ULL;
    });
    register_function("freelocale", [](const uint64_t*) -> uint64_t {
        return 0;
    });

    register_function("__android_log_vprint", [](const uint64_t* args) -> uint64_t {
        const char* tag = reinterpret_cast<const char*>(args[1]);
        const char* fmt = reinterpret_cast<const char*>(args[2]);
        if (fmt) {
            std::cout << "[Android Log " << (tag ? tag : "") << "] " << fmt << "\n";
        }
        return 0;
    });

    register_function("__vsnprintf_chk", [](const uint64_t* args) -> uint64_t {
        char* buf = reinterpret_cast<char*>(args[0]);
        size_t maxlen = static_cast<size_t>(args[1]);
        const char* fmt = reinterpret_cast<const char*>(args[4]);
        uint64_t ap = args[5];
        return static_cast<uint64_t>(format_arm64_vsnprintf(buf, maxlen, fmt, ap));
    });

    register_function("glGetString", [](const uint64_t* args) -> uint64_t {
        uint32_t name = static_cast<uint32_t>(args[0]);
        static const char* s_vendor = "Rockstar Games / LCS Recomp";
        static const char* s_renderer = "Direct3D 12 / Vulkan 1.4 Native Bridge";
        static const char* s_version = "OpenGL ES 3.2 (GTA LCS Recomp Native)";
        static const char* s_extensions = "GL_OES_texture_float GL_OES_element_index_uint GL_EXT_texture_filter_anisotropic";

        switch (name) {
            case 0x1f00: return reinterpret_cast<uint64_t>(s_vendor);
            case 0x1f01: return reinterpret_cast<uint64_t>(s_renderer);
            case 0x1f02: return reinterpret_cast<uint64_t>(s_version);
            case 0x1f03: return reinterpret_cast<uint64_t>(s_extensions);
            default:     return reinterpret_cast<uint64_t>("");
        }
    });

    register_function("strstr", [](const uint64_t* args) -> uint64_t {
        if (!args[0] || !args[1]) return 0;
        const char* haystack = reinterpret_cast<const char*>(args[0]);
        const char* needle = reinterpret_cast<const char*>(args[1]);
        const char* res = std::strstr(haystack, needle);
        return reinterpret_cast<uint64_t>(res);
    });

    register_function("getauxval", [](const uint64_t* args) -> uint64_t {
        uint64_t type = args[0];
        if (type == 0x10) { // AT_HWCAP
            return 0xff; // FP | ASIMD | AES | PMULL | SHA1 | SHA2 | CRC32
        }
        if (type == 6) { // AT_PAGESZ
            return 4096;
        }
        return 0;
    });

    register_function("syscall", [](const uint64_t* args) -> uint64_t {
        int64_t sys_no = static_cast<int64_t>(args[0]);
        switch (sys_no) {
            case 178: // __NR_gettid (0xb2)
                return 1000;
            case 172: // __NR_getpid (0xac)
                return 1000;
            case 98:  // __NR_futex
                return 0;
            case 96:  // __NR_set_tid_address
                return 1000;
            case 113: // __NR_clock_gettime
                return 0;
            default:
                std::cout << "[HLE syscall] Handled syscall " << sys_no << "\n";
                return 0;
        }
    });

    // Standard File I/O
    static int s_errno = 0;
    register_function("__errno", [](const uint64_t*) -> uint64_t {
        return reinterpret_cast<uint64_t>(&s_errno);
    });

    // Android Native Window
    register_function("ANativeWindow_fromSurface", [](const uint64_t*) -> uint64_t {
        return 0x7f60404000ULL;
    });
    register_function("ANativeWindow_getWidth", [](const uint64_t*) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().get_surface_width();
    });
    register_function("ANativeWindow_getHeight", [](const uint64_t*) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().get_surface_height();
    });
    register_function("ANativeWindow_setBuffersGeometry", [](const uint64_t*) -> uint64_t {
        return 0;
    });
    register_function("ANativeWindow_release", [](const uint64_t*) -> uint64_t {
        return 0;
    });

    // GLES State Stubs
    register_function("glViewport", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glClearColor", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glClear", [](const uint64_t*) -> uint64_t { return 0; });

    // Standard C Algorithm & String Helpers
    register_function("bsearch", [this](const uint64_t* args) -> uint64_t {
        const char* key = reinterpret_cast<const char*>(args[0]);
        const char* base = reinterpret_cast<const char*>(args[1]);
        size_t nmemb = args[2];
        size_t size = args[3];
        uint64_t compar = args[4];

        auto* cpu = get_active_cpu();
        if (!key || !base || nmemb == 0 || size == 0 || !compar || !cpu) return 0;

        size_t low = 0;
        size_t high = nmemb;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            const char* elem = base + mid * size;
            int32_t res = static_cast<int32_t>(cpu->call_nested(compar, {
                reinterpret_cast<uint64_t>(key),
                reinterpret_cast<uint64_t>(elem)
            }));
            if (res == 0) {
                return reinterpret_cast<uint64_t>(elem);
            } else if (res < 0) {
                high = mid;
            } else {
                low = mid + 1;
            }
        }
        return 0;
    });

    register_function("strerror", [](const uint64_t*) -> uint64_t {
        static char s_err_buf[128] = "Success";
        return reinterpret_cast<uint64_t>(s_err_buf);
    });

    register_function("__strcat_chk", [](const uint64_t* args) -> uint64_t {
        char* dest = reinterpret_cast<char*>(args[0]);
        const char* src = reinterpret_cast<const char*>(args[1]);
        if (dest && src) {
            std::strcat(dest, src);
        }
        return args[0];
    });

    register_function("__vsprintf_chk", [](const uint64_t* args) -> uint64_t {
        char* s = reinterpret_cast<char*>(args[0]);
        size_t slen = static_cast<size_t>(args[2]);
        const char* format = reinterpret_cast<const char*>(args[3]);
        uint64_t ap = args[4];
        int ret = format_arm64_vsnprintf(s, slen ? slen : 4096, format, ap);
        return static_cast<uint64_t>(ret);
    });

    register_function("fopen", [](const uint64_t* args) -> uint64_t {
        const char* path = reinterpret_cast<const char*>(args[0]);
        const char* mode = reinterpret_cast<const char*>(args[1]);
        if (!path || !mode) return 0;

        // Sanitize mode string for MSVC CRT (strip Linux/POSIX flags like 'e', 'm', 'x', 'c')
        char clean_mode[16] = {0};
        size_t c_idx = 0;
        for (const char* m = mode; *m && c_idx < sizeof(clean_mode) - 1; ++m) {
            if (*m == 'r' || *m == 'w' || *m == 'a' || *m == 'b' || *m == '+' || *m == 't') {
                clean_mode[c_idx++] = *m;
            }
        }
        if (c_idx == 0) {
            clean_mode[0] = 'r';
        }

        std::string spath(path);
        // Normalize any Linux system file requests (e.g., OpenAL configuration files)
        if (spath.rfind("/etc/", 0) == 0 || spath.rfind("/proc/", 0) == 0 || spath.rfind("/sys/", 0) == 0) {
            std::string local_path = "gtalcs" + spath.substr(spath.find_last_of('/'));
            if (std::filesystem::exists(local_path)) {
                spath = local_path;
            } else {
                std::cout << "[HLE] fopen: rejected system path \"" << path << "\"\n";
                return 0;
            }
        }

        FILE* fp = std::fopen(spath.c_str(), clean_mode);
        std::cout << "[HLE] fopen(\"" << path << "\" [mode: \"" << clean_mode << "\"]) -> " << fp << "\n";
        return reinterpret_cast<uint64_t>(fp);
    });

    register_function("getcwd", [](const uint64_t* args) -> uint64_t {
        char* buf = reinterpret_cast<char*>(args[0]);
        size_t size = static_cast<size_t>(args[1]);
        if (buf && size > 1) {
            std::strncpy(buf, ".", size);
            return args[0];
        }
        return 0;
    });

    register_function("__readlink_chk", [](const uint64_t*) -> uint64_t {
        return static_cast<uint64_t>(-1);
    });

    register_function("readlink", [](const uint64_t*) -> uint64_t {
        return static_cast<uint64_t>(-1);
    });

    register_function("opendir", [](const uint64_t* args) -> uint64_t {
        const char* name = reinterpret_cast<const char*>(args[0]);
        std::cout << "[HLE] opendir(\"" << (name ? name : "null") << "\") -> null\n";
        return 0;
    });

    register_function("closedir", [](const uint64_t*) -> uint64_t {
        return 0;
    });

    register_function("readdir", [](const uint64_t*) -> uint64_t {
        return 0;
    });

    register_function("slCreateEngine", [](const uint64_t*) -> uint64_t {
        std::cout << "[HLE] slCreateEngine called -> returning SL_RESULT_FEATURE_UNSUPPORTED (0xc)\n";
        return 0x0000000cULL;
    });

    register_function("fclose", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        if (fp && fp != stdout && fp != stderr && fp != stdin) {
            return std::fclose(fp);
        }
        return 0;
    });

    register_function("fread", [](const uint64_t* args) -> uint64_t {
        void* ptr = reinterpret_cast<void*>(args[0]);
        size_t size = args[1];
        size_t count = args[2];
        FILE* fp = lcs::os::translate_guest_file_ptr(args[3]);
        return (ptr && fp) ? std::fread(ptr, size, count, fp) : 0;
    });

    register_function("fwrite", [](const uint64_t* args) -> uint64_t {
        const void* ptr = reinterpret_cast<const void*>(args[0]);
        size_t size = args[1];
        size_t count = args[2];
        FILE* fp = lcs::os::translate_guest_file_ptr(args[3]);
        return (ptr && fp) ? std::fwrite(ptr, size, count, fp) : 0;
    });

    register_function("fseek", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        long offset = static_cast<long>(args[1]);
        int origin = static_cast<int>(args[2]);
        return fp ? std::fseek(fp, offset, origin) : -1;
    });

    register_function("ftell", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        return fp ? std::ftell(fp) : -1;
    });

    register_function("fputs", [](const uint64_t* args) -> uint64_t {
        const char* str = reinterpret_cast<const char*>(args[0]);
        FILE* fp = lcs::os::translate_guest_file_ptr(args[1]);
        return (str && fp) ? std::fputs(str, fp) : 0;
    });

    register_function("fflush", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        return fp ? std::fflush(fp) : 0;
    });

    register_function("fputc", [](const uint64_t* args) -> uint64_t {
        int c = static_cast<int>(args[0]);
        FILE* fp = lcs::os::translate_guest_file_ptr(args[1]);
        return fp ? std::fputc(c, fp) : EOF;
    });

    register_function("fprintf", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        const char* fmt = reinterpret_cast<const char*>(args[1]);
        if (!fp || !fmt) return 0;
        return static_cast<uint64_t>(std::fprintf(fp, fmt, args[2], args[3], args[4], args[5], args[6], args[7]));
    });

    register_function("vfprintf", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        const char* fmt = reinterpret_cast<const char*>(args[1]);
        uint64_t ap = args[2];
        std::cout << "[HLE vfprintf] guest_fp=0x" << std::hex << args[0] << " translated_fp=" << fp
                  << " fmt=\"" << (fmt ? fmt : "null") << "\" ap=0x" << ap << std::dec << "\n";
        if (!fmt) return 0;
        char buf[2048] = {0};
        int ret = format_arm64_vsnprintf(buf, sizeof(buf), fmt, ap);
        std::cout << "[HLE vfprintf formatted] " << buf << "\n";
        if (fp) {
            std::fputs(buf, fp);
            std::fflush(fp);
        } else {
            std::cout << buf << std::flush;
        }
        return static_cast<uint64_t>(ret);
    });

    register_function("dl_iterate_phdr", [](const uint64_t*) -> uint64_t {
        return 0;
    });

    register_function("pthread_rwlock_rdlock", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_rwlock_wrlock", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_rwlock_unlock", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_rwlock_init", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_rwlock_destroy", [](const uint64_t*) -> uint64_t { return 0; });

    // EGL Bridge
    register_function("eglGetDisplay", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_get_display(args[0]);
    });

    register_function("eglInitialize", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_initialize(
            args[0],
            reinterpret_cast<int32_t*>(args[1]),
            reinterpret_cast<int32_t*>(args[2])
        );
    });

    register_function("eglTerminate", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_terminate(args[0]);
    });

    register_function("eglChooseConfig", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_choose_config(
            args[0],
            reinterpret_cast<const int32_t*>(args[1]),
            reinterpret_cast<uint64_t*>(args[2]),
            static_cast<int32_t>(args[3]),
            reinterpret_cast<int32_t*>(args[4])
        );
    });

    register_function("eglGetConfigAttrib", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_get_config_attrib(
            args[0],
            args[1],
            static_cast<int32_t>(args[2]),
            reinterpret_cast<int32_t*>(args[3])
        );
    });

    register_function("eglCreateWindowSurface", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_create_window_surface(
            args[0],
            args[1],
            args[2],
            reinterpret_cast<const int32_t*>(args[3])
        );
    });

    register_function("eglCreateContext", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_create_context(
            args[0],
            args[1],
            args[2],
            reinterpret_cast<const int32_t*>(args[3])
        );
    });

    register_function("eglMakeCurrent", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_make_current(
            args[0],
            args[1],
            args[2],
            args[3]
        );
    });

    register_function("eglSwapBuffers", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_swap_buffers(args[0], args[1]);
    });

    register_function("eglSwapInterval", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_swap_interval(args[0], static_cast<int32_t>(args[1]));
    });

    register_function("eglQuerySurface", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_query_surface(
            args[0],
            args[1],
            static_cast<int32_t>(args[2]),
            reinterpret_cast<int32_t*>(args[3])
        );
    });

    register_function("eglDestroySurface", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_destroy_surface(args[0], args[1]);
    });

    register_function("eglDestroyContext", [](const uint64_t* args) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_destroy_context(args[0], args[1]);
    });

    register_function("eglGetError", [](const uint64_t*) -> uint64_t {
        return lcs::graphics::RenderBridge::instance().egl_get_error();
    });

    register_function("eglGetProcAddress", [this](const uint64_t* args) -> uint64_t {
        const char* procname = reinterpret_cast<const char*>(args[0]);
        if (!procname) return 0;
        return resolve(procname);
    });

    // GLES 2.0/3.0 State, Shader, Buffer, and Drawing Bridge
    static uint32_t s_next_tex_id = 1;
    register_function("glGenTextures", [](const uint64_t* args) -> uint64_t {
        int32_t n = static_cast<int32_t>(args[0]);
        uint32_t* textures = reinterpret_cast<uint32_t*>(args[1]);
        if (textures && n > 0) {
            for (int32_t i = 0; i < n; i++) textures[i] = s_next_tex_id++;
        }
        return 0;
    });
    register_function("glDeleteTextures", [](const uint64_t*) -> uint64_t { return 0; });

    static uint32_t s_next_buf_id = 1;
    register_function("glGenBuffers", [](const uint64_t* args) -> uint64_t {
        int32_t n = static_cast<int32_t>(args[0]);
        uint32_t* buffers = reinterpret_cast<uint32_t*>(args[1]);
        if (buffers && n > 0) {
            for (int32_t i = 0; i < n; i++) buffers[i] = s_next_buf_id++;
        }
        return 0;
    });
    register_function("glDeleteBuffers", [](const uint64_t*) -> uint64_t { return 0; });

    static uint32_t s_next_fbo_id = 1;
    register_function("glGenFramebuffers", [](const uint64_t* args) -> uint64_t {
        int32_t n = static_cast<int32_t>(args[0]);
        uint32_t* fbos = reinterpret_cast<uint32_t*>(args[1]);
        if (fbos && n > 0) {
            for (int32_t i = 0; i < n; i++) fbos[i] = s_next_fbo_id++;
        }
        return 0;
    });
    register_function("glDeleteFramebuffers", [](const uint64_t*) -> uint64_t { return 0; });

    static uint32_t s_next_rbo_id = 1;
    register_function("glGenRenderbuffers", [](const uint64_t* args) -> uint64_t {
        int32_t n = static_cast<int32_t>(args[0]);
        uint32_t* rbos = reinterpret_cast<uint32_t*>(args[1]);
        if (rbos && n > 0) {
            for (int32_t i = 0; i < n; i++) rbos[i] = s_next_rbo_id++;
        }
        return 0;
    });
    register_function("glDeleteRenderbuffers", [](const uint64_t*) -> uint64_t { return 0; });

    register_function("glCheckFramebufferStatus", [](const uint64_t*) -> uint64_t {
        return 0x8CD5; // GL_FRAMEBUFFER_COMPLETE
    });

    static uint32_t s_next_shader_id = 1;
    register_function("glCreateShader", [](const uint64_t*) -> uint64_t {
        return s_next_shader_id++;
    });
    register_function("glShaderSource", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glCompileShader", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glGetShaderiv", [](const uint64_t* args) -> uint64_t {
        uint32_t pname = static_cast<uint32_t>(args[1]);
        int32_t* params = reinterpret_cast<int32_t*>(args[2]);
        if (params) {
            if (pname == 0x8B81) *params = 1; // GL_COMPILE_STATUS
            else *params = 0;
        }
        return 0;
    });
    register_function("glGetShaderInfoLog", [](const uint64_t* args) -> uint64_t {
        int32_t* length = reinterpret_cast<int32_t*>(args[2]);
        char* infoLog = reinterpret_cast<char*>(args[3]);
        if (length) *length = 0;
        if (infoLog) infoLog[0] = '\0';
        return 0;
    });

    static uint32_t s_next_prog_id = 1;
    register_function("glCreateProgram", [](const uint64_t*) -> uint64_t {
        return s_next_prog_id++;
    });
    register_function("glAttachShader", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glLinkProgram", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUseProgram", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glGetProgramiv", [](const uint64_t* args) -> uint64_t {
        uint32_t pname = static_cast<uint32_t>(args[1]);
        int32_t* params = reinterpret_cast<int32_t*>(args[2]);
        if (params) {
            if (pname == 0x8B82) *params = 1; // GL_LINK_STATUS
            else *params = 0;
        }
        return 0;
    });
    register_function("glGetProgramInfoLog", [](const uint64_t* args) -> uint64_t {
        int32_t* length = reinterpret_cast<int32_t*>(args[2]);
        char* infoLog = reinterpret_cast<char*>(args[3]);
        if (length) *length = 0;
        if (infoLog) infoLog[0] = '\0';
        return 0;
    });
    register_function("glGetAttribLocation", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glGetUniformLocation", [](const uint64_t*) -> uint64_t { return 0; });

    register_function("glBindTexture", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBindBuffer", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBindFramebuffer", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBindRenderbuffer", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glFramebufferTexture2D", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glRenderbufferStorage", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glFramebufferRenderbuffer", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glTexParameteri", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glTexParameterf", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glTexImage2D", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glTexSubImage2D", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBufferData", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBufferSubData", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glEnable", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glDisable", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBlendFunc", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glBlendFuncSeparate", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glDepthFunc", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glDepthMask", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glColorMask", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glCullFace", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glScissor", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glVertexAttribPointer", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glEnableVertexAttribArray", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glDisableVertexAttribArray", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUniform1i", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUniform1f", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUniform2f", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUniform3f", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUniform4f", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glUniformMatrix4fv", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glDrawArrays", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glDrawElements", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glFlush", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glFinish", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glGetError", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("glActiveTexture", [](const uint64_t*) -> uint64_t { return 0; });

    // Timing & System
    register_function("clock_gettime", [](const uint64_t* args) -> uint64_t {
        uint64_t* tp = reinterpret_cast<uint64_t*>(args[1]);
        if (tp) {
            auto now = std::chrono::steady_clock::now();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            tp[0] = static_cast<uint64_t>(nanos / 1000000000ULL);
            tp[1] = static_cast<uint64_t>(nanos % 1000000000ULL);
        }
        return 0;
    });

    register_function("time", [](const uint64_t* args) -> uint64_t {
        auto now = std::chrono::system_clock::now();
        time_t t = std::chrono::system_clock::to_time_t(now);
        time_t* out = reinterpret_cast<time_t*>(args[0]);
        if (out) *out = t;
        return static_cast<uint64_t>(t);
    });

    register_function("fseeko", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
        int64_t offset = static_cast<int64_t>(args[1]);
        int origin = static_cast<int>(args[2]);
#if defined(_WIN32)
        return fp ? _fseeki64(fp, offset, origin) : -1;
#else
        return fp ? fseeko(fp, offset, origin) : -1;
#endif
    });

    register_function("ftello", [](const uint64_t* args) -> uint64_t {
        FILE* fp = lcs::os::translate_guest_file_ptr(args[0]);
#if defined(_WIN32)
        return fp ? static_cast<uint64_t>(_ftelli64(fp)) : static_cast<uint64_t>(-1);
#else
        return fp ? static_cast<uint64_t>(ftello(fp)) : static_cast<uint64_t>(-1);
#endif
    });

    register_function("__strncpy_chk2", [](const uint64_t* args) -> uint64_t {
        char* dest = reinterpret_cast<char*>(args[0]);
        const char* src = reinterpret_cast<const char*>(args[1]);
        size_t n = args[2];
        if (dest && src && n) std::strncpy(dest, src, n);
        return args[0];
    });

    register_function("sin", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) cpu->set_fp_double(0, std::sin(cpu->get_fp_double(0)));
        return 0;
    });
    register_function("cos", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) cpu->set_fp_double(0, std::cos(cpu->get_fp_double(0)));
        return 0;
    });
    register_function("tan", [this](const uint64_t*) -> uint64_t {
        auto* cpu = get_active_cpu();
        if (cpu) cpu->set_fp_double(0, std::tan(cpu->get_fp_double(0)));
        return 0;
    });

    register_function("setvbuf", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("sem_init", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_attr_init", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_attr_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_attr_setdetachstate", [](const uint64_t*) -> uint64_t { return 0; });
    register_function("pthread_attr_setstacksize", [](const uint64_t*) -> uint64_t { return 0; });

    register_function("_ZTHN10ALCcontext13sLocalContextE", [](const uint64_t*) -> uint64_t {
        return 0;
    });
}

} // namespace lcs::cpu
