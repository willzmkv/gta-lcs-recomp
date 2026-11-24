#include "game/lcs_stochastic.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "graphics/render_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {
namespace {

constexpr uint64_t kGotUploadPngRva = 0x809eb8ULL; // uploadPNGTexture(lglTexture*, bool)
constexpr uint64_t kGotUploadPvrRva = 0x809ec0ULL; // uploadPVRTexture(lglTexture*, bool)

// lglTexture (0x58 bytes, built in lglTextureManager::getTexture): GL name written by the upload
// at +0x04, libc++ std::string with the full file path at +0x40.
constexpr uint64_t kLglGlIdOff = 0x04;
constexpr uint64_t kLglPathOff = 0x40;

// Repeating ground/wall textures of the LCS world (file names as the game loads them, lower
// case). A trailing '*' matches a prefix. Never add textures with alpha cut-outs, lettering, road
// markings, edges, corners or transitions (grass_path, grass_tyre, tar_1line...): the random
// offsets would scatter them.
const char* const kDefaultWhitelist[] = {
    // SkyGfx Extended texdb.txt (stochastic=1) entries that exist in LCS:
    "alumox64", "cj_wood1", "concrete_64hv", "concrete_rough_256", "concretebig3_256",
    "concretebig4256", "concretebigblu4256128", "concretebuild64", "concretemanky",
    "concretenew256", "concretenewb256", "dirt64b2", "dirtgrn128", "dirtkb_64hv", "dustyconcrete",
    "dustytar_64hv", "grass_128hv", "grass_dry_64hv", "gravelground128", "gravelkb_128",
    "greyground256", "metalox64", "mottled_creme_64hv", "mottled_grey_64hv", "newall9",
    "newall_harling_sless", "newgrnd1_128", "redmetal", "rocktq128", "rustb256128", "rustd64",
    "sandytar_64hv", "slab256", "tenwhitebrick64",
    // ...and their size/LOD variants:
    "concretebig3_256128", "concretebig4256128", "concretemanky_lod", "concretenew256128",
    "concretenewb256128", "concroadslab_256128", "dirty256128", "dustyconcrete128", "grass_32",
    "grass_64hv", "grass_64hv_lod", "greyground256128", "newall9128", "newall_harling_sless128",
    "redmetal02",
    // Uniform noise-like ground, rock and asphalt textures picked by name:
    "alod_cliffnew_128_lod", "alod_cliffwall_64hv", "cliffnew_128", "cliffnew_128_lod",
    "cliffwall-64hv", "cliffwall_64hv", "cliffwall_64hvlod", "concrete02_64hv", "concretebig3_64",
    "concretebigb256", "concretebigb256128", "concretebigb64", "concretebigblu4256",
    "concretebigc256128", "concretenewb32", "deadgrass_64hv", "dirt64", "dirt64b", "dirt64b_lod",
    "dirt_64hv", "dirtmix_128", "grass_deadmix_64hv", "metalox32", "mud", "mud_64hv",
    "mud_wet_128hv", "rockwall03_64hv", "sand", "sandfloor", "sandfloorgrey", "sandlod",
    "sandy_64hv", "sandymix_64hv", "tar_mix128hv", "tar_mix256hv", "tarmac_128h", "tarmac_64hv",
    "tarmacplain_128h",
};

std::vector<std::string> g_exact;    // sorted, for binary search
std::vector<std::string> g_prefixes;
bool g_diag_names = false;
std::mutex g_seen_mtx;
std::unordered_set<std::string> g_seen;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void add_entry(const std::string& raw) {
    std::string e = lower(raw);
    bool remove = false;
    if (!e.empty() && e[0] == '-') { remove = true; e.erase(0, 1); }
    if (e.empty()) return;
    const bool prefix = e.back() == '*';
    if (prefix) e.pop_back();
    auto& list = prefix ? g_prefixes : g_exact;
    list.erase(std::remove(list.begin(), list.end(), e), list.end());
    if (!remove) list.push_back(e);
}

// Built-in list plus stochastic_textures.txt next to lcs.exe (one name per line, '-name' removes
// a built-in entry, '#' starts a comment).
void load_whitelist() {
    for (const char* n : kDefaultWhitelist) add_entry(n);
    std::ifstream f("stochastic_textures.txt");
    std::string line;
    int user = 0;
    while (std::getline(f, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        const auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        const auto e = line.find_last_not_of(" \t\r");
        add_entry(line.substr(b, e - b + 1));
        ++user;
    }
    std::sort(g_exact.begin(), g_exact.end());
    std::cout << "[Stochastic] Whitelist: " << g_exact.size() << " names, " << g_prefixes.size()
              << " prefixes" << (user ? " (stochastic_textures.txt: " + std::to_string(user) + " lines)" : "")
              << "\n";
}

bool whitelisted(const std::string& name) {
    if (std::binary_search(g_exact.begin(), g_exact.end(), name)) return true;
    for (const auto& p : g_prefixes) {
        if (name.compare(0, p.size(), p) == 0) return true;
    }
    return false;
}

std::string_view read_libcxx_string(uint64_t va) {
    const auto* p = reinterpret_cast<const uint8_t*>(va);
    if ((p[0] & 1u) == 0) return {reinterpret_cast<const char*>(p + 1), static_cast<size_t>(p[0] >> 1)};
    const uint64_t size = *reinterpret_cast<const uint64_t*>(va + 8);
    const uint64_t data = *reinterpret_cast<const uint64_t*>(va + 16);
    if (!data || size > 512) return {};
    return {reinterpret_cast<const char*>(data), static_cast<size_t>(size)};
}

// Runs right after uploadPVRTexture/uploadPNGTexture returns. The recycler can hand an old GL
// name to a new texture, so every upload sets the flag either way.
void on_upload_post(uint64_t lgl) {
    if (!lgl) return;
    const uint32_t gl_id = *reinterpret_cast<const uint32_t*>(lgl + kLglGlIdOff);
    if (!gl_id) return;
    const std::string_view path = read_libcxx_string(lgl + kLglPathOff);
    size_t b = path.find_last_of("/\\");
    b = (b == std::string_view::npos) ? 0 : b + 1;
    std::string_view base = path.substr(b);
    if (const auto dot = base.find_last_of('.'); dot != std::string_view::npos) base = base.substr(0, dot);
    const std::string name = lower(base);
    const bool on = !name.empty() && whitelisted(name);
    lcs::graphics::RenderBridge::instance().set_texture_stochastic(gl_id, on);
    if (g_diag_names) {
        std::lock_guard<std::mutex> lock(g_seen_mtx);
        if (g_seen.insert(std::string(path)).second) {
            std::cout << "[TexName] " << path << (on ? " stochastic" : "") << "\n";
        }
    }
}

void write_got(lcs::cpu::DynarmicCore& cpu, uint64_t slot_va, uint64_t new_target) {
#if defined(LCS_PLATFORM_WINDOWS)
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(slot_va), 8, PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(reinterpret_cast<void*>(slot_va), &new_target, 8);
    VirtualProtect(reinterpret_cast<void*>(slot_va), 8, old_prot, &old_prot);
#else
    std::memcpy(reinterpret_cast<void*>(slot_va), &new_target, 8);
#endif
    cpu.invalidate_cache(slot_va, 8);
}

} // namespace

void install_stochastic_texture_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    g_diag_names = std::getenv("LCS_DIAG_TEXNAMES") != nullptr;
    load_whitelist();

#if defined(LCS_PLATFORM_WINDOWS)
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    auto* page = static_cast<uint8_t*>(std::aligned_alloc(4096, 0x1000));
#endif
    if (!page) {
        std::cerr << "[Stochastic] Stub page allocation failed; stochastic texturing disabled.\n";
        return;
    }
    std::memset(page, 0, 0x1000);

    const uint64_t post_va = lcs::cpu::HLEManager::instance().register_function(
        "__lcs_sto_upload_post", [](const uint64_t* a) -> uint64_t { on_upload_post(a[0]); return 0; });

    // orig(x0, x1); post(saved x0); return orig's x0.
    static constexpr uint32_t kStub[14] = {
        0xa9be7bfdu, // stp x29, x30, [sp, #-0x20]!
        0xf9000bf3u, // str x19, [sp, #0x10]
        0xaa0003f3u, // mov x19, x0
        0x58000170u, // ldr x16, #0x38 (orig)
        0xd63f0200u, // blr x16
        0xf9000fe0u, // str x0, [sp, #0x18]
        0xaa1303e0u, // mov x0, x19
        0x58000130u, // ldr x16, #0x40 (post)
        0xd63f0200u, // blr x16
        0xf9400fe0u, // ldr x0, [sp, #0x18]
        0xf9400bf3u, // ldr x19, [sp, #0x10]
        0xa8c27bfdu, // ldp x29, x30, [sp], #0x20
        0xd65f03c0u, // ret
        0xd503201fu, // nop
    };
    struct Hook { uint64_t got_rva; const char* sym; };
    const Hook hooks[] = {
        {kGotUploadPvrRva, "_Z16uploadPVRTextureP10lglTextureb"},
        {kGotUploadPngRva, "_Z16uploadPNGTextureP10lglTextureb"},
    };
    const uint64_t bias = image_game.get_load_bias();
    const uint64_t page_va = reinterpret_cast<uint64_t>(page);
    int installed = 0;
    for (size_t i = 0; i < std::size(hooks); ++i) {
        const uint64_t expected = image_game.find_symbol(hooks[i].sym);
        const uint64_t slot_va = bias + hooks[i].got_rva;
        const uint64_t cur = *reinterpret_cast<const uint64_t*>(slot_va);
        if (!expected || (cur && cur != expected)) {
            std::cerr << "[Stochastic] GOT slot for " << hooks[i].sym << " does not match; skipped.\n";
            continue;
        }
        uint8_t* stub = page + i * 0x50;
        const uint64_t stub_va = page_va + i * 0x50;
        const uint64_t lits[2] = {cur ? cur : expected, post_va};
        std::memcpy(stub, kStub, sizeof(kStub));
        std::memcpy(stub + 0x38, lits, sizeof(lits));
        cpu.invalidate_cache(stub_va, 0x50);
        write_got(cpu, slot_va, stub_va);
        ++installed;
    }
    std::cout << "[Stochastic] Texture upload hooks installed: " << installed << "/2\n";
}

} // namespace lcs::game
