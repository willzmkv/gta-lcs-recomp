#include "jni_bridge.h"
#include "cpu/dynarmic_core.h"
#include "cpu/callbacks.h"
#include "audio/audio_system.h"
#include "graphics/video_player.h"
#include "graphics/render_bridge.h"
#include "graphics/window.h"
#include "settings/settings.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <ratio>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace lcs::os {

JNIBridge& JNIBridge::instance() {
    static JNIBridge s_instance;
    return s_instance;
}

JNIBridge::JNIBridge() = default;

JNIBridge::~JNIBridge() {
    if (m_video_player) {
        m_video_player->close();
    }
    if (m_memory_block) {
#if defined(LCS_PLATFORM_WINDOWS)
        VirtualFree(m_memory_block, 0, MEM_RELEASE);
#else
        munmap(m_memory_block, 0x100000);
#endif
        m_memory_block = nullptr;
    }
}

bool JNIBridge::initialize(cpu::DynarmicCore& cpu, cpu::HLEManager& hle) {
    if (m_memory_block) return true;
    m_cpu = &cpu;

    // Allocate 1 MB for JNI data, tables, and heap at 0x7f60300000
#if defined(LCS_PLATFORM_WINDOWS)
    m_memory_block = VirtualAlloc(reinterpret_cast<void*>(JNI_GUEST_VM_ADDR),
                                  0x100000,
                                  MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
#else
    m_memory_block = mmap(reinterpret_cast<void*>(JNI_GUEST_VM_ADDR),
                          0x100000,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif

    if (!m_memory_block) {
        std::cerr << "[JNI] Failed to allocate JNI virtual memory block at 0x"
                  << std::hex << JNI_GUEST_VM_ADDR << std::dec << "\n";
        return false;
    }

    // JavaVM struct points to JNIInvokeInterface
    *reinterpret_cast<uint64_t*>(JNI_GUEST_VM_ADDR) = JNI_GUEST_INVOKE_TABLE;

    // JNIEnv struct points to JNINativeInterface
    *reinterpret_cast<uint64_t*>(JNI_GUEST_ENV_ADDR) = JNI_GUEST_NATIVE_TABLE;

    setup_known_classes_and_methods();
    build_invoke_table(hle);
    build_native_table(hle);

    std::cout << "  - JNI 1.6 Bridge initialized.\n";
    std::cout << "    * JavaVM at: 0x" << std::hex << JNI_GUEST_VM_ADDR
              << " -> InvokeInterface at 0x" << JNI_GUEST_INVOKE_TABLE << "\n";
    std::cout << "    * JNIEnv at: 0x" << JNI_GUEST_ENV_ADDR
              << " -> NativeInterface at 0x" << JNI_GUEST_NATIVE_TABLE << std::dec << "\n";
    std::cout << "    * Registered " << m_classes.size() << " classes and "
              << m_methods.size() << " pre-bound methods.\n";

    // Initialize video player for cutscenes
    m_video_player = std::make_unique<lcs::graphics::VideoPlayer>();

    return true;
}

void JNIBridge::build_invoke_table(cpu::HLEManager& hle) {
    uint64_t* invoke_table = reinterpret_cast<uint64_t*>(JNI_GUEST_INVOKE_TABLE);

    // GetEnv (offset 0x30 / index 6)
    uint64_t get_env_trampoline = hle.register_function("JavaVM::GetEnv", [](const uint64_t* args) -> uint64_t {
        uint64_t vm = args[0];
        uint64_t env_out_ptr = args[1];
        uint32_t version = static_cast<uint32_t>(args[2]);

        (void)vm;
        if (env_out_ptr) {
            *reinterpret_cast<uint64_t*>(env_out_ptr) = JNI_GUEST_ENV_ADDR;
        }

        static bool logged = false;
        if (!logged) {
            logged = true;
            std::cout << "[JNI] JavaVM::GetEnv(version=0x" << std::hex << version << ") -> JNI_OK\n" << std::dec;
        }
        return 0; // JNI_OK
    });

    // AttachCurrentThread (offset 0x20 / index 4)
    uint64_t attach_thread_trampoline = hle.register_function("JavaVM::AttachCurrentThread", [](const uint64_t* args) -> uint64_t {
        uint64_t env_out_ptr = args[1];
        if (env_out_ptr) {
            *reinterpret_cast<uint64_t*>(env_out_ptr) = JNI_GUEST_ENV_ADDR;
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::cout << "[JNI] JavaVM::AttachCurrentThread() -> JNI_OK\n";
        }
        return 0; // JNI_OK
    });

    // DetachCurrentThread (offset 0x28 / index 5)
    uint64_t detach_thread_trampoline = hle.register_function("JavaVM::DetachCurrentThread", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0; // JNI_OK
    });

    // DestroyJavaVM (offset 0x18 / index 3)
    uint64_t destroy_vm_trampoline = hle.register_function("JavaVM::DestroyJavaVM", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0; // JNI_OK
    });

    invoke_table[0] = 0; // reserved0
    invoke_table[1] = 0; // reserved1
    invoke_table[2] = 0; // reserved2
    invoke_table[3] = destroy_vm_trampoline;
    invoke_table[4] = attach_thread_trampoline;
    invoke_table[5] = detach_thread_trampoline;
    invoke_table[6] = get_env_trampoline;
    invoke_table[7] = attach_thread_trampoline; // AttachCurrentThreadAsDaemon
}

void JNIBridge::build_native_table(cpu::HLEManager& hle) {
    uint64_t* native_table = reinterpret_cast<uint64_t*>(JNI_GUEST_NATIVE_TABLE);

    // Default stub for any unmapped slot
    uint64_t unmapped_stub = hle.register_function("JNI::UnmappedSlot", [](const uint64_t* args) -> uint64_t {
        std::cout << "[JNI STUB] Unmapped JNI method slot invoked!\n";
        return 0;
    });

    for (size_t i = 0; i < 256; i++) {
        native_table[i] = unmapped_stub;
    }

    // Slot 6 (+0x30): FindClass(env, name)
    native_table[6] = hle.register_function("JNI::FindClass", [this](const uint64_t* args) -> uint64_t {
        const char* name = reinterpret_cast<const char*>(args[1]);
        return handle_find_class(name);
    });

    // Slot 21 (+0xa8): NewGlobalRef(env, obj)
    native_table[21] = hle.register_function("JNI::NewGlobalRef", [](const uint64_t* args) -> uint64_t {
        return args[1]; // pass-through handle
    });

    // Slot 22 (+0xb0): DeleteGlobalRef(env, obj)
    native_table[22] = hle.register_function("JNI::DeleteGlobalRef", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0;
    });

    // Slot 23 (+0xb8): DeleteLocalRef(env, obj)
    native_table[23] = hle.register_function("JNI::DeleteLocalRef", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0;
    });

    // Slot 33 (+0x108): GetMethodID(env, clazz, name, sig)
    native_table[33] = hle.register_function("JNI::GetMethodID", [this](const uint64_t* args) -> uint64_t {
        uint64_t clazz = args[1];
        const char* name = reinterpret_cast<const char*>(args[2]);
        const char* sig = reinterpret_cast<const char*>(args[3]);
        return handle_get_method_id(clazz, name, sig, false);
    });

    // Slot 113 (+0x388): GetStaticMethodID(env, clazz, name, sig)
    native_table[113] = hle.register_function("JNI::GetStaticMethodID", [this](const uint64_t* args) -> uint64_t {
        uint64_t clazz = args[1];
        const char* name = reinterpret_cast<const char*>(args[2]);
        const char* sig = reinterpret_cast<const char*>(args[3]);
        return handle_get_method_id(clazz, name, sig, true);
    });

    // Method Invocations (Static and Instance)
    auto dispatch_call = [this](bool is_static, bool is_va) {
        return [this, is_static, is_va](const uint64_t* args) -> uint64_t {
            uint64_t target = args[1]; // clazz or obj
            uint64_t method_id = args[2];
            return handle_call_method(target, method_id, args, is_static, is_va);
        };
    };

    uint64_t call_static_method   = hle.register_function("JNI::CallStaticMethod", dispatch_call(true, false));
    uint64_t call_static_method_v = hle.register_function("JNI::CallStaticMethodV", dispatch_call(true, true));
    uint64_t call_inst_method     = hle.register_function("JNI::CallMethod", dispatch_call(false, false));
    uint64_t call_inst_method_v   = hle.register_function("JNI::CallMethodV", dispatch_call(false, true));

    // Hook instance calls
    native_table[34] = call_inst_method;   // CallObjectMethod
    native_table[35] = call_inst_method_v; // CallObjectMethodV
    native_table[37] = call_inst_method;   // CallBooleanMethod
    native_table[38] = call_inst_method_v; // CallBooleanMethodV
    native_table[49] = call_inst_method;   // CallIntMethod
    native_table[50] = call_inst_method_v; // CallIntMethodV
    native_table[52] = call_inst_method;   // CallLongMethod
    native_table[53] = call_inst_method_v; // CallLongMethodV
    native_table[55] = call_inst_method;   // CallFloatMethod
    native_table[56] = call_inst_method_v; // CallFloatMethodV
    native_table[61] = call_inst_method;   // CallVoidMethod
    native_table[62] = call_inst_method_v; // CallVoidMethodV

    // Hook static calls
    native_table[114] = call_static_method;   // CallStaticObjectMethod
    native_table[115] = call_static_method_v; // CallStaticObjectMethodV
    native_table[117] = call_static_method;   // CallStaticBooleanMethod
    native_table[118] = call_static_method_v; // CallStaticBooleanMethodV
    native_table[129] = call_static_method;   // CallStaticIntMethod
    native_table[130] = call_static_method_v; // CallStaticIntMethodV
    native_table[141] = call_static_method;   // CallStaticVoidMethod
    native_table[142] = call_static_method_v; // CallStaticVoidMethodV

    // Strings
    native_table[167] = hle.register_function("JNI::NewStringUTF", [this](const uint64_t* args) -> uint64_t {
        const char* str = reinterpret_cast<const char*>(args[1]);
        return create_string_utf(str ? str : "");
    });

    native_table[168] = hle.register_function("JNI::GetStringUTFLength", [this](const uint64_t* args) -> uint64_t {
        uint64_t str_obj = args[1];
        auto it = m_string_objects.find(str_obj);
        return it != m_string_objects.end() ? it->second.length() : 0;
    });

    native_table[169] = hle.register_function("JNI::GetStringUTFChars", [this](const uint64_t* args) -> uint64_t {
        uint64_t str_obj = args[1];
        uint64_t is_copy = args[2];
        if (is_copy) {
            *reinterpret_cast<uint8_t*>(is_copy) = 0; // false, no copy
        }
        return reinterpret_cast<uint64_t>(get_string_utf_chars(str_obj));
    });

    native_table[170] = hle.register_function("JNI::ReleaseStringUTFChars", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0;
    });

    // Arrays
    native_table[171] = hle.register_function("JNI::GetArrayLength", [this](const uint64_t* args) -> uint64_t {
        uint64_t arr_obj = args[1];
        auto it_obj = m_object_arrays.find(arr_obj);
        if (it_obj != m_object_arrays.end()) return it_obj->second.size();
        auto it_byte = m_byte_arrays.find(arr_obj);
        if (it_byte != m_byte_arrays.end()) return it_byte->second.size();
        return 0;
    });

    native_table[172] = hle.register_function("JNI::NewObjectArray", [this](const uint64_t* args) -> uint64_t {
        int32_t len = static_cast<int32_t>(args[1]);
        uint64_t init_elem = args[3];
        if (len < 0) len = 0;

        uint64_t guest_addr = JNI_GUEST_HEAP_BASE + m_heap_bump;
        m_heap_bump = (m_heap_bump + (len * 8) + 63) & ~63;

        m_object_arrays[guest_addr].resize(len, init_elem);
        return guest_addr;
    });

    native_table[173] = hle.register_function("JNI::GetObjectArrayElement", [this](const uint64_t* args) -> uint64_t {
        uint64_t arr_obj = args[1];
        int32_t index = static_cast<int32_t>(args[2]);
        auto it = m_object_arrays.find(arr_obj);
        if (it != m_object_arrays.end() && index >= 0 && index < static_cast<int32_t>(it->second.size())) {
            return it->second[index];
        }
        return 0;
    });

    native_table[174] = hle.register_function("JNI::SetObjectArrayElement", [this](const uint64_t* args) -> uint64_t {
        uint64_t arr_obj = args[1];
        int32_t index = static_cast<int32_t>(args[2]);
        uint64_t val = args[3];
        auto it = m_object_arrays.find(arr_obj);
        if (it != m_object_arrays.end() && index >= 0 && index < static_cast<int32_t>(it->second.size())) {
            it->second[index] = val;
        }
        return 0;
    });

    native_table[178] = hle.register_function("JNI::NewByteArray", [this](const uint64_t* args) -> uint64_t {
        int32_t len = static_cast<int32_t>(args[1]);
        if (len < 0) len = 0;

        uint64_t guest_addr = JNI_GUEST_HEAP_BASE + m_heap_bump;
        m_heap_bump = (m_heap_bump + len + 63) & ~63;

        m_byte_arrays[guest_addr].resize(len, 0);
        return guest_addr;
    });

    native_table[184] = hle.register_function("JNI::GetByteArrayElements", [](const uint64_t* args) -> uint64_t {
        uint64_t is_copy = args[2];
        if (is_copy) *reinterpret_cast<uint8_t*>(is_copy) = 0;
        return args[1]; // guest memory address itself
    });

    native_table[192] = hle.register_function("JNI::ReleaseByteArrayElements", [](const uint64_t* args) -> uint64_t {
        (void)args;
        return 0;
    });

    // RegisterNatives (slot 215, +0x6b8)
    native_table[215] = hle.register_function("JNI::RegisterNatives", [this](const uint64_t* args) -> uint64_t {
        uint64_t clazz = args[1];
        uint64_t methods_array = args[2];
        int32_t n_methods = static_cast<int32_t>(args[3]);
        return handle_register_natives(clazz, methods_array, n_methods);
    });

    // GetJavaVM (slot 219, +0x6d8)
    native_table[219] = hle.register_function("JNI::GetJavaVM", [](const uint64_t* args) -> uint64_t {
        uint64_t vm_out = args[1];
        if (vm_out) {
            *reinterpret_cast<uint64_t*>(vm_out) = JNI_GUEST_VM_ADDR;
        }
        return 0; // JNI_OK
    });
}

void JNIBridge::setup_known_classes_and_methods() {
    const char* default_classes[] = {
        "com/rockstargames/gtalcs/CommonAPI",
        "com/rockstargames/gtalcs/RockstarJNIlib",
        "com/rockstargames/hal/andHttp",
        "com/rockstargames/hal/andFile",
        "com/rockstargames/hal/andView"
    };

    for (size_t i = 0; i < 5; i++) {
        JNIClassInfo c;
        c.id = static_cast<uint32_t>(i);
        c.name = default_classes[i];
        c.guest_addr = JNI_GUEST_CLASS_BASE + (i * 0x40);

        m_class_name_to_id[c.name] = c.id;
        m_classes.push_back(c);
    }

    // All 66 pre-discovered methods
    struct MethodDef {
        const char* className;
        const char* name;
        const char* sig;
        bool isStatic;
        const char* retType;
    } defs[] = {
        {"com/rockstargames/gtalcs/CommonAPI", "PlayVideo", "(Ljava/lang/String;)V", true, "void"},
        {"com/rockstargames/hal/andVideo", "PlayVideoFile", "(Ljava/lang/String;)V", true, "void"},
        {"com/rockstargames/hal/andVideo", "createVideo", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "PlaylistBeginInit", "(Ljava/lang/String;)V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "PlaylistFinishInit", "(Z)V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "isVideoPlaying", "()Z", true, "bool"},
        {"com/rockstargames/gtalcs/CommonAPI", "PlaylistPlay", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "PlaylistPause", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "PlaylistPlaying", "()Z", true, "bool"},
        {"com/rockstargames/gtalcs/CommonAPI", "PlaylistHasSongs", "()Z", true, "bool"},
        {"com/rockstargames/gtalcs/CommonAPI", "GooglePlaylistAvailable", "()Z", true, "bool"},
        {"com/rockstargames/gtalcs/CommonAPI", "GoToUrl", "(Ljava/lang/String;)V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "ShowLoadingScreen", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "HideLoadingScreen", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "ShowLoadingBar", "(F)V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "ShowLoadingBarProgressHack", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "GetDeviceLanguage", "()Ljava/lang/String;", true, "obj"},
        {"com/rockstargames/gtalcs/CommonAPI", "QuitApp", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "RemoveFrameRateLock", "()V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "setKeepScreenOn", "(Z)V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "setInUI", "(Z)V", true, "void"},
        {"com/rockstargames/gtalcs/CommonAPI", "GetVersionName", "()Ljava/lang/String;", true, "obj"},

        {"com/rockstargames/gtalcs/RockstarJNIlib", "ShowPrompt", "()V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "CheckGate", "()Z", true, "bool"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "ShowGate", "()V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "ShowGateBeforeLoad", "()V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "SignIn", "()V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "SignOut", "()V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "SetLocalePriority", "(Ljava/lang/String;)V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "UpdateRockstarID", "()V", true, "void"},
        {"com/rockstargames/gtalcs/RockstarJNIlib", "GameLoad", "()V", true, "void"},

        {"com/rockstargames/hal/andHttp", "IsNetworkReachable", "()Z", false, "bool"},

        {"com/rockstargames/hal/andFile", "getFile", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B", false, "obj"},
        {"com/rockstargames/hal/andFile", "writeUserFile", "(Ljava/lang/String;Ljava/lang/String;)V", false, "void"},
        {"com/rockstargames/hal/andFile", "readUserFile", "(Ljava/lang/String;)Ljava/lang/String;", false, "obj"},

        {"com/rockstargames/hal/andView", "createView", "(I)Lcom/rockstargames/hal/andView;", false, "obj"},
        {"com/rockstargames/hal/andView", "IsCursorAtEnd", "()Z", false, "bool"},
        {"com/rockstargames/hal/andView", "IsCursorAtStart", "()Z", false, "bool"},
        {"com/rockstargames/hal/andView", "addSubview", "(Lcom/rockstargames/hal/andView;)V", false, "void"},
        {"com/rockstargames/hal/andView", "clipChildren", "(Z)V", false, "void"},
        {"com/rockstargames/hal/andView", "finalize", "()V", false, "void"},
        {"com/rockstargames/hal/andView", "getAlpha", "()F", false, "float"},
        {"com/rockstargames/hal/andView", "getContainer", "()Landroid/view/ViewGroup;", false, "obj"},
        {"com/rockstargames/hal/andView", "getDebugId", "()Ljava/lang/String;", false, "obj"},
        {"com/rockstargames/hal/andView", "getHandle", "()I", false, "int"},
        {"com/rockstargames/hal/andView", "getHidden", "()Z", false, "bool"},
        {"com/rockstargames/hal/andView", "getInnerView", "()Landroid/view/View;", false, "obj"},
        {"com/rockstargames/hal/andView", "getOuterView", "()Landroid/view/View;", false, "obj"},
        {"com/rockstargames/hal/andView", "getView", "()Landroid/view/View;", false, "obj"},
        {"com/rockstargames/hal/andView", "invalidateHierarchy", "()V", false, "void"},
        {"com/rockstargames/hal/andView", "onTouch", "(Landroid/view/View;Landroid/view/MotionEvent;)Z", false, "bool"},
        {"com/rockstargames/hal/andView", "printHierarchy", "(I)V", false, "void"},
        {"com/rockstargames/hal/andView", "recalcLayout", "()V", false, "void"},
        {"com/rockstargames/hal/andView", "removeAllSubviews", "()V", false, "void"},
        {"com/rockstargames/hal/andView", "removeFromSuperview", "()V", false, "void"},
        {"com/rockstargames/hal/andView", "sendViewToBack", "(Lcom/rockstargames/hal/andView;)V", false, "void"},
        {"com/rockstargames/hal/andView", "setAlpha", "(F)V", false, "void"},
        {"com/rockstargames/hal/andView", "setAntiFlicker", "()V", false, "void"},
        {"com/rockstargames/hal/andView", "setBackgroundColour", "(IIII)V", false, "void"},
        {"com/rockstargames/hal/andView", "setBounds", "(FFFFFF)V", false, "void"},
        {"com/rockstargames/hal/andView", "setDebugString", "(Ljava/lang/String;)V", false, "void"},
        {"com/rockstargames/hal/andView", "setHidden", "(Z)V", false, "void"},
        {"com/rockstargames/hal/andView", "setHighlightView", "(Z)V", false, "void"},
        {"com/rockstargames/hal/andView", "setLayerType", "(Z)V", false, "void"},
        {"com/rockstargames/hal/andView", "setTransform", "(FFFFFF)V", false, "void"},
        {"com/rockstargames/hal/andView", "setUserInteractionEnabled", "(Z)V", false, "void"},
        {"com/rockstargames/hal/andView", "setView", "(Landroid/view/View;)V", false, "void"},
        {"com/rockstargames/hal/andView", "toImage", "(Ljava/lang/String;)Lcom/rockstargames/hal/andImage;", false, "obj"}
    };

    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        JNIMethodInfo m;
        m.id = static_cast<uint32_t>(i);
        m.className = defs[i].className;
        m.methodName = defs[i].name;
        m.signature = defs[i].sig;
        m.isStatic = defs[i].isStatic;
        m.returnType = defs[i].retType;

        std::string key = m.className + "." + m.methodName + m.signature;
        m_method_key_to_id[key] = m.id;
        m_methods.push_back(m);
    }
}

uint64_t JNIBridge::handle_find_class(const char* name) {
    if (!name) return 0;

    std::string s(name);
    auto it = m_class_name_to_id.find(s);
    if (it != m_class_name_to_id.end()) {
        uint32_t id = it->second;
        std::cout << "[JNI] FindClass(\"" << s << "\") -> 0x" << std::hex << m_classes[id].guest_addr << std::dec << "\n";
        return m_classes[id].guest_addr;
    }

    // Register dynamically
    uint32_t new_id = static_cast<uint32_t>(m_classes.size());
    JNIClassInfo c;
    c.id = new_id;
    c.name = s;
    c.guest_addr = JNI_GUEST_CLASS_BASE + (new_id * 0x40);

    m_class_name_to_id[s] = new_id;
    m_classes.push_back(c);

    std::cout << "[JNI] FindClass(\"" << s << "\") [NEW] -> 0x" << std::hex << c.guest_addr << std::dec << "\n";
    return c.guest_addr;
}

uint64_t JNIBridge::handle_get_method_id(uint64_t clazz, const char* name, const char* sig, bool is_static) {
    if (!name || !sig) return 0;

    std::string className = "<unknown>";
    for (const auto& c : m_classes) {
        if (c.guest_addr == clazz) {
            className = c.name;
            break;
        }
    }

    std::string key = className + "." + name + sig;
    auto it = m_method_key_to_id.find(key);
    if (it != m_method_key_to_id.end()) {
        uint32_t id = it->second;
        uint64_t handle = JNI_GUEST_METHOD_BASE + (id * 0x40);
        std::cout << "[JNI] Get" << (is_static ? "Static" : "") << "MethodID("
                  << key << ") -> 0x" << std::hex << handle << std::dec << "\n";
        return handle;
    }

    // Register dynamic method
    uint32_t id = static_cast<uint32_t>(m_methods.size());
    JNIMethodInfo m;
    m.id = id;
    m.className = className;
    m.methodName = name;
    m.signature = sig;
    m.isStatic = is_static;

    m_method_key_to_id[key] = id;
    m_methods.push_back(m);

    uint64_t handle = JNI_GUEST_METHOD_BASE + (id * 0x40);
    std::cout << "[JNI] Get" << (is_static ? "Static" : "") << "MethodID("
              << key << ") [DYNAMIC] -> 0x" << std::hex << handle << std::dec << "\n";
    return handle;
}

uint64_t JNIBridge::handle_call_method(uint64_t target, uint64_t method_handle, const uint64_t* args, bool is_static, bool is_va) {
    (void)target;
    // Extract an integer/pointer/object argument (handling AArch64 AAPCS64 va_list GP slots).
    auto extract_arg_u64 = [&](uint32_t arg_index) -> uint64_t {
        uint64_t raw = args[3];
        if (is_va && raw >= 0x7f60000000ULL && raw <= 0x7f601f0000ULL - 32) {
            struct AArch64VaList {
                uint64_t stack;
                uint64_t gr_top;
                uint64_t vr_top;
                int32_t  gr_offs;
                int32_t  vr_offs;
            };
            const auto* va = reinterpret_cast<const AArch64VaList*>(raw);
            int32_t gp_off = va->gr_offs + static_cast<int32_t>(arg_index) * 8;
            uint64_t arg_addr = 0;
            if (va->gr_offs < 0 && gp_off < 0 && gp_off >= -64 && va->gr_top != 0) {
                arg_addr = static_cast<uint64_t>(static_cast<int64_t>(va->gr_top) + gp_off);
            } else if (va->stack != 0) {
                int32_t num_gp_in_regs = (va->gr_offs < 0) ? ((-va->gr_offs) / 8) : 0;
                int32_t stack_slot = static_cast<int32_t>(arg_index) - num_gp_in_regs;
                if (stack_slot >= 0) {
                    arg_addr = va->stack + static_cast<uint64_t>(stack_slot) * 8;
                }
            }
            if (arg_addr != 0) {
                return *reinterpret_cast<const uint64_t*>(arg_addr);
            }
        }
        return (3 + arg_index < 8) ? args[3 + arg_index] : 0;
    };

    uint32_t method_id = static_cast<uint32_t>((method_handle - JNI_GUEST_METHOD_BASE) / 0x40);
    if (method_id >= m_methods.size()) {
        std::cerr << "[JNI] CallMethod on invalid method handle: 0x" << std::hex << method_handle << std::dec << "\n";
        return 0;
    }

    const auto& m = m_methods[method_id];
    std::cout << "[JNI] Call" << (is_static ? "Static" : "") << "Method: "
              << m.className << "." << m.methodName << m.signature << "\n";

    if (m.methodName == "GetDeviceLanguage") {
        static constexpr const char* kLangCodes[] = {"en", "fr", "de", "it", "es", "ru", "ja"};
        const int raw_lang = std::clamp(lcs::settings::get().game_language, 0, 7);
        const int idx = (raw_lang == 7) ? 0 : raw_lang;
        return create_string_utf(kLangCodes[idx]);
    }
    else if (m.methodName == "GetVersionName") {
        return create_string_utf("2.4.379");
    }
    else if (m.methodName == "isVideoPlaying" || m.methodName == "IsVideoPlaying") {
        return is_video_playing() ? 1 : 0;
    }
    else if (m.methodName == "CheckGate") {
        return 0; // Gate passed
    }
    else if (m.methodName == "IsNetworkReachable") {
        return 1; // Network available
    }
    else if (m.methodName == "createView") {
        // Return a dummy view handle
        return JNI_GUEST_HEAP_BASE + 0x2000;
    }
    else if (m.methodName == "PlaylistBeginInit") {
        m_pending.playlist_finish_init = true;
        return 0;
    }
    else if (m.methodName == "GameLoad") {
        m_pending.game_loaded = true;
        return 0;
    }
    else if (m.methodName == "ShowGateBeforeLoad") {
        m_pending.start_game_before_load = true;
        return 0;
    }
    else if (m.methodName == "PlayVideo" || m.methodName == "PlayVideoFile") {
        uint64_t str_handle = extract_arg_u64(0);
        
        std::cout << "[JNI] PlayVideo raw handle: 0x" << std::hex << str_handle << std::dec << "\n";
        
        // Resolve jstring to UTF-8 using our new helper
        std::string video_path = resolve_jstring(str_handle);
        
        std::cout << "[JNI] PlayVideo called with path: " << (video_path.empty() ? "NULL" : video_path.c_str()) << "\n";
        
        if (!video_path.empty() && m_video_player) {
            std::string full_path = "gtalcs/gamefiles/assets/" + video_path;
            if (!std::filesystem::exists(full_path)) {
                std::string movies_path = "gtalcs/gamefiles/assets/Movies/" + video_path;
                if (std::filesystem::exists(movies_path)) {
                    full_path = movies_path;
                }
            }
            
            std::cout << "[JNI] Looking for video at: " << full_path << " (exists: " << std::filesystem::exists(full_path) << ")\n";
            
            if (std::filesystem::exists(full_path)) {
                if (m_video_player->open(full_path, false)) {
                    lcs::audio::clear_video_audio();
                    m_video_player->play();
                    m_video_has_first_frame = false;
                    m_video_last_pts_100ns = -1;
                    m_video_audio_last_pts_100ns = -1;
                    m_video_frame_count = 0;
                    m_video_start_time = std::chrono::steady_clock::now();
                    update_video(false);
                    std::cout << "[JNI] Started video playback: " << full_path << "\n";
                    return 0;
                } else {
                    std::cerr << "[JNI] Failed to open video file: " << full_path << "\n";
                }
            } else {
                std::cerr << "[JNI] Video file not found: " << full_path << "\n";
            }
        }
        
        // Fallback: immediate completion if video fails or not found
        m_pending.video_finished = true;
        return 0;
    }
    else if (m.methodName == "ShowGate") {
        m_pending.start_game = true;
        return 0;
    }
    else if (m.methodName == "ShowLoadingScreen") {
        lcs::graphics::pump_loading_events();
        lcs::graphics::RenderBridge::instance().present_loading_bar(0.0f);
        return 0;
    }
    else if (m.methodName == "HideLoadingScreen") {
        lcs::graphics::RenderBridge::instance().set_loading_bar(false, 0.0f);
        return 0;
    }
    else if (m.methodName == "ShowLoadingBar") {
        float progress = 0.0f;
        uint64_t raw = args[3];
        if (is_va && raw >= 0x7f60000000ULL && raw <= 0x7f601f0000ULL - 32) {
            struct AArch64VaList {
                uint64_t stack;
                uint64_t gr_top;
                uint64_t vr_top;
                int32_t  gr_offs;
                int32_t  vr_offs;
            };
            const auto* va = reinterpret_cast<const AArch64VaList*>(raw);
            if (va->vr_offs < 0 && va->vr_top != 0) {
                const uint64_t faddr = static_cast<uint64_t>(static_cast<int64_t>(va->vr_top) + va->vr_offs);
                double dval = 0.0;
                float fval = 0.0f;
                std::memcpy(&dval, reinterpret_cast<const void*>(faddr), sizeof(double));
                std::memcpy(&fval, reinterpret_cast<const void*>(faddr), sizeof(float));
                if (std::isfinite(dval) && dval >= 0.0001 && dval <= 100.0) {
                    progress = static_cast<float>(dval);
                } else if (std::isfinite(fval) && fval >= 0.0f && fval <= 100.0f) {
                    progress = fval;
                }
            }
        } else if (m_cpu) {
            progress = m_cpu->get_fp_float(0);
        }
        if (!std::isfinite(progress) || progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress *= 0.01f;
        progress = std::clamp(progress, 0.0f, 1.0f);

        lcs::graphics::RenderBridge::instance().set_loading_bar(true, progress);
        static auto s_last_pump = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_pump >= std::chrono::milliseconds(33)) {
            s_last_pump = now;
            lcs::graphics::pump_loading_events();
            lcs::graphics::RenderBridge::instance().present_loading_bar(progress);
        }
        return 0;
    }
    else if (m.methodName == "QuitApp") {
        m_pending.quit_requested = true;
        return 0;
    }
    else if (m.methodName == "readUserFile") {
        uint64_t str_handle = extract_arg_u64(0);
        const char* fname = get_string_utf_chars(str_handle);
        if (fname && fname[0] != '\0') {
            std::filesystem::path fpath = std::filesystem::path(m_userdata_dir) / fname;
            if (std::filesystem::exists(fpath)) {
                std::ifstream ifs(fpath, std::ios::binary);
                if (ifs) {
                    std::string content((std::istreambuf_iterator<char>(ifs)),
                                         std::istreambuf_iterator<char>());
                    return create_string_utf(content);
                }
            }
        }
        return 0; // null, file not found
    }
    else if (m.methodName == "writeUserFile") {
        uint64_t path_handle = extract_arg_u64(0);
        uint64_t data_handle = extract_arg_u64(1);
        const char* fname = get_string_utf_chars(path_handle);
        const char* data = get_string_utf_chars(data_handle);
        if (fname && data) {
            std::filesystem::create_directories(m_userdata_dir);
            std::ofstream ofs(std::filesystem::path(m_userdata_dir) / fname, std::ios::binary);
            if (ofs) {
                ofs << data;
            }
        }
        return 0;
    }

    return 0;
}

uint64_t JNIBridge::handle_register_natives(uint64_t clazz, uint64_t methods_array, int32_t n_methods) {
    std::string className = "<unknown>";
    for (const auto& c : m_classes) {
        if (c.guest_addr == clazz) {
            className = c.name;
            break;
        }
    }

    std::cout << "[JNI] RegisterNatives for " << className << " (count: " << n_methods << "):\n";

    // struct JNINativeMethod { const char* name; const char* signature; void* fnPtr; };
    struct NativeMethodArm64 {
        uint64_t name_ptr;
        uint64_t sig_ptr;
        uint64_t fn_ptr;
    };

    auto* methods = reinterpret_cast<const NativeMethodArm64*>(methods_array);
    for (int32_t i = 0; i < n_methods; i++) {
        const char* mname = reinterpret_cast<const char*>(methods[i].name_ptr);
        const char* msig  = reinterpret_cast<const char*>(methods[i].sig_ptr);
        uint64_t fn_ptr   = methods[i].fn_ptr;

        std::cout << "  [" << i << "] " << (mname ? mname : "")
                  << (msig ? msig : "") << " -> 0x" << std::hex << fn_ptr << std::dec << "\n";

        std::string key = className + "." + (mname ? mname : "") + (msig ? msig : "");
        auto it = m_method_key_to_id.find(key);
        if (it != m_method_key_to_id.end()) {
            m_methods[it->second].nativeFnPtr = fn_ptr;
        } else {
            JNIMethodInfo m;
            m.id = static_cast<uint32_t>(m_methods.size());
            m.className = className;
            m.methodName = mname ? mname : "";
            m.signature = msig ? msig : "";
            m.nativeFnPtr = fn_ptr;
            m_method_key_to_id[key] = m.id;
            m_methods.push_back(m);
        }
    }

    return 0; // JNI_OK
}

uint64_t JNIBridge::create_object_array(const std::vector<uint64_t>& elements) {
    size_t alloc_size = (elements.size() * sizeof(uint64_t) + 63) & ~63;
    uint64_t guest_addr = JNI_GUEST_HEAP_BASE + m_heap_bump;
    m_heap_bump += alloc_size;

    uint64_t* dest = reinterpret_cast<uint64_t*>(guest_addr);
    for (size_t i = 0; i < elements.size(); i++) {
        dest[i] = elements[i];
    }

    m_object_arrays[guest_addr] = elements;
    return guest_addr;
}

uint64_t JNIBridge::create_string_utf(const std::string& str) {
    size_t len = str.length();
    size_t alloc_size = (len + 1 + 63) & ~63; // 64-byte alignment

    uint64_t guest_addr = JNI_GUEST_HEAP_BASE + m_heap_bump;
    m_heap_bump += alloc_size;

    char* dest = reinterpret_cast<char*>(guest_addr);
    std::memcpy(dest, str.c_str(), len + 1);

    m_string_objects[guest_addr] = str;
    return guest_addr;
}

const char* JNIBridge::get_string_utf_chars(uint64_t string_obj) {
    auto it = m_string_objects.find(string_obj);
    if (it != m_string_objects.end()) {
        return it->second.c_str();
    }
    // Might be direct guest string pointer
    if (string_obj >= JNI_GUEST_HEAP_BASE && string_obj < JNI_GUEST_HEAP_BASE + JNI_GUEST_HEAP_SIZE) {
        return reinterpret_cast<const char*>(string_obj);
    }
    return "";
}

// Resolve a jstring handle to a UTF-8 string using CPU memory access
// Returns empty string if resolution fails
std::string JNIBridge::resolve_jstring(uint64_t jstring_handle) {
    // Check cache first
    auto it = m_string_objects.find(jstring_handle);
    if (it != m_string_objects.end()) {
        return it->second;
    }
    
    // Check if it's a direct pointer in our heap
    if (jstring_handle >= JNI_GUEST_HEAP_BASE && jstring_handle < JNI_GUEST_HEAP_BASE + JNI_GUEST_HEAP_SIZE) {
        const char* direct = reinterpret_cast<const char*>(jstring_handle);
        if (direct[0] != '\0') {
            return std::string(direct);
        }
    }
    
    // Try to parse Java String object from guest memory using CPU memory access
    // Java String object layout (Android ART 64-bit, uncompressed pointers):
    // offset 0x00: klass pointer (8 bytes)
    // offset 0x08: monitor (8 bytes)
    // offset 0x10: value (Handle<char[]>) - 8 bytes, reference to char[] array
    // offset 0x18: hash_code (int32, 4 bytes)
    // offset 0x1C: coder (uint8_t, 1 byte) - 0=LATIN1, 1=UTF-16
    // offset 0x1D: hash_is_zero (bool, 1 byte)
    // The char[] array layout:
    //   offset 0x00: klass pointer (8 bytes)
    //   offset 0x08: monitor (8 bytes)
    //   offset 0x10: length (int32, 4 bytes)
    //   offset 0x14: data (char[]) - UTF-16 (2 bytes/char) or LATIN1 (1 byte/char)
    
    if (jstring_handle == 0) {
        return "";
    }
    
    // Get current CPU to access guest memory
    auto* cpu = lcs::cpu::DynarmicCore::current();
    if (!cpu) {
        return "";
    }
    
    // Read value field (Handle<char[]>) at offset 0x10
    uint64_t value_ref = 0;
    if (!lcs::cpu::DynarmicCore::read_guest_memory(jstring_handle + 0x10, &value_ref, sizeof(value_ref))) {
        return "";
    }
    
    // Read coder at offset 0x1C
    uint8_t coder = 0;
    if (!lcs::cpu::DynarmicCore::read_guest_memory(jstring_handle + 0x1C, &coder, sizeof(coder))) {
        return "";
    }
    
    if (value_ref == 0) {
        return "";
    }
    
    // Parse the char[] array at value_ref
    // char[] array layout:
    //   offset 0x00: klass pointer (8 bytes)
    //   offset 0x08: monitor (8 bytes)
    //   offset 0x10: length (int32, 4 bytes)
    //   offset 0x14: data (char[]) - UTF-16 (2 bytes/char) or LATIN1 (1 byte/char)
    
    int32_t char_length = 0;
    if (!lcs::cpu::DynarmicCore::read_guest_memory(value_ref + 0x10, &char_length, sizeof(char_length))) {
        return "";
    }
    
    if (char_length <= 0 || char_length >= 1024) {
        return "";
    }
    
    // coder already read above at line 795
    
    std::string result;
    result.reserve(char_length);
    
    if (coder == 0) {
        // LATIN1 - 1 byte per char
        std::vector<uint8_t> latin1_data(char_length);
        if (!lcs::cpu::DynarmicCore::read_guest_memory(value_ref + 0x14, latin1_data.data(), char_length)) {
            return "";
        }
        for (int i = 0; i < char_length; i++) {
            // Convert LATIN1 to UTF-8 (ASCII subset)
            if (latin1_data[i] <= 0x7F) {
                result.push_back(static_cast<char>(latin1_data[i]));
            } else {
                // Non-ASCII LATIN1 - convert to UTF-8
                result.push_back(static_cast<char>(0xC0 | (latin1_data[i] >> 6)));
                result.push_back(static_cast<char>(0x80 | (latin1_data[i] & 0x3F)));
            }
        }
    } else if (coder == 1) {
        // UTF-16 - 2 bytes per char
        std::vector<uint16_t> utf16_data(char_length);
        if (!lcs::cpu::DynarmicCore::read_guest_memory(value_ref + 0x14, utf16_data.data(), char_length * sizeof(uint16_t))) {
            return "";
        }
        for (int i = 0; i < char_length; i++) {
            uint16_t c = utf16_data[i];
            if (c <= 0x7F) {
                result.push_back(static_cast<char>(c));
            } else if (c <= 0x7FF) {
                result.push_back(static_cast<char>(0xC0 | (c >> 6)));
                result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                result.push_back(static_cast<char>(0xE0 | (c >> 12)));
                result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
    }
    
    return result;
}

JNIBridge::PendingCallbacks JNIBridge::consume_pending_callbacks() {
    PendingCallbacks out = m_pending;
    m_pending = PendingCallbacks{};
    return out;
}

bool JNIBridge::is_video_playing() const {
    return m_video_player && m_video_player->is_playing();
}

void JNIBridge::skip_video() {
    if (!m_video_player || !m_video_player->is_playing()) return;
    m_video_player->close();
    lcs::audio::clear_video_audio();
    std::vector<uint8_t>().swap(m_video_frame_rgba);
    std::vector<int16_t>().swap(m_video_audio_pcm);
    m_video_has_first_frame = false;
    m_video_last_pts_100ns = -1;
    m_video_audio_last_pts_100ns = -1;
    lcs::graphics::RenderBridge::instance().clear_video_frame();
    m_pending.video_finished = true;
}

void JNIBridge::update_video(bool smoke_test) {
    if (!m_video_player || !m_video_player->is_playing()) return;

    if (smoke_test && m_video_frame_count >= 30) {
        std::cout << "[SmokeTest] Skipping intro video after " << m_video_frame_count << " decoded frames.\n";
        skip_video();
        return;
    }

    auto pump_video_audio = [&](int64_t target_pts_100ns) {
        if (!m_video_player->has_audio()) return;
        constexpr int64_t kAhead100ns = 2500000LL; // 250 ms buffer ahead of video PTS
        int packets = 0;
        while (m_video_audio_last_pts_100ns < target_pts_100ns + kAhead100ns && packets < 32) {
            int64_t apts = 0;
            if (!m_video_player->read_audio_samples(m_video_audio_pcm, apts)) break;
            m_video_audio_last_pts_100ns = apts;
            if (!smoke_test && !m_video_audio_pcm.empty()) {
                lcs::audio::queue_video_audio(m_video_audio_pcm.data(), m_video_audio_pcm.size() / 2);
            }
            packets++;
        }
    };

    int64_t pts = 0;
    if (!m_video_has_first_frame) {
        if (!m_video_player->read_frame(m_video_frame_rgba, pts)) {
            skip_video();
            return;
        }
        m_video_has_first_frame = true;
        m_video_last_pts_100ns = pts;
        m_video_start_time = std::chrono::steady_clock::now();
        m_video_frame_count = 1;
        lcs::graphics::RenderBridge::instance().set_video_frame(
            m_video_player->get_width(), m_video_player->get_height(), m_video_frame_rgba.data());
        pump_video_audio(pts);
        return;
    }

    if (smoke_test) {
        if (!m_video_player->read_frame(m_video_frame_rgba, pts)) {
            skip_video();
            return;
        }
        m_video_last_pts_100ns = pts;
        m_video_frame_count++;
        lcs::graphics::RenderBridge::instance().set_video_frame(
            m_video_player->get_width(), m_video_player->get_height(), m_video_frame_rgba.data());
        pump_video_audio(pts);
        return;
    }

    auto elapsed_100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        std::chrono::steady_clock::now() - m_video_start_time).count();
    if (elapsed_100ns > m_video_last_pts_100ns + 2500000LL) {
        m_video_start_time = std::chrono::steady_clock::now() -
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<int64_t, std::ratio<1, 10000000>>(m_video_last_pts_100ns));
        elapsed_100ns = m_video_last_pts_100ns + 333333LL;
    }

    int decoded = 0;
    bool got_new = false;
    while (m_video_last_pts_100ns < elapsed_100ns && decoded < 3) {
        if (!m_video_player->read_frame(m_video_frame_rgba, pts)) {
            skip_video();
            return;
        }
        m_video_last_pts_100ns = pts;
        m_video_frame_count++;
        decoded++;
        got_new = true;
    }
    if (got_new) {
        lcs::graphics::RenderBridge::instance().set_video_frame(
            m_video_player->get_width(), m_video_player->get_height(), m_video_frame_rgba.data());
    }
    pump_video_audio(std::max<int64_t>(0, m_video_last_pts_100ns));
}

} // namespace lcs::os
