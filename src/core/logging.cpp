#include <iostream>

namespace lcs::core {
    void log_info(const char* msg) { std::cout << "[INFO] " << msg << "\n"; }
    void log_error(const char* msg) { std::cerr << "[ERROR] " << msg << "\n"; }
}
