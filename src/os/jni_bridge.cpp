#include "jni_bridge.h"
#include "cpu/dynarmic_core.h"
#include "cpu/callbacks.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>

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

        std::cout << "[JNI] JavaVM::GetEnv(version=0x" << std::hex << version << ") -> JNI_OK\n" << std::dec;
        return 0; // JNI_OK
    });

    // AttachCurrentThread (offset 0x20 / index 4)
    uint64_t attach_thread_trampoline = hle.register_function("JavaVM::AttachCurrentThread", [](const uint64_t* args) -> uint64_t {
        uint64_t env_out_ptr = args[1];
        if (env_out_ptr) {
            *reinterpret_cast<uint64_t*>(env_out_ptr) = JNI_GUEST_ENV_ADDR;
        }
        std::cout << "[JNI] JavaVM::AttachCurrentThread() -> JNI_OK\n";
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
    (void)is_va;

    uint32_t method_id = static_cast<uint32_t>((method_handle - JNI_GUEST_METHOD_BASE) / 0x40);
    if (method_id >= m_methods.size()) {
        std::cerr << "[JNI] CallMethod on invalid method handle: 0x" << std::hex << method_handle << std::dec << "\n";
        return 0;
    }

    const auto& m = m_methods[method_id];
    std::cout << "[JNI] Call" << (is_static ? "Static" : "") << "Method: "
              << m.className << "." << m.methodName << m.signature << "\n";

    if (m.methodName == "GetDeviceLanguage") {
        return create_string_utf("en");
    }
    else if (m.methodName == "GetVersionName") {
        return create_string_utf("2.4.379");
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
    else if (m.methodName == "ShowGate") {
        m_pending.start_game = true;
        return 0;
    }
    else if (m.methodName == "QuitApp") {
        m_pending.quit_requested = true;
        return 0;
    }
    else if (m.methodName == "readUserFile") {
        uint64_t str_handle = args[3];
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
        uint64_t path_handle = args[3];
        uint64_t data_handle = args[4];
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

JNIBridge::PendingCallbacks JNIBridge::consume_pending_callbacks() {
    PendingCallbacks out = m_pending;
    m_pending = PendingCallbacks{};
    return out;
}

} // namespace lcs::os
