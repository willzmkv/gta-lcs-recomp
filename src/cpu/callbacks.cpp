#include "callbacks.h"
#include "dynarmic_core.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <chrono>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

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

    register_default_stubs();
    return true;
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

    return allocate_trampoline(id);
}

uint64_t HLEManager::resolve(const char* name) {
    if (!name || *name == '\0') return 0;

    std::string s(name);
    auto it = m_symbol_to_id.find(s);
    if (it != m_symbol_to_id.end()) {
        return TRAMPOLINE_BASE + (it->second * 8);
    }

    // Auto-stub unknown symbol
    return register_function(s, [s](const uint64_t* args) -> uint64_t {
        std::cout << "[HLE STUB] Called unimplemented: " << s
                  << "(0x" << std::hex << args[0] << ", 0x" << args[1]
                  << ", 0x" << args[2] << ", 0x" << args[3] << std::dec << ")\n";
        return 0;
    });
}

void HLEManager::wire_to_cpu(DynarmicCore& cpu) {
    m_cpu = &cpu;
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
            if (init_routine && m_cpu) {
                std::cout << "[HLE] pthread_once invoking guest callback 0x" << std::hex << init_routine << std::dec << "...\n";
                m_cpu->call_nested(init_routine);
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

    // Memory management
    register_function("malloc", [](const uint64_t* args) -> uint64_t {
        return reinterpret_cast<uint64_t>(std::malloc(args[0]));
    });

    register_function("free", [](const uint64_t* args) -> uint64_t {
        std::free(reinterpret_cast<void*>(args[0]));
        return 0;
    });

    register_function("calloc", [](const uint64_t* args) -> uint64_t {
        return reinterpret_cast<uint64_t>(std::calloc(args[0], args[1]));
    });

    register_function("realloc", [](const uint64_t* args) -> uint64_t {
        return reinterpret_cast<uint64_t>(std::realloc(reinterpret_cast<void*>(args[0]), args[1]));
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
        return std::strcmp(reinterpret_cast<const char*>(args[0]), reinterpret_cast<const char*>(args[1]));
    });

    register_function("strncmp", [](const uint64_t* args) -> uint64_t {
        return std::strncmp(reinterpret_cast<const char*>(args[0]), reinterpret_cast<const char*>(args[1]), args[2]);
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
    register_function("__android_log_print", [](const uint64_t* args) -> uint64_t {
        const char* tag = reinterpret_cast<const char*>(args[1]);
        const char* fmt = reinterpret_cast<const char*>(args[2]);
        std::cout << "[Android " << (tag ? tag : "LCS") << "] " << (fmt ? fmt : "") << "\n";
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
        if (buf && maxlen > 0) {
            buf[0] = '\0';
        }
        return 0;
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
}

} // namespace lcs::cpu
