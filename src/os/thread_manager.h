#pragma once

#include <cstdint>

namespace lcs::cpu {
class HLEManager;
}

namespace lcs::os {

// Registers the Bionic pthread / semaphore / C++ ABI guard HLE implementations.
// Every guest pthread runs on its own host std::thread with a dedicated Dynarmic
// core (own stack, TLS block and exclusive monitor slot). Guest mutexes, condition
// variables, semaphores and rwlocks are keyed by guest address and backed by host
// primitives, created lazily so static initializers (all-zero) work.
void register_thread_hle(lcs::cpu::HLEManager& hle);

// Guest-visible thread id of the calling host thread (main thread = 1).
uint64_t current_guest_tid();

// Per-thread errno storage returned by the __errno HLE.
int* current_errno_ptr();

// Number of guest threads still running (diagnostics).
uint64_t running_guest_thread_count();

} // namespace lcs::os
