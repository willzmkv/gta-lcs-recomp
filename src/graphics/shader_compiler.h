#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lcs::graphics {

struct ShaderVariableInfo {
    std::string name;
    uint32_t gl_type = 0x8B52; // GL_FLOAT_VEC4 default
    int32_t array_size = 1;
};

struct ShaderReflection {
    std::vector<ShaderVariableInfo> attributes;
    std::vector<ShaderVariableInfo> uniforms;
    std::vector<ShaderVariableInfo> varyings;
};

struct UniformMemberLayout {
    std::string name;
    uint32_t gl_type = 0;
    int32_t array_size = 1;
    uint32_t byte_offset = 0;
    uint32_t byte_size = 16;
    bool is_sampler = false;
    int32_t sampler_slot = -1;
};

struct AttribLayout {
    std::string name;
    uint32_t gl_type = 0;
    int32_t location = 0;
};

struct TranslatedProgramHLSL {
    std::string vs_hlsl;
    std::string ps_hlsl;
    std::vector<AttribLayout> attribs;
    std::vector<UniformMemberLayout> uniforms;
    uint32_t cbuffer_size = 0; // padded to 256 bytes
};

// Strips comments, evaluates #define/#ifdef/#ifndef/#if/#else/#endif, and expands macros.
std::string preprocess_glsl(const std::string& source);

// Extracts active attribute, varying, and uniform declarations from preprocessed GLSL ES source.
ShaderReflection reflect_glsl(const std::string& preprocessed_source);

// Translates preprocessed GLSL ES 2.0 vertex + fragment shaders into HLSL SM 5.0.
TranslatedProgramHLSL translate_program_to_hlsl(
    const std::string& vs_preprocessed,
    const std::string& fs_preprocessed,
    const std::unordered_map<std::string, int32_t>& bound_attrib_locations);

// Legacy single-shader entry point.
bool compile_shader_source(const char* glsl_src, int stage);

} // namespace lcs::graphics
