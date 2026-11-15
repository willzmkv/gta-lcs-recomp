#include <cstdint>

namespace lcs::cpu {
    void init_dynarmic() {}
    void run_code(uint64_t entry_point) { (void)entry_point; }
    void stop_dynarmic() {}
}
