#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <chrono>

namespace lcs::cpu {
    class DynarmicCore;
    class HLEManager;
}

namespace lcs::graphics {
    class VideoPlayer;
}

namespace lcs::os {

// Fixed Guest Virtual Addresses for JNI Subsystem
inline constexpr uint64_t JNI_GUEST_VM_ADDR           = 0x7f60300000ULL;
inline constexpr uint64_t JNI_GUEST_ENV_ADDR          = 0x7f60301000ULL;
inline constexpr uint64_t JNI_GUEST_INVOKE_TABLE      = 0x7f60302000ULL;
inline constexpr uint64_t JNI_GUEST_NATIVE_TABLE      = 0x7f60303000ULL;
inline constexpr uint64_t JNI_GUEST_CLASS_BASE        = 0x7f60305000ULL;
inline constexpr uint64_t JNI_GUEST_METHOD_BASE       = 0x7f60306000ULL;
inline constexpr uint64_t JNI_GUEST_HEAP_BASE         = 0x7f60310000ULL;
inline constexpr size_t   JNI_GUEST_HEAP_SIZE         = 0x000f0000ULL; // ~1 MB

struct JNIMethodInfo {
    uint32_t id = 0;
    std::string className;
    std::string methodName;
    std::string signature;
    bool isStatic = false;
    std::string returnType;
    uint64_t nativeFnPtr = 0; // If registered via RegisterNatives
};

struct JNIClassInfo {
    uint32_t id = 0;
    std::string name;
    uint64_t guest_addr = 0;
};

class JNIBridge {
public:
    static JNIBridge& instance();

    bool initialize(cpu::DynarmicCore& cpu, cpu::HLEManager& hle);

    uint64_t get_vm_address() const { return JNI_GUEST_VM_ADDR; }
    uint64_t get_env_address() const { return JNI_GUEST_ENV_ADDR; }

    // Helpers to create guest objects
    uint64_t create_string_utf(const std::string& str);
    const char* get_string_utf_chars(uint64_t string_obj);
    std::string resolve_jstring(uint64_t jstring_handle);
    uint64_t create_object_array(const std::vector<uint64_t>& elements);

    // Callbacks from libGame.so
    void set_userdata_dir(const std::string& path) { m_userdata_dir = path; }

    struct PendingCallbacks {
        bool playlist_finish_init = false;
        bool game_loaded = false;
        bool start_game_before_load = false;
        bool start_game = false;
        bool video_finished = false;
        bool quit_requested = false;
    };
    PendingCallbacks consume_pending_callbacks();

    // Video playback lifecycle and skip control
    void update_video(bool smoke_test = false);
    void skip_video();
    bool is_video_playing() const;

private:
    JNIBridge();
    ~JNIBridge();

    void setup_known_classes_and_methods();
    void build_invoke_table(cpu::HLEManager& hle);
    void build_native_table(cpu::HLEManager& hle);

    // JNI Dispatch implementations
    uint64_t handle_find_class(const char* name);
    uint64_t handle_get_method_id(uint64_t clazz, const char* name, const char* sig, bool is_static);
    uint64_t handle_call_method(uint64_t target, uint64_t method_id, const uint64_t* args, bool is_static, bool is_va);
    uint64_t handle_register_natives(uint64_t clazz, uint64_t methods_array, int32_t n_methods);

    cpu::DynarmicCore* m_cpu = nullptr;
    void* m_memory_block = nullptr;
    size_t m_heap_bump = 0;

    std::string m_userdata_dir = "gtalcs/userdata";

    std::vector<JNIClassInfo> m_classes;
    std::unordered_map<std::string, uint32_t> m_class_name_to_id;

    std::vector<JNIMethodInfo> m_methods;
    std::unordered_map<std::string, uint32_t> m_method_key_to_id;

    // Allocated guest strings cache
    std::unordered_map<uint64_t, std::string> m_string_objects;
    std::unordered_map<uint64_t, std::vector<uint8_t>> m_byte_arrays;
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_object_arrays;
    PendingCallbacks m_pending;
    std::unique_ptr<lcs::graphics::VideoPlayer> m_video_player;
    std::vector<uint8_t> m_video_frame_rgba;
    std::vector<int16_t> m_video_audio_pcm;
    std::chrono::steady_clock::time_point m_video_start_time{};
    int64_t m_video_last_pts_100ns = -1;
    int64_t m_video_audio_last_pts_100ns = -1;
    uint64_t m_video_frame_count = 0;
    bool m_video_has_first_frame = false;
};

} // namespace lcs::os
