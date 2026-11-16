#pragma once
#include <cstdint>
#include <string_view>
#include <cstdio>

namespace lcs::os {

// Initialize Bionic libc data structures and tables
void init_bionic_libc();

// Resolves Bionic libc exported data symbols (_ctype_, __sF, stdout, stderr, stdin)
// Returns non-zero address if resolved, 0 otherwise
uint64_t resolve_bionic_libc_data(std::string_view name);

// Translates a guest FILE* pointer (handling stdin, stdout, stderr buffers) to host FILE*
FILE* translate_guest_file_ptr(uint64_t guest_fp);

} // namespace lcs::os
