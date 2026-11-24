// Guest thread scheduler and Bionic synchronization primitives.
//
// Threading model:
//   - Each guest pthread maps 1:1 to a host std::thread.
//   - Each host thread owns a dedicated DynarmicCore (separate JIT state, stack,
//     TLS block, exclusive monitor slot). Guest memory is shared (identity mapped).
//   - HLE trampolines are dispatched through the core's fallback SVC handler, so
//     every core sees the same HLE table.
//   - Guest sync objects are keyed by guest address and created lazily, which makes
//     zero-initialized statics (PTHREAD_MUTEX_INITIALIZER etc.) work without init.
//
// Registries are intentionally immortal (never destroyed) so process exit does not
// tear down state that still-running guest threads may touch.

#include "thread_manager.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lcs::os {

namespace {

using Clock = std::chrono::steady_clock;

// Linux / Bionic errno values (guest ABI, not host).
constexpr uint64_t G_EPERM     = 1;
constexpr uint64_t G_ESRCH     = 3;
constexpr uint64_t G_EAGAIN    = 11;
constexpr uint64_t G_EBUSY     = 16;
constexpr uint64_t G_EINVAL    = 22;
constexpr uint64_t G_EDEADLK   = 35;
constexpr uint64_t G_ETIMEDOUT = 110;
constexpr uint64_t G_MINUS_ONE = static_cast<uint64_t>(-1);

constexpr uint64_t MAIN_THREAD_ID = 1;
constexpr size_t   DEFAULT_STACK  = 4ULL * 1024 * 1024;
constexpr size_t   MIN_STACK      = 1ULL * 1024 * 1024;
constexpr size_t   MAX_STACK      = 64ULL * 1024 * 1024;

thread_local uint64_t t_guest_tid = MAIN_THREAD_ID;
thread_local int t_errno = 0;

template <typename T>
T* immortal() {
    static T* s_instance = new T();
    return s_instance;
}

// ---------------------------------------------------------------------------
// Address-keyed registry of host objects backing guest sync primitives.
// ---------------------------------------------------------------------------
template <typename T>
class AddressRegistry {
public:
    T& get(uint64_t addr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& slot = m_objects[addr];
        if (!slot) slot = std::make_unique<T>();
        return *slot;
    }

private:
    std::mutex m_mutex;
    std::unordered_map<uint64_t, std::unique_ptr<T>> m_objects;
};

// Recursive, owner-tracking mutex. Recursive locking is tolerated for every mutex
// type: well-formed guest code cannot observe the difference, and it avoids
// self-deadlock in code paths that were previously exercised with no-op locks.
struct GuestMutex {
    std::mutex m;
    std::condition_variable cv;
    uint64_t owner = 0;
    uint32_t count = 0;
};

struct GuestCond {
    std::condition_variable_any cv;
};

struct GuestSemaphore {
    std::mutex m;
    std::condition_variable cv;
    uint32_t count = 0;
};

AddressRegistry<GuestMutex>& mutexes() { return *immortal<AddressRegistry<GuestMutex>>(); }
AddressRegistry<GuestMutex>& rwlocks() { return *immortal<AddressRegistry<GuestMutex>>(); }
AddressRegistry<GuestCond>& conds() { return *immortal<AddressRegistry<GuestCond>>(); }
AddressRegistry<GuestSemaphore>& semaphores() { return *immortal<AddressRegistry<GuestSemaphore>>(); }

// Converts a guest struct timespec (absolute) to a host steady deadline. Our
// clock_gettime HLE reports steady_clock for every clock id, so small values are
// steady-based; values that look like a Unix epoch came from gettimeofday/time.
Clock::time_point to_deadline(uint64_t ts_addr) {
    if (!ts_addr) return Clock::now();
    int64_t sec = 0, nsec = 0;
    std::memcpy(&sec, reinterpret_cast<const void*>(ts_addr), 8);
    std::memcpy(&nsec, reinterpret_cast<const void*>(ts_addr + 8), 8);
    auto dur = std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec);
    if (sec > 1000000000LL) {
        auto sys_deadline = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(dur));
        return Clock::now() + std::chrono::duration_cast<Clock::duration>(
            sys_deadline - std::chrono::system_clock::now());
    }
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(dur));
}

Clock::time_point to_relative_deadline(uint64_t ts_addr) {
    if (!ts_addr) return Clock::now();
    int64_t sec = 0, nsec = 0;
    std::memcpy(&sec, reinterpret_cast<const void*>(ts_addr), 8);
    std::memcpy(&nsec, reinterpret_cast<const void*>(ts_addr + 8), 8);
    return Clock::now() + std::chrono::duration_cast<Clock::duration>(
        std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec));
}

uint64_t mutex_lock(GuestMutex& gm, const Clock::time_point* deadline) {
    const uint64_t tid = t_guest_tid;
    std::unique_lock<std::mutex> lk(gm.m);
    if (gm.count > 0 && gm.owner == tid) {
        gm.count++;
        return 0;
    }
    auto free_pred = [&] { return gm.count == 0; };
    if (deadline) {
        if (!gm.cv.wait_until(lk, *deadline, free_pred)) return G_ETIMEDOUT;
    } else {
        gm.cv.wait(lk, free_pred);
    }
    gm.owner = tid;
    gm.count = 1;
    return 0;
}

uint64_t mutex_trylock(GuestMutex& gm) {
    const uint64_t tid = t_guest_tid;
    std::lock_guard<std::mutex> lk(gm.m);
    if (gm.count == 0) {
        gm.owner = tid;
        gm.count = 1;
        return 0;
    }
    if (gm.owner == tid) {
        gm.count++;
        return 0;
    }
    return G_EBUSY;
}

uint64_t mutex_unlock(GuestMutex& gm) {
    const uint64_t tid = t_guest_tid;
    std::lock_guard<std::mutex> lk(gm.m);
    if (gm.count == 0 || gm.owner != tid) return G_EPERM;
    if (--gm.count == 0) {
        gm.owner = 0;
        gm.cv.notify_one();
    }
    return 0;
}

uint64_t cond_wait(GuestCond& gc, GuestMutex& gm, const Clock::time_point* deadline) {
    const uint64_t tid = t_guest_tid;
    std::unique_lock<std::mutex> lk(gm.m);
    // The guest believes it owns the mutex on return, so restore the ownership depth
    // it had (at least one) even if it called wait without holding the lock.
    uint32_t saved = (gm.owner == tid && gm.count > 0) ? gm.count : 1;
    if (gm.owner == tid) {
        gm.owner = 0;
        gm.count = 0;
        gm.cv.notify_one();
    }

    bool timed_out = false;
    if (deadline) {
        timed_out = gc.cv.wait_until(lk, *deadline) == std::cv_status::timeout;
    } else {
        gc.cv.wait(lk);
    }

    gm.cv.wait(lk, [&] { return gm.count == 0; });
    gm.owner = tid;
    gm.count = saved;
    return timed_out ? G_ETIMEDOUT : 0;
}

uint64_t set_errno_fail(int err) {
    t_errno = err;
    return G_MINUS_ONE;
}

uint64_t sem_wait_impl(GuestSemaphore& s, const Clock::time_point* deadline) {
    std::unique_lock<std::mutex> lk(s.m);
    auto avail = [&] { return s.count > 0; };
    if (deadline) {
        if (!s.cv.wait_until(lk, *deadline, avail)) return set_errno_fail(static_cast<int>(G_ETIMEDOUT));
    } else {
        s.cv.wait(lk, avail);
    }
    s.count--;
    return 0;
}

// ---------------------------------------------------------------------------
// Thread registry
// ---------------------------------------------------------------------------
struct GuestThread {
    uint64_t id = 0;
    std::thread host;
    uint64_t retval = 0;
    std::atomic<bool> finished{false};
    std::atomic<bool> detached{false};
};

struct ThreadRegistry {
    std::mutex m;
    std::unordered_map<uint64_t, std::shared_ptr<GuestThread>> threads;
    std::atomic<uint64_t> next_id{2};
    std::atomic<uint64_t> running{0};
};

ThreadRegistry& thread_registry() { return *immortal<ThreadRegistry>(); }

// pthread keys: global slot table, per-thread values.
struct KeyTable {
    std::mutex m;
    std::unordered_map<uint32_t, uint64_t> destructors; // key -> guest destructor (0 = none)
    uint32_t next_key = 1;
};

KeyTable& key_table() { return *immortal<KeyTable>(); }
thread_local std::unordered_map<uint32_t, uint64_t> t_key_values;

void run_key_destructors(lcs::cpu::DynarmicCore& core) {
    // POSIX: repeat up to PTHREAD_DESTRUCTOR_ITERATIONS (4) while values remain.
    for (int iter = 0; iter < 4; iter++) {
        bool any = false;
        std::vector<std::pair<uint64_t, uint64_t>> pending; // destructor, value
        {
            std::lock_guard<std::mutex> lock(key_table().m);
            for (auto& [key, value] : t_key_values) {
                if (!value) continue;
                auto it = key_table().destructors.find(key);
                if (it != key_table().destructors.end() && it->second) {
                    pending.emplace_back(it->second, value);
                }
                value = 0;
            }
        }
        for (auto& [dtor, value] : pending) {
            core.call(dtor, {value});
            any = true;
        }
        if (!any) break;
    }
}

// Bionic (LP64) pthread_attr_t: { u32 flags; void* stack_base; size_t stack_size;
// size_t guard_size; i32 sched_policy; i32 sched_priority; char reserved[16]; }
constexpr size_t   ATTR_SIZE           = 56;
constexpr size_t   ATTR_OFF_FLAGS      = 0;
constexpr size_t   ATTR_OFF_STACK_SIZE = 16;
constexpr uint32_t ATTR_FLAG_DETACHED  = 0x1;

void guest_thread_main(std::shared_ptr<GuestThread> gt, uint64_t entry, uint64_t arg, size_t stack_size) {
    t_guest_tid = gt->id;
    {
        lcs::cpu::DynarmicCore core;
        if (core.initialize_thread(stack_size)) {
            lcs::cpu::HLEManager::instance().wire_to_cpu(core);
            gt->retval = core.call(entry, {arg});
            run_key_destructors(core);
        } else {
            std::cerr << "[THREAD] Guest thread " << gt->id << " failed to initialize its CPU core.\n";
        }
    }
    gt->finished = true;
    thread_registry().running--;

    if (gt->detached) {
        std::lock_guard<std::mutex> lock(thread_registry().m);
        thread_registry().threads.erase(gt->id);
    }
}

std::shared_ptr<GuestThread> find_thread(uint64_t id) {
    std::lock_guard<std::mutex> lock(thread_registry().m);
    auto it = thread_registry().threads.find(id);
    return it != thread_registry().threads.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
// C++ ABI static-local guards (Itanium: byte 0 = initialized).
// ---------------------------------------------------------------------------
struct GuardState {
    std::mutex m;
    std::condition_variable cv;
    std::unordered_map<uint64_t, uint64_t> in_progress; // guard addr -> owner tid
};

GuardState& guard_state() { return *immortal<GuardState>(); }

} // namespace

uint64_t current_guest_tid() {
    return t_guest_tid;
}

int* current_errno_ptr() {
    return &t_errno;
}

uint64_t running_guest_thread_count() {
    return thread_registry().running.load();
}

void register_thread_hle(lcs::cpu::HLEManager& hle) {
    using lcs::cpu::DynarmicCore;

    // --- Threads -------------------------------------------------------------
    hle.register_function("pthread_create", [](const uint64_t* args) -> uint64_t {
        uint64_t* out = reinterpret_cast<uint64_t*>(args[0]);
        const uint8_t* attr = reinterpret_cast<const uint8_t*>(args[1]);
        uint64_t entry = args[2];
        uint64_t arg = args[3];
        if (!entry) return G_EINVAL;

        size_t stack_size = DEFAULT_STACK;
        bool detached = false;
        if (attr) {
            uint32_t flags = 0;
            uint64_t requested = 0;
            std::memcpy(&flags, attr + ATTR_OFF_FLAGS, 4);
            std::memcpy(&requested, attr + ATTR_OFF_STACK_SIZE, 8);
            detached = (flags & ATTR_FLAG_DETACHED) != 0;
            if (requested >= MIN_STACK && requested <= MAX_STACK) stack_size = static_cast<size_t>(requested);
        }

        auto gt = std::make_shared<GuestThread>();
        gt->id = thread_registry().next_id++;
        gt->detached = detached;
        if (out) *out = gt->id; // visible to the guest before the thread runs
        {
            std::lock_guard<std::mutex> lock(thread_registry().m);
            thread_registry().threads[gt->id] = gt;
        }
        thread_registry().running++;

        std::cout << "[THREAD] pthread_create id=" << gt->id << " entry=0x" << std::hex << entry
                  << " arg=0x" << arg << std::dec << " stack=" << (stack_size >> 10) << "KB"
                  << (detached ? " detached" : "") << "\n";

        try {
            gt->host = std::thread(guest_thread_main, gt, entry, arg, stack_size);
        } catch (const std::exception& e) {
            std::cerr << "[THREAD] Host thread creation failed: " << e.what() << "\n";
            thread_registry().running--;
            std::lock_guard<std::mutex> lock(thread_registry().m);
            thread_registry().threads.erase(gt->id);
            return G_EAGAIN;
        }
        if (detached) gt->host.detach();
        return 0;
    });

    hle.register_function("pthread_join", [](const uint64_t* args) -> uint64_t {
        uint64_t id = args[0];
        uint64_t* retval_out = reinterpret_cast<uint64_t*>(args[1]);
        if (id == t_guest_tid) return G_EDEADLK;
        auto gt = find_thread(id);
        if (!gt) return G_ESRCH;
        if (gt->host.joinable()) gt->host.join();
        if (retval_out) *retval_out = gt->retval;
        std::lock_guard<std::mutex> lock(thread_registry().m);
        thread_registry().threads.erase(id);
        return 0;
    });

    hle.register_function("pthread_detach", [](const uint64_t* args) -> uint64_t {
        auto gt = find_thread(args[0]);
        if (!gt) return G_ESRCH;
        gt->detached = true;
        if (gt->host.joinable()) gt->host.detach();
        if (gt->finished) {
            std::lock_guard<std::mutex> lock(thread_registry().m);
            thread_registry().threads.erase(args[0]);
        }
        return 0;
    });

    hle.register_function("pthread_exit", [](const uint64_t* args) -> uint64_t {
        DynarmicCore* core = DynarmicCore::current();
        if (core && t_guest_tid != MAIN_THREAD_ID) {
            core->request_exit();
        } else {
            std::cerr << "[THREAD] pthread_exit called on the main thread; ignored.\n";
        }
        return args[0];
    });

    hle.register_function("pthread_self", [](const uint64_t*) -> uint64_t { return t_guest_tid; });
    hle.register_function("pthread_equal", [](const uint64_t* args) -> uint64_t { return args[0] == args[1] ? 1 : 0; });
    hle.register_function("gettid", [](const uint64_t*) -> uint64_t { return 999 + t_guest_tid; });
    hle.register_function("pthread_gettid_np", [](const uint64_t* args) -> uint64_t { return 999 + args[0]; });
    hle.register_function("pthread_setname_np", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_getname_np", [](const uint64_t* args) -> uint64_t {
        char* buf = reinterpret_cast<char*>(args[1]);
        if (buf && args[2] > 0) buf[0] = '\0';
        return 0;
    });
    hle.register_function("pthread_setschedparam", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_getschedparam", [](const uint64_t* args) -> uint64_t {
        if (args[1]) *reinterpret_cast<int32_t*>(args[1]) = 0;
        if (args[2]) *reinterpret_cast<int32_t*>(args[2]) = 0;
        return 0;
    });
    hle.register_function("sched_yield", [](const uint64_t*) -> uint64_t {
        std::this_thread::yield();
        return 0;
    });
    hle.register_function("__errno", [](const uint64_t*) -> uint64_t {
        return reinterpret_cast<uint64_t>(&t_errno);
    });

    // --- Thread attributes -----------------------------------------------------
    hle.register_function("pthread_attr_init", [](const uint64_t* args) -> uint64_t {
        uint8_t* attr = reinterpret_cast<uint8_t*>(args[0]);
        if (!attr) return G_EINVAL;
        std::memset(attr, 0, ATTR_SIZE);
        uint64_t def = DEFAULT_STACK;
        std::memcpy(attr + ATTR_OFF_STACK_SIZE, &def, 8);
        return 0;
    });
    hle.register_function("pthread_attr_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_attr_setdetachstate", [](const uint64_t* args) -> uint64_t {
        uint8_t* attr = reinterpret_cast<uint8_t*>(args[0]);
        if (!attr) return G_EINVAL;
        uint32_t flags = 0;
        std::memcpy(&flags, attr + ATTR_OFF_FLAGS, 4);
        if (args[1] == 1) flags |= ATTR_FLAG_DETACHED; else flags &= ~ATTR_FLAG_DETACHED;
        std::memcpy(attr + ATTR_OFF_FLAGS, &flags, 4);
        return 0;
    });
    hle.register_function("pthread_attr_getdetachstate", [](const uint64_t* args) -> uint64_t {
        const uint8_t* attr = reinterpret_cast<const uint8_t*>(args[0]);
        int32_t* out = reinterpret_cast<int32_t*>(args[1]);
        if (!attr || !out) return G_EINVAL;
        uint32_t flags = 0;
        std::memcpy(&flags, attr + ATTR_OFF_FLAGS, 4);
        *out = (flags & ATTR_FLAG_DETACHED) ? 1 : 0;
        return 0;
    });
    hle.register_function("pthread_attr_setstacksize", [](const uint64_t* args) -> uint64_t {
        uint8_t* attr = reinterpret_cast<uint8_t*>(args[0]);
        if (!attr) return G_EINVAL;
        uint64_t size = args[1];
        std::memcpy(attr + ATTR_OFF_STACK_SIZE, &size, 8);
        return 0;
    });
    hle.register_function("pthread_attr_getstacksize", [](const uint64_t* args) -> uint64_t {
        const uint8_t* attr = reinterpret_cast<const uint8_t*>(args[0]);
        uint64_t* out = reinterpret_cast<uint64_t*>(args[1]);
        if (!attr || !out) return G_EINVAL;
        std::memcpy(out, attr + ATTR_OFF_STACK_SIZE, 8);
        return 0;
    });
    hle.register_function("pthread_attr_setschedparam", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_attr_setschedpolicy", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_attr_setguardsize", [](const uint64_t*) -> uint64_t { return 0; });

    // --- Mutexes ---------------------------------------------------------------
    hle.register_function("pthread_mutexattr_init", [](const uint64_t* args) -> uint64_t {
        if (args[0]) *reinterpret_cast<int32_t*>(args[0]) = 0;
        return 0;
    });
    hle.register_function("pthread_mutexattr_settype", [](const uint64_t* args) -> uint64_t {
        if (args[0]) *reinterpret_cast<int32_t*>(args[0]) = static_cast<int32_t>(args[1]);
        return 0;
    });
    hle.register_function("pthread_mutexattr_gettype", [](const uint64_t* args) -> uint64_t {
        if (args[0] && args[1]) *reinterpret_cast<int32_t*>(args[1]) = *reinterpret_cast<const int32_t*>(args[0]);
        return 0;
    });
    hle.register_function("pthread_mutexattr_setpshared", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_mutexattr_destroy", [](const uint64_t*) -> uint64_t { return 0; });

    hle.register_function("pthread_mutex_init", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        GuestMutex& gm = mutexes().get(args[0]);
        std::lock_guard<std::mutex> lk(gm.m);
        if (gm.count == 0) gm.owner = 0;
        return 0;
    });
    hle.register_function("pthread_mutex_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_mutex_lock", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        return mutex_lock(mutexes().get(args[0]), nullptr);
    });
    hle.register_function("pthread_mutex_timedlock", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        Clock::time_point deadline = to_deadline(args[1]);
        return mutex_lock(mutexes().get(args[0]), &deadline);
    });
    hle.register_function("pthread_mutex_trylock", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        return mutex_trylock(mutexes().get(args[0]));
    });
    hle.register_function("pthread_mutex_unlock", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        return mutex_unlock(mutexes().get(args[0]));
    });

    // --- Condition variables ---------------------------------------------------
    hle.register_function("pthread_condattr_init", [](const uint64_t* args) -> uint64_t {
        if (args[0]) *reinterpret_cast<int32_t*>(args[0]) = 0;
        return 0;
    });
    hle.register_function("pthread_condattr_setclock", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_condattr_setpshared", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_condattr_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_cond_init", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        conds().get(args[0]);
        return 0;
    });
    hle.register_function("pthread_cond_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("pthread_cond_signal", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        conds().get(args[0]).cv.notify_one();
        return 0;
    });
    hle.register_function("pthread_cond_broadcast", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        conds().get(args[0]).cv.notify_all();
        return 0;
    });
    hle.register_function("pthread_cond_wait", [](const uint64_t* args) -> uint64_t {
        if (!args[0] || !args[1]) return G_EINVAL;
        return cond_wait(conds().get(args[0]), mutexes().get(args[1]), nullptr);
    });
    auto timedwait_abs = [](const uint64_t* args) -> uint64_t {
        if (!args[0] || !args[1]) return G_EINVAL;
        Clock::time_point deadline = to_deadline(args[2]);
        return cond_wait(conds().get(args[0]), mutexes().get(args[1]), &deadline);
    };
    hle.register_function("pthread_cond_timedwait", timedwait_abs);
    hle.register_function("pthread_cond_timedwait_monotonic_np", timedwait_abs);
    hle.register_function("pthread_cond_timedwait_monotonic", timedwait_abs);
    hle.register_function("pthread_cond_clockwait", [](const uint64_t* args) -> uint64_t {
        if (!args[0] || !args[1]) return G_EINVAL;
        Clock::time_point deadline = to_deadline(args[3]);
        return cond_wait(conds().get(args[0]), mutexes().get(args[1]), &deadline);
    });
    hle.register_function("pthread_cond_timedwait_relative_np", [](const uint64_t* args) -> uint64_t {
        if (!args[0] || !args[1]) return G_EINVAL;
        Clock::time_point deadline = to_relative_deadline(args[2]);
        return cond_wait(conds().get(args[0]), mutexes().get(args[1]), &deadline);
    });

    // --- Read/write locks (exclusive, recursive) ------------------------------
    hle.register_function("pthread_rwlock_init", [](const uint64_t* args) -> uint64_t {
        if (args[0]) rwlocks().get(args[0]);
        return 0;
    });
    hle.register_function("pthread_rwlock_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    auto rw_lock = [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        return mutex_lock(rwlocks().get(args[0]), nullptr);
    };
    auto rw_trylock = [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        return mutex_trylock(rwlocks().get(args[0]));
    };
    hle.register_function("pthread_rwlock_rdlock", rw_lock);
    hle.register_function("pthread_rwlock_wrlock", rw_lock);
    hle.register_function("pthread_rwlock_tryrdlock", rw_trylock);
    hle.register_function("pthread_rwlock_trywrlock", rw_trylock);
    hle.register_function("pthread_rwlock_unlock", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return G_EINVAL;
        return mutex_unlock(rwlocks().get(args[0]));
    });

    // --- pthread_once ------------------------------------------------------------
    hle.register_function("pthread_once", [](const uint64_t* args) -> uint64_t {
        int32_t* control = reinterpret_cast<int32_t*>(args[0]);
        uint64_t routine = args[1];
        if (!control) return G_EINVAL;
        std::atomic_ref<int32_t> state(*control);
        int32_t expected = 0;
        if (state.compare_exchange_strong(expected, 1)) {
            DynarmicCore* core = DynarmicCore::current();
            if (routine && core) core->call_nested(routine);
            state.store(2);
            return 0;
        }
        while (state.load() != 2) std::this_thread::yield();
        return 0;
    });

    // --- pthread keys ------------------------------------------------------------
    hle.register_function("pthread_key_create", [](const uint64_t* args) -> uint64_t {
        uint32_t* key_out = reinterpret_cast<uint32_t*>(args[0]);
        if (!key_out) return G_EINVAL;
        std::lock_guard<std::mutex> lock(key_table().m);
        uint32_t key = key_table().next_key++;
        key_table().destructors[key] = args[1];
        *key_out = key;
        return 0;
    });
    hle.register_function("pthread_key_delete", [](const uint64_t* args) -> uint64_t {
        std::lock_guard<std::mutex> lock(key_table().m);
        key_table().destructors.erase(static_cast<uint32_t>(args[0]));
        return 0;
    });
    hle.register_function("pthread_setspecific", [](const uint64_t* args) -> uint64_t {
        t_key_values[static_cast<uint32_t>(args[0])] = args[1];
        return 0;
    });
    hle.register_function("pthread_getspecific", [](const uint64_t* args) -> uint64_t {
        auto it = t_key_values.find(static_cast<uint32_t>(args[0]));
        return it != t_key_values.end() ? it->second : 0;
    });

    // --- Semaphores (sem_t is a 4-byte counter in Bionic; host-backed here) ----
    hle.register_function("sem_init", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return set_errno_fail(static_cast<int>(G_EINVAL));
        GuestSemaphore& s = semaphores().get(args[0]);
        std::lock_guard<std::mutex> lk(s.m);
        s.count = static_cast<uint32_t>(args[2]);
        return 0;
    });
    hle.register_function("sem_destroy", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("sem_post", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return set_errno_fail(static_cast<int>(G_EINVAL));
        GuestSemaphore& s = semaphores().get(args[0]);
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.count++;
        }
        s.cv.notify_one();
        return 0;
    });
    hle.register_function("sem_wait", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return set_errno_fail(static_cast<int>(G_EINVAL));
        return sem_wait_impl(semaphores().get(args[0]), nullptr);
    });
    hle.register_function("sem_timedwait", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return set_errno_fail(static_cast<int>(G_EINVAL));
        Clock::time_point deadline = to_deadline(args[1]);
        return sem_wait_impl(semaphores().get(args[0]), &deadline);
    });
    hle.register_function("sem_trywait", [](const uint64_t* args) -> uint64_t {
        if (!args[0]) return set_errno_fail(static_cast<int>(G_EINVAL));
        GuestSemaphore& s = semaphores().get(args[0]);
        std::lock_guard<std::mutex> lk(s.m);
        if (s.count == 0) return set_errno_fail(static_cast<int>(G_EAGAIN));
        s.count--;
        return 0;
    });
    hle.register_function("sem_getvalue", [](const uint64_t* args) -> uint64_t {
        if (!args[0] || !args[1]) return set_errno_fail(static_cast<int>(G_EINVAL));
        GuestSemaphore& s = semaphores().get(args[0]);
        std::lock_guard<std::mutex> lk(s.m);
        *reinterpret_cast<int32_t*>(args[1]) = static_cast<int32_t>(s.count);
        return 0;
    });

    // Named POSIX semaphores (used by CdStreamThread in libGame.so)
    struct NamedSemaphoreTable {
        std::mutex m;
        std::unordered_map<std::string, uint64_t> by_name;
        uint64_t next_handle = 0x7f60500000ULL;
    };
    auto* named_sems = immortal<NamedSemaphoreTable>();

    hle.register_function("sem_open", [named_sems](const uint64_t* args) -> uint64_t {
        const char* name = reinterpret_cast<const char*>(args[0]);
        if (!name) return set_errno_fail(static_cast<int>(G_EINVAL));
        std::string key(name);
        uint64_t handle = 0;
        bool created = false;
        {
            std::lock_guard<std::mutex> lk(named_sems->m);
            auto it = named_sems->by_name.find(key);
            if (it != named_sems->by_name.end()) {
                handle = it->second;
            } else {
                handle = named_sems->next_handle;
                named_sems->next_handle += 16;
                named_sems->by_name[key] = handle;
                created = true;
            }
        }
        if (created) {
            GuestSemaphore& s = semaphores().get(handle);
            std::lock_guard<std::mutex> lk(s.m);
            s.count = static_cast<uint32_t>(args[3]);
        }
        return handle;
    });
    hle.register_function("sem_close", [](const uint64_t*) -> uint64_t { return 0; });
    hle.register_function("sem_unlink", [named_sems](const uint64_t* args) -> uint64_t {
        const char* name = reinterpret_cast<const char*>(args[0]);
        if (!name) return set_errno_fail(static_cast<int>(G_EINVAL));
        std::lock_guard<std::mutex> lk(named_sems->m);
        named_sems->by_name.erase(name);
        return 0;
    });

    hle.register_function("nanosleep", [](const uint64_t* args) -> uint64_t {
        const int64_t* req = reinterpret_cast<const int64_t*>(args[0]);
        if (!req || req[0] < 0 || req[1] < 0 || req[1] >= 1000000000LL) {
            return set_errno_fail(static_cast<int>(G_EINVAL));
        }
        const uint64_t ns = static_cast<uint64_t>(req[0]) * 1'000'000'000ULL +
                            static_cast<uint64_t>(req[1]);
        if (ns > 0) {
            lcs::cpu::HLEManager::high_res_sleep_ns(ns);
        }
        return 0;
    });

    // --- C++ ABI thread-safe static initialization guards ---------------------
    hle.register_function("__cxa_guard_acquire", [](const uint64_t* args) -> uint64_t {
        uint8_t* guard = reinterpret_cast<uint8_t*>(args[0]);
        if (!guard) return 0;
        const uint64_t tid = t_guest_tid;
        GuardState& gs = guard_state();
        std::unique_lock<std::mutex> lk(gs.m);
        for (;;) {
            if (std::atomic_ref<uint8_t>(*guard).load() != 0) return 0;
            auto it = gs.in_progress.find(args[0]);
            if (it == gs.in_progress.end()) {
                gs.in_progress[args[0]] = tid;
                return 1;
            }
            if (it->second == tid) return 1; // recursive init (UB in C++); do not deadlock
            gs.cv.wait(lk);
        }
    });
    hle.register_function("__cxa_guard_release", [](const uint64_t* args) -> uint64_t {
        uint8_t* guard = reinterpret_cast<uint8_t*>(args[0]);
        if (!guard) return 0;
        GuardState& gs = guard_state();
        {
            std::lock_guard<std::mutex> lk(gs.m);
            std::atomic_ref<uint8_t>(*guard).store(1);
            gs.in_progress.erase(args[0]);
        }
        gs.cv.notify_all();
        return 0;
    });
    hle.register_function("__cxa_guard_abort", [](const uint64_t* args) -> uint64_t {
        GuardState& gs = guard_state();
        {
            std::lock_guard<std::mutex> lk(gs.m);
            gs.in_progress.erase(args[0]);
        }
        gs.cv.notify_all();
        return 0;
    });
}

} // namespace lcs::os
