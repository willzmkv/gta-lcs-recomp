#include "shader_compiler.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace lcs::graphics {

namespace {

std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool in_line = false;
    bool in_block = false;
    for (size_t i = 0; i < src.size(); i++) {
        char c = src[i];
        char next = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (in_line) {
            if (c == '\n') {
                in_line = false;
                out.push_back('\n');
            }
            continue;
        }
        if (in_block) {
            if (c == '*' && next == '/') {
                in_block = false;
                i++;
            } else if (c == '\n') {
                out.push_back('\n');
            }
            continue;
        }
        if (c == '/' && next == '/') {
            in_line = true;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block = true;
            i++;
            continue;
        }
        if (c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool eval_pp_expr(const std::string& expr, const std::unordered_map<std::string, std::string>& macros) {
    std::string s = trim(expr);
    if (s.empty()) return false;

    int depth = 0;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (depth == 0 && s[i] == '|' && s[i + 1] == '|') {
            return eval_pp_expr(s.substr(0, i), macros) || eval_pp_expr(s.substr(i + 2), macros);
        }
    }
    depth = 0;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (depth == 0 && s[i] == '&' && s[i + 1] == '&') {
            return eval_pp_expr(s.substr(0, i), macros) && eval_pp_expr(s.substr(i + 2), macros);
        }
    }

    if (s.front() == '!') {
        return !eval_pp_expr(s.substr(1), macros);
    }
    if (s.front() == '(' && s.back() == ')') {
        int d = 0;
        bool outer = true;
        for (size_t i = 0; i + 1 < s.size(); i++) {
            if (s[i] == '(') d++;
            else if (s[i] == ')') d--;
            if (d == 0) {
                outer = false;
                break;
            }
        }
        if (outer) return eval_pp_expr(s.substr(1, s.size() - 2), macros);
    }

    if (s.rfind("defined", 0) == 0) {
        std::string rest = trim(s.substr(7));
        if (!rest.empty() && rest.front() == '(' && rest.back() == ')') {
            rest = trim(rest.substr(1, rest.size() - 2));
        }
        return macros.find(rest) != macros.end();
    }

    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
        return std::strtol(s.c_str(), nullptr, 0) != 0;
    }

    auto it = macros.find(s);
    if (it != macros.end()) {
        std::string val = trim(it->second);
        if (val.empty()) return true;
        if (std::isdigit(static_cast<unsigned char>(val[0]))) {
            return std::strtol(val.c_str(), nullptr, 0) != 0;
        }
        return true;
    }
    return false;
}

std::string expand_macros(const std::string& line, const std::unordered_map<std::string, std::string>& macros) {
    std::string out;
    out.reserve(line.size());
    size_t i = 0;
    while (i < line.size()) {
        if (is_ident_start(line[i])) {
            size_t start = i;
            while (i < line.size() && is_ident_char(line[i])) i++;
            std::string ident = line.substr(start, i - start);
            auto it = macros.find(ident);
            if (it != macros.end() && !it->second.empty()) {
                out.append(it->second);
            } else {
                out.append(ident);
            }
        } else {
            out.push_back(line[i++]);
        }
    }
    return out;
}

int32_t eval_int_expr(const std::string& expr) {
    std::string s = trim(expr);
    if (s.empty()) return 1;
    size_t mul = s.find('*');
    if (mul != std::string::npos) {
        return eval_int_expr(s.substr(0, mul)) * eval_int_expr(s.substr(mul + 1));
    }
    size_t add = s.find('+');
    if (add != std::string::npos) {
        return eval_int_expr(s.substr(0, add)) + eval_int_expr(s.substr(add + 1));
    }
    int32_t v = static_cast<int32_t>(std::strtol(s.c_str(), nullptr, 0));
    return v > 0 ? v : 1;
}

uint32_t gl_type_from_name(const std::string& type_name) {
    if (type_name == "float") return 0x1406;      // GL_FLOAT
    if (type_name == "vec2") return 0x8B50;       // GL_FLOAT_VEC2
    if (type_name == "vec3") return 0x8B51;       // GL_FLOAT_VEC3
    if (type_name == "vec4") return 0x8B52;       // GL_FLOAT_VEC4
    if (type_name == "int") return 0x1404;        // GL_INT
    if (type_name == "ivec2") return 0x8B53;      // GL_INT_VEC2
    if (type_name == "ivec3") return 0x8B54;      // GL_INT_VEC3
    if (type_name == "ivec4") return 0x8B55;      // GL_INT_VEC4
    if (type_name == "bool") return 0x8B56;       // GL_BOOL
    if (type_name == "mat2") return 0x8B5A;       // GL_FLOAT_MAT2
    if (type_name == "mat3") return 0x8B5B;       // GL_FLOAT_MAT3
    if (type_name == "mat4") return 0x8B5C;       // GL_FLOAT_MAT4
    if (type_name == "sampler2D") return 0x8B5E;  // GL_SAMPLER_2D
    if (type_name == "samplerCube") return 0x8B60;// GL_SAMPLER_CUBE
    return 0x8B52;
}

std::string hlsl_type_from_gl_type(uint32_t gl_type) {
    switch (gl_type) {
        case 0x1406: return "float";
        case 0x8B50: return "float2";
        case 0x8B51: return "float3";
        case 0x8B52: return "float4";
        case 0x1404: return "int";
        case 0x8B53: return "int2";
        case 0x8B54: return "int3";
        case 0x8B55: return "int4";
        case 0x8B56: return "bool";
        case 0x8B5A: return "row_major float2x2";
        case 0x8B5B: return "row_major float3x3";
        case 0x8B5C: return "row_major float4x4";
        case 0x8B5E: return "Texture2D";
        case 0x8B60: return "TextureCube";
        default:     return "float4";
    }
}

uint32_t register_count_for_gl_type(uint32_t gl_type) {
    switch (gl_type) {
        case 0x8B5A: return 2; // mat2
        case 0x8B5B: return 3; // mat3
        case 0x8B5C: return 4; // mat4
        default:     return 1;
    }
}

bool is_precision_qualifier(const std::string& tok) {
    return tok == "highp" || tok == "mediump" || tok == "lowp";
}

// Removes top-level attribute / uniform / varying declarations and returns the shader functions/body.
std::string strip_top_level_decls(const std::string& preprocessed) {
    std::istringstream iss(preprocessed);
    std::string line;
    std::string out;
    out.reserve(preprocessed.size());

    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (!t.empty() && t.find(';') != std::string::npos) {
            std::istringstream dss(t);
            std::string first_tok;
            dss >> first_tok;
            if (first_tok == "attribute" || first_tok == "in" ||
                first_tok == "uniform" || first_tok == "varying") {
                continue;
            }
        }
        out.append(line);
        out.push_back('\n');
    }
    return out;
}

enum class ExprKind {
    Unknown,
    Scalar,
    Vec,
    Mat
};

struct Tok {
    std::string text;
    bool is_ws = false;
    bool is_ident = false;
    bool is_num = false;
};

std::vector<Tok> tokenize_glsl(const std::string& src) {
    std::vector<Tok> toks;
    size_t i = 0;
    while (i < src.size()) {
        if (std::isspace(static_cast<unsigned char>(src[i]))) {
            size_t s = i;
            while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) i++;
            toks.push_back({src.substr(s, i - s), true, false, false});
        } else if (is_ident_start(src[i])) {
            size_t s = i;
            while (i < src.size() && is_ident_char(src[i])) i++;
            toks.push_back({src.substr(s, i - s), false, true, false});
        } else if (std::isdigit(static_cast<unsigned char>(src[i])) ||
                   (src[i] == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t s = i;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '.')) i++;
            toks.push_back({src.substr(s, i - s), false, false, true});
        } else {
            // Two-char operators
            if (i + 1 < src.size()) {
                char a = src[i], b = src[i + 1];
                if ((a == '=' && b == '=') || (a == '!' && b == '=') ||
                    (a == '<' && b == '=') || (a == '>' && b == '=') ||
                    (a == '&' && b == '&') || (a == '|' && b == '|') ||
                    (a == '+' && b == '=') || (a == '-' && b == '=') ||
                    (a == '*' && b == '=') || (a == '/' && b == '=') ||
                    (a == '+' && b == '+') || (a == '-' && b == '-')) {
                    toks.push_back({src.substr(i, 2), false, false, false});
                    i += 2;
                    continue;
                }
            }
            toks.push_back({src.substr(i, 1), false, false, false});
            i++;
        }
    }
    return toks;
}

int next_non_ws(const std::vector<Tok>& toks, int idx) {
    while (idx < static_cast<int>(toks.size()) && toks[idx].is_ws) idx++;
    return idx < static_cast<int>(toks.size()) ? idx : -1;
}

int prev_non_ws(const std::vector<Tok>& toks, int idx) {
    while (idx >= 0 && toks[idx].is_ws) idx--;
    return idx;
}

int find_matching_close(const std::vector<Tok>& toks, int open_idx, const std::string& open_ch, const std::string& close_ch) {
    int depth = 0;
    for (int i = open_idx; i < static_cast<int>(toks.size()); i++) {
        if (toks[i].text == open_ch) depth++;
        else if (toks[i].text == close_ch) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

int find_matching_open(const std::vector<Tok>& toks, int close_idx, const std::string& open_ch, const std::string& close_ch) {
    int depth = 0;
    for (int i = close_idx; i >= 0; i--) {
        if (toks[i].text == close_ch) depth++;
        else if (toks[i].text == open_ch) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

// Rewrites GLSL body for HLSL compatibility:
//  - void main() -> void gl_main()
//  - removes highp/mediump/lowp qualifiers
//  - rewrites single-argument vecN(s) -> ((floatN)(s)) and ivecN(s) -> ((intN)(s))
//  - rewrites 2-argument atan(y, x) -> atan2(y, x)
//  - rewrites matrix*vec, vec*matrix, matrix*matrix into HLSL mul(...)
std::string rewrite_glsl_body_to_hlsl(
    const std::string& body,
    const std::unordered_map<std::string, ExprKind>& initial_var_types) {

    std::unordered_map<std::string, ExprKind> var_types = initial_var_types;
    std::vector<Tok> toks = tokenize_glsl(body);

    // Pass 1: collect local variable types and strip precision qualifiers, rename main -> gl_main
    std::vector<Tok> pass1;
    pass1.reserve(toks.size());
    for (int i = 0; i < static_cast<int>(toks.size()); i++) {
        if (toks[i].is_ident && is_precision_qualifier(toks[i].text)) {
            if (i + 1 < static_cast<int>(toks.size()) && toks[i + 1].is_ws) i++;
            continue;
        }
        if (toks[i].is_ident && toks[i].text == "main") {
            int nxt = next_non_ws(toks, i + 1);
            int prv = prev_non_ws(toks, i - 1);
            if (nxt >= 0 && toks[nxt].text == "(" && prv >= 0 && toks[prv].text == "void") {
                Tok t = toks[i];
                t.text = "gl_main";
                pass1.push_back(std::move(t));
                continue;
            }
        }
        pass1.push_back(toks[i]);
    }

    // Collect local variable declarations in pass1
    for (int i = 0; i < static_cast<int>(pass1.size()); i++) {
        if (!pass1[i].is_ident) continue;
        ExprKind k = ExprKind::Unknown;
        const std::string& ty = pass1[i].text;
        if (ty == "mat2" || ty == "mat3" || ty == "mat4") k = ExprKind::Mat;
        else if (ty == "vec2" || ty == "vec3" || ty == "vec4" ||
                 ty == "ivec2" || ty == "ivec3" || ty == "ivec4" ||
                 ty == "bvec2" || ty == "bvec3" || ty == "bvec4") k = ExprKind::Vec;
        else if (ty == "float" || ty == "int" || ty == "bool") k = ExprKind::Scalar;
        if (k != ExprKind::Unknown) {
            int nxt = next_non_ws(pass1, i + 1);
            if (nxt >= 0 && pass1[nxt].is_ident) {
                var_types[pass1[nxt].text] = k;
            }
        }
    }

    // Pass 2: rewrite single-argument vecN(x) -> ((vecN)(x)) and 2-arg atan(y, x) -> atan2(y, x)
    auto rewrite_calls = [&](std::vector<Tok>& in_toks) {
        std::vector<Tok> out;
        out.reserve(in_toks.size());
        for (int i = 0; i < static_cast<int>(in_toks.size()); i++) {
            if (in_toks[i].is_ident &&
                (in_toks[i].text == "vec2" || in_toks[i].text == "vec3" || in_toks[i].text == "vec4" ||
                 in_toks[i].text == "ivec2" || in_toks[i].text == "ivec3" || in_toks[i].text == "ivec4" ||
                 in_toks[i].text == "atan")) {
                int open_p = next_non_ws(in_toks, i + 1);
                if (open_p >= 0 && in_toks[open_p].text == "(") {
                    int close_p = find_matching_close(in_toks, open_p, "(", ")");
                    if (close_p > open_p) {
                        int depth = 0;
                        int commas = 0;
                        for (int j = open_p + 1; j < close_p; j++) {
                            if (in_toks[j].text == "(" || in_toks[j].text == "[") depth++;
                            else if (in_toks[j].text == ")" || in_toks[j].text == "]") depth--;
                            else if (depth == 0 && in_toks[j].text == ",") commas++;
                        }
                        if (in_toks[i].text == "atan") {
                            Tok t = in_toks[i];
                            if (commas == 1) t.text = "atan2";
                            out.push_back(std::move(t));
                            continue;
                        }
                        if (commas == 0) {
                            // Single-argument vector constructor -> HLSL C-style cast ((vecN)(arg))
                            out.push_back({"(", false, false, false});
                            out.push_back({"(", false, false, false});
                            out.push_back(in_toks[i]);
                            out.push_back({")", false, false, false});
                            out.push_back({"(", false, false, false});
                            // Recursively check inner tokens? Simpler: emit inner and let next loop pass handle outer,
                            // or just push "(" and close ")" via tracking. Actually let's push the inner tokens directly!
                            std::string inner;
                            for (int j = open_p + 1; j < close_p; j++) inner.append(in_toks[j].text);
                            out.push_back({inner, false, false, false});
                            out.push_back({")", false, false, false});
                            out.push_back({")", false, false, false});
                            i = close_p;
                            continue;
                        }
                    }
                }
            }
            out.push_back(in_toks[i]);
        }
        return out;
    };

    // Run twice to handle nested single-arg constructors cleanly.
    pass1 = rewrite_calls(pass1);
    pass1 = rewrite_calls(pass1);

    // Re-tokenize after string insertions so pass 3 sees atomic tokens cleanly.
    std::string rebuilt;
    for (const auto& t : pass1) rebuilt.append(t.text);
    pass1 = tokenize_glsl(rebuilt);

    // Helper to classify a token slice [b..e]
    auto classify_span = [&](const std::vector<Tok>& tks, int b, int e) -> ExprKind {
        int fb = next_non_ws(tks, b);
        int fe = prev_non_ws(tks, e);
        if (fb < 0 || fe < fb) return ExprKind::Unknown;

        // Strip outer parens if the whole span is parenthesized
        while (fb < fe && tks[fb].text == "(" && find_matching_close(tks, fb, "(", ")") == fe) {
            fb = next_non_ws(tks, fb + 1);
            fe = prev_non_ws(tks, fe - 1);
            if (fb < 0 || fe < fb) return ExprKind::Unknown;
        }

        // Single identifier
        if (fb == fe && tks[fb].is_ident) {
            auto it = var_types.find(tks[fb].text);
            return it != var_types.end() ? it->second : ExprKind::Unknown;
        }
        // Single number
        if (fb == fe && tks[fb].is_num) return ExprKind::Scalar;

        // Indexed mat[i] -> Vec!
        if (tks[fe].text == "]") {
            int open_b = find_matching_open(tks, fe, "[", "]");
            int base = prev_non_ws(tks, open_b - 1);
            if (base >= fb && tks[base].is_ident) {
                auto it = var_types.find(tks[base].text);
                if (it != var_types.end() && it->second == ExprKind::Mat) {
                    return ExprKind::Vec;
                }
            }
        }

        // Constructor vec4(...) or float4(...)
        if (tks[fb].is_ident &&
            (tks[fb].text == "vec2" || tks[fb].text == "vec3" || tks[fb].text == "vec4" ||
             tks[fb].text == "float2" || tks[fb].text == "float3" || tks[fb].text == "float4")) {
            return ExprKind::Vec;
        }

        // Contains matrix variable without [] indexing (simple heuristic inside binary * operands)
        for (int i = fb; i <= fe; i++) {
            if (tks[i].is_ident) {
                auto it = var_types.find(tks[i].text);
                if (it != var_types.end() && it->second == ExprKind::Mat) {
                    int nxt = next_non_ws(tks, i + 1);
                    if (nxt < 0 || nxt > fe || tks[nxt].text != "[") {
                        return ExprKind::Mat;
                    }
                }
            }
        }
        return ExprKind::Vec;
    };

    // Pass 3: rewrite matrix multiplications `L * R` -> `mul(R, L)` iteratively until none remain.
    for (int iter = 0; iter < 16; iter++) {
        bool changed = false;
        for (int i = 0; i < static_cast<int>(pass1.size()); i++) {
            if (pass1[i].text != "*") continue;

            // Find left operand span backwards
            int l_end = prev_non_ws(pass1, i - 1);
            if (l_end < 0) continue;
            int cur = l_end;
            while (cur >= 0) {
                if (pass1[cur].text == ")") {
                    cur = find_matching_open(pass1, cur, "(", ")");
                    int bef = prev_non_ws(pass1, cur - 1);
                    if (bef >= 0 && pass1[bef].is_ident) {
                        cur = bef;
                    }
                } else if (pass1[cur].text == "]") {
                    cur = find_matching_open(pass1, cur, "[", "]");
                    int bef = prev_non_ws(pass1, cur - 1);
                    if (bef >= 0) { cur = bef; continue; }
                }
                int bef = prev_non_ws(pass1, cur - 1);
                if (bef >= 0 && pass1[bef].text == ".") {
                    cur = prev_non_ws(pass1, bef - 1);
                    continue;
                }
                if (bef >= 0 && (pass1[bef].text == "-" || pass1[bef].text == "+")) {
                    int bb = prev_non_ws(pass1, bef - 1);
                    if (bb < 0 || (!pass1[bb].is_ident && !pass1[bb].is_num && pass1[bb].text != ")" && pass1[bb].text != "]")) {
                        cur = bef;
                    }
                }
                break;
            }
            int l_start = cur;

            // Find right operand span forwards
            int r_start = next_non_ws(pass1, i + 1);
            if (r_start < 0) continue;
            cur = r_start;
            if (pass1[cur].text == "-" || pass1[cur].text == "+") {
                cur = next_non_ws(pass1, cur + 1);
                if (cur < 0) continue;
            }
            while (cur < static_cast<int>(pass1.size())) {
                if (pass1[cur].text == "(") {
                    cur = find_matching_close(pass1, cur, "(", ")");
                    if (cur < 0) break;
                } else if (pass1[cur].is_ident) {
                    int nxt = next_non_ws(pass1, cur + 1);
                    if (nxt >= 0 && pass1[nxt].text == "(") {
                        cur = find_matching_close(pass1, nxt, "(", ")");
                        if (cur < 0) break;
                    }
                }
                int nxt = next_non_ws(pass1, cur + 1);
                while (nxt >= 0 && (pass1[nxt].text == "." || pass1[nxt].text == "[")) {
                    if (pass1[nxt].text == ".") {
                        cur = next_non_ws(pass1, nxt + 1);
                    } else {
                        cur = find_matching_close(pass1, nxt, "[", "]");
                    }
                    if (cur < 0) break;
                    nxt = next_non_ws(pass1, cur + 1);
                }
                break;
            }
            int r_end = cur;
            if (r_end < r_start || l_start < 0) continue;

            ExprKind lk = classify_span(pass1, l_start, l_end);
            ExprKind rk = classify_span(pass1, r_start, r_end);
            if ((lk == ExprKind::Mat && rk != ExprKind::Scalar) ||
                (rk == ExprKind::Mat && lk != ExprKind::Scalar)) {
                std::string l_str, r_str;
                for (int k = l_start; k <= l_end; k++) l_str.append(pass1[k].text);
                for (int k = r_start; k <= r_end; k++) r_str.append(pass1[k].text);
                // HLSL with row_major matrices: GLSL (column-major) mat*vec = row_major mul(vec, mat)
                // GLSL (column-major) vec*mat = row_major mul(mat, vec)
                std::string rep;
                if (lk == ExprKind::Mat && rk != ExprKind::Mat) {
                    // GLSL mat*vec (col-major) = HLSL row_major mul(vec, mat)
                    rep = "mul(" + trim(r_str) + ", " + trim(l_str) + ")";
                } else if (rk == ExprKind::Mat && lk != ExprKind::Mat) {
                    // GLSL vec*mat (col-major) = HLSL row_major mul(mat, vec)
                    rep = "mul(" + trim(r_str) + ", " + trim(l_str) + ")";
                } else {
                    // Both matrices: mul(R, L) for right-to-left composition
                    rep = "mul(" + trim(r_str) + ", " + trim(l_str) + ")";
                }
                std::vector<Tok> rep_toks = tokenize_glsl(rep);
                pass1.erase(pass1.begin() + l_start, pass1.begin() + r_end + 1);
                pass1.insert(pass1.begin() + l_start, rep_toks.begin(), rep_toks.end());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }

    // Note: Uniform/local mat4/mat3 use `#define mat4 row_major float4x4` and `glUniformMatrix*`
    // copies GLSL column c directly into HLSL row c (`m[c]`), so `m[idx]` already indexes column c
    // cleanly for both rvalues (`vc_scn_mat4_view[3]`) and lvalues (`BoneToLocal[0] += ...`).
    std::string final_body;

    for (const auto& t : pass1) final_body.append(t.text);
    return final_body;
}

} // namespace

std::string preprocess_glsl(const std::string& source) {
    std::string clean = strip_comments(source);
    std::unordered_map<std::string, std::string> macros;

    struct CondFrame {
        bool parent_active = true;
        bool this_active = true;
        bool any_taken = true;
    };
    std::vector<CondFrame> stack;

    auto is_active = [&]() -> bool {
        return stack.empty() || stack.back().this_active;
    };

    std::istringstream iss(clean);
    std::string line;
    std::string out;
    out.reserve(clean.size());

    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (!t.empty() && t.front() == '#') {
            std::string body = trim(t.substr(1));
            size_t sp = 0;
            while (sp < body.size() && !std::isspace(static_cast<unsigned char>(body[sp]))) sp++;
            std::string dir = body.substr(0, sp);
            std::string rest = trim(body.substr(sp));

            if (dir == "ifdef") {
                bool cond = macros.find(rest) != macros.end();
                bool p = is_active();
                stack.push_back({p, p && cond, cond});
            } else if (dir == "ifndef") {
                bool cond = macros.find(rest) == macros.end();
                bool p = is_active();
                stack.push_back({p, p && cond, cond});
            } else if (dir == "if") {
                bool cond = eval_pp_expr(rest, macros);
                bool p = is_active();
                stack.push_back({p, p && cond, cond});
            } else if (dir == "elif") {
                if (!stack.empty()) {
                    auto& top = stack.back();
                    bool cond = !top.any_taken && eval_pp_expr(rest, macros);
                    top.this_active = top.parent_active && cond;
                    if (cond) top.any_taken = true;
                }
            } else if (dir == "else") {
                if (!stack.empty()) {
                    auto& top = stack.back();
                    top.this_active = top.parent_active && !top.any_taken;
                    top.any_taken = true;
                }
            } else if (dir == "endif") {
                if (!stack.empty()) stack.pop_back();
            } else if (dir == "define" && is_active()) {
                size_t id_end = 0;
                while (id_end < rest.size() && is_ident_char(rest[id_end])) id_end++;
                std::string name = rest.substr(0, id_end);
                std::string val = trim(rest.substr(id_end));
                if (id_end < rest.size() && rest[id_end] == '(') {
                    macros[name] = "";
                } else {
                    macros[name] = val;
                }
            } else if (dir == "undef" && is_active()) {
                macros.erase(rest);
            }
            continue;
        }

        if (!is_active()) continue;
        if (t.rfind("precision ", 0) == 0) continue;

        out.append(expand_macros(line, macros));
        out.push_back('\n');
    }

    return out;
}

ShaderReflection reflect_glsl(const std::string& preprocessed_source) {
    ShaderReflection refl;
    std::istringstream iss(preprocessed_source);
    std::string line;

    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (t.empty()) continue;

        size_t semi = t.find(';');
        if (semi == std::string::npos) continue;
        std::string decl = trim(t.substr(0, semi));

        std::istringstream dss(decl);
        std::vector<std::string> tokens;
        std::string tok;
        while (dss >> tok) {
            if (!is_precision_qualifier(tok)) {
                tokens.push_back(tok);
            }
        }
        if (tokens.size() < 3) continue;

        bool is_attr = (tokens[0] == "attribute" || tokens[0] == "in");
        bool is_unif = (tokens[0] == "uniform");
        bool is_vary = (tokens[0] == "varying");
        if (!is_attr && !is_unif && !is_vary) continue;

        std::string type_name = tokens[1];
        std::string rest;
        for (size_t i = 2; i < tokens.size(); i++) {
            if (i > 2) rest.push_back(' ');
            rest.append(tokens[i]);
        }

        size_t eq = rest.find('=');
        if (eq != std::string::npos) rest = trim(rest.substr(0, eq));

        int32_t arr_size = 1;
        std::string var_name = rest;
        size_t lbr = rest.find('[');
        if (lbr != std::string::npos) {
            size_t rbr = rest.find(']', lbr);
            var_name = trim(rest.substr(0, lbr));
            if (rbr != std::string::npos) {
                arr_size = eval_int_expr(rest.substr(lbr + 1, rbr - lbr - 1));
            }
        }

        ShaderVariableInfo info;
        info.name = var_name;
        info.gl_type = gl_type_from_name(type_name);
        info.array_size = arr_size;

        if (is_attr) {
            refl.attributes.push_back(std::move(info));
        } else if (is_unif) {
            refl.uniforms.push_back(std::move(info));
        } else {
            refl.varyings.push_back(std::move(info));
        }
    }

    return refl;
}

TranslatedProgramHLSL translate_program_to_hlsl(
    const std::string& vs_preprocessed,
    const std::string& fs_preprocessed,
    const std::unordered_map<std::string, int32_t>& bound_attrib_locations,
    uint32_t sint_mask,
    uint32_t uint_mask) {

    TranslatedProgramHLSL result;
    result.vs_preprocessed = vs_preprocessed;
    result.fs_preprocessed = fs_preprocessed;
    result.bound_attribs = bound_attrib_locations;

    ShaderReflection vs_refl = reflect_glsl(vs_preprocessed);
    ShaderReflection fs_refl = reflect_glsl(fs_preprocessed);

    // 1. Assign attribute locations
    int32_t next_loc = 0;
    for (const auto& a : vs_refl.attributes) {
        AttribLayout al;
        al.name = a.name;
        al.gl_type = a.gl_type;
        auto it = bound_attrib_locations.find(a.name);
        if (it != bound_attrib_locations.end()) {
            al.location = it->second;
        } else {
            while (true) {
                int32_t cand = next_loc++;
                bool used = false;
                for (const auto& [k, v] : bound_attrib_locations) {
                    if (v == cand) { used = true; break; }
                }
                if (!used) { al.location = cand; break; }
            }
        }
        result.attribs.push_back(std::move(al));
    }

    // 2. Merge unique uniforms & compute deterministic 16-byte register cbuffer layout.
    // Place large arrays (e.g. bone palettes u_matBones[64]) at the tail so unskinned draws can upload a compact CB.
    std::unordered_set<std::string> seen_u;
    uint32_t next_reg = 0;
    int32_t next_sampler = 0;
    auto add_uniform_list = [&](const std::vector<ShaderVariableInfo>& list, bool large_arrays) {
        for (const auto& u : list) {
            const bool is_sampler = (u.gl_type == 0x8B5E || u.gl_type == 0x8B60);
            const bool is_large = !is_sampler && (u.array_size > 8);
            if (is_large != large_arrays) continue;
            if (!seen_u.insert(u.name).second) continue;
            UniformMemberLayout mem;
            mem.name = u.name;
            mem.gl_type = u.gl_type;
            mem.array_size = std::max(1, u.array_size);
            if (is_sampler) {
                mem.is_sampler = true;
                mem.sampler_slot = next_sampler++;
            } else {
                uint32_t regs = register_count_for_gl_type(u.gl_type) * static_cast<uint32_t>(mem.array_size);
                mem.byte_offset = next_reg * 16;
                mem.byte_size = regs * 16;
                next_reg += regs;
            }
            result.uniforms.push_back(std::move(mem));
        }
    };
    add_uniform_list(vs_refl.uniforms, false);
    add_uniform_list(fs_refl.uniforms, false);
    add_uniform_list(vs_refl.uniforms, true);
    add_uniform_list(fs_refl.uniforms, true);
    uint32_t raw_cb_bytes = std::max(16u, next_reg * 16);
    result.cbuffer_size = (raw_cb_bytes + 255u) & ~255u;

    // 3. Build shared HLSL preamble (macros, helpers, cbuffer, textures/samplers, PSInput)
    std::ostringstream shared;
    shared << "#define vec2 float2\n"
           << "#define vec3 float3\n"
           << "#define vec4 float4\n"
           << "#define ivec2 int2\n"
           << "#define ivec3 int3\n"
           << "#define ivec4 int4\n"
           << "#define bvec2 bool2\n"
           << "#define bvec3 bool3\n"
           << "#define bvec4 bool4\n"
           << "#define mat2 row_major float2x2\n"
           << "#define mat3 row_major float3x3\n"
           << "#define mat4 row_major float4x4\n"
           << "#define mix lerp\n"
           << "#define fract frac\n"
           << "#define inversesqrt rsqrt\n"
           << "#define dFdx ddx\n"
           << "#define dFdy ddy\n"
           << "#define texture2D(tex, uv) lcs_tex2d_##tex(uv)\n"
           << "#define textureCube(tex, dir) tex.Sample(tex##_sampler, dir)\n"
           << "float gl_mod(float x, float y) { return x - y * floor(x / y); }\n"
           << "float2 gl_mod(float2 x, float2 y) { return x - y * floor(x / y); }\n"
           << "float3 gl_mod(float3 x, float3 y) { return x - y * floor(x / y); }\n"
           << "float4 gl_mod(float4 x, float4 y) { return x - y * floor(x / y); }\n"
           << "#define mod gl_mod\n"
           << "float gl_pow(float x, float y) { return (!(x > 0.0f)) ? (y == 0.0f ? 1.0f : 0.0f) : pow(x, y); }\n"
           << "float2 gl_pow(float2 x, float2 y) { return float2(gl_pow(x.x, y.x), gl_pow(x.y, y.y)); }\n"
           << "float3 gl_pow(float3 x, float3 y) { return float3(gl_pow(x.x, y.x), gl_pow(x.y, y.y), gl_pow(x.z, y.z)); }\n"
           << "float4 gl_pow(float4 x, float4 y) { return float4(gl_pow(x.x, y.x), gl_pow(x.y, y.y), gl_pow(x.z, y.z), gl_pow(x.w, y.w)); }\n"
           << "#define pow gl_pow\n"
           << "float gl_normalize(float v) { return (v != 0.0f) ? sign(v) : 0.0f; }\n"
           << "float2 gl_normalize(float2 v) { float l = dot(v, v); return (l > 1e-12f) ? (v * rsqrt(l)) : float2(0.0f, 0.0f); }\n"
           << "float3 gl_normalize(float3 v) { float l = dot(v, v); return (l > 1e-12f) ? (v * rsqrt(l)) : float3(0.0f, 0.0f, 0.0f); }\n"
           << "float4 gl_normalize(float4 v) { float l = dot(v, v); return (l > 1e-12f) ? (v * rsqrt(l)) : float4(0.0f, 0.0f, 0.0f, 0.0f); }\n"
           << "#define normalize gl_normalize\n"
           << "float4 gl_mat4_col(row_major float4x4 m, int c) { return m[c]; }\n"
           << "float3 gl_mat4_col(row_major float3x3 m, int c) { return m[c]; }\n\n";

    // cbuffer b0
    bool has_cbuf_vars = false;
    for (const auto& u : result.uniforms) {
        if (!u.is_sampler) { has_cbuf_vars = true; break; }
    }
    if (has_cbuf_vars) {
        shared << "cbuffer GLUniforms : register(b0) {\n";
        for (const auto& u : result.uniforms) {
            if (u.is_sampler) continue;
            uint32_t reg_idx = u.byte_offset / 16;
            shared << "    " << hlsl_type_from_gl_type(u.gl_type) << " " << u.name;
            if (u.array_size > 1) {
                shared << "[" << u.array_size << "]";
            }
            shared << " : packoffset(c" << reg_idx << ");\n";
        }
        shared << "};\n\n";
    }

    shared << "cbuffer PCColorGradeCB : register(b1) {\n"
           << "    float4 pc_color_grade;\n"
           << "};\n\n";

    for (const auto& u : result.uniforms) {
        if (!u.is_sampler) continue;
        shared << hlsl_type_from_gl_type(u.gl_type) << " " << u.name
               << " : register(t" << u.sampler_slot << ");\n";
        shared << "SamplerState " << u.name << "_sampler : register(s" << u.sampler_slot << ");\n";
    }
    shared << "\n";

    // Merge varyings (prefer VS varyings, fallback FS varyings)
    std::vector<ShaderVariableInfo> varyings = !vs_refl.varyings.empty() ? vs_refl.varyings : fs_refl.varyings;
    shared << "struct PSInput {\n";
    shared << "    float4 gl_Position : SV_POSITION;\n";
    for (size_t i = 0; i < varyings.size(); i++) {
        shared << "    " << hlsl_type_from_gl_type(varyings[i].gl_type) << " "
               << varyings[i].name << " : TEXCOORD" << i << ";\n";
    }
    shared << "};\n\n";

    // Variable classification table for matrix rewrite
    std::unordered_map<std::string, ExprKind> var_types;
    for (const auto& u : result.uniforms) {
        if (u.gl_type == 0x8B5A || u.gl_type == 0x8B5B || u.gl_type == 0x8B5C) {
            var_types[u.name] = ExprKind::Mat;
        } else if (u.gl_type == 0x1406 || u.gl_type == 0x1404 || u.gl_type == 0x8B56) {
            var_types[u.name] = ExprKind::Scalar;
        } else {
            var_types[u.name] = ExprKind::Vec;
        }
    }
    for (const auto& a : result.attribs) var_types[a.name] = ExprKind::Vec;
    for (const auto& v : varyings) var_types[v.name] = ExprKind::Vec;

    // 4. Build Vertex Shader HLSL
    {
        auto vs_input_type = [&](const AttribLayout& a) -> std::string {
            if (a.location >= 0 && a.location < 16) {
                const uint32_t bit = 1u << static_cast<uint32_t>(a.location);
                if (sint_mask & bit) return "int4";
                if (uint_mask & bit) return "uint4";
            }
            return hlsl_type_from_gl_type(a.gl_type);
        };
        auto vs_input_assign = [&](const AttribLayout& a) -> std::string {
            if (a.location >= 0 && a.location < 16) {
                const uint32_t bit = 1u << static_cast<uint32_t>(a.location);
                if ((sint_mask | uint_mask) & bit) {
                    switch (a.gl_type) {
                        case 0x1406: return "(float)_in." + a.name + ".x";
                        case 0x8B50: return "(float2)_in." + a.name + ".xy";
                        case 0x8B51: return "(float3)_in." + a.name + ".xyz";
                        case 0x1404: return "(int)_in." + a.name + ".x";
                        case 0x8B53: return "(int2)_in." + a.name + ".xy";
                        case 0x8B54: return "(int3)_in." + a.name + ".xyz";
                        case 0x8B55: return "(int4)_in." + a.name;
                        default:     return "(float4)_in." + a.name;
                    }
                }
            }
            return "_in." + a.name;
        };

        std::ostringstream vs;
        vs << shared.str();
        vs << "struct VSInput {\n";
        for (const auto& a : result.attribs) {
            vs << "    " << vs_input_type(a) << " "
               << a.name << " : ATTR" << a.location << ";\n";
        }
        if (result.attribs.empty()) {
            vs << "    uint vertex_id : SV_VertexID;\n";
        }
        vs << "};\n\n";

        vs << "static float4 gl_Position;\n";
        for (const auto& a : result.attribs) {
            vs << "static " << hlsl_type_from_gl_type(a.gl_type) << " " << a.name << ";\n";
        }
        for (const auto& v : varyings) {
            vs << "static " << hlsl_type_from_gl_type(v.gl_type) << " " << v.name << ";\n";
        }
        vs << "\n";

        std::string vs_body = strip_top_level_decls(vs_preprocessed);
        vs << rewrite_glsl_body_to_hlsl(vs_body, var_types) << "\n";

        vs << "PSInput main(VSInput _in) {\n";
        for (const auto& a : result.attribs) {
            vs << "    " << a.name << " = " << vs_input_assign(a) << ";\n";
        }
        vs << "    gl_Position = float4(0.0, 0.0, 0.0, 1.0);\n";
        for (const auto& v : varyings) {
            vs << "    " << v.name << " = (" << hlsl_type_from_gl_type(v.gl_type) << ")0;\n";
        }
        vs << "    gl_main();\n";
        vs << "    PSInput _out;\n";
        vs << "    _out.gl_Position = gl_Position;\n";
        vs << "    // Remap OpenGL clip space [-w, +w] to Direct3D 12 [0, +w] to prevent near-plane clipping\n";
        vs << "    _out.gl_Position.z = (_out.gl_Position.z + _out.gl_Position.w) * 0.5f;\n";
        for (const auto& v : varyings) {
            vs << "    _out." << v.name << " = " << v.name << ";\n";
        }
        vs << "    return _out;\n";
        vs << "}\n";
        result.vs_hlsl = vs.str();
    }

    // 5. Build Pixel Shader HLSL
    {
        std::ostringstream ps;
        ps << shared.str();

        // texture2D() goes through one helper per sampler. Samplers t0/t1 can switch per draw to
        // stochastic (hex-tiling) sampling, as in SkyGfx Extended: three samples at pseudo-random
        // offsets pinned to the vertices of a triangle grid, blended by barycentric weights, with
        // the derivatives of the original UV. pc_color_grade.w (set per draw by the D3D12 backend)
        // holds bit i = sampler t<i> is tagged, bit 2 = sharpen the weights (w^4) to keep contrast.
        bool has_sto = false;
        for (const auto& u : result.uniforms) {
            if (u.is_sampler && u.gl_type == 0x8B5E && u.sampler_slot < 2) has_sto = true;
        }
        if (has_sto) {
            ps << "float2 lcs_sto_hash(float2 p) {\n"
               << "    return frac(sin(fmod(float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3))), 3.14159)) * 43758.5453);\n"
               << "}\n"
               << "float4 lcs_sample_sto(Texture2D t, SamplerState s, float2 uv, uint bit) {\n"
               << "    const float2 dx = ddx(uv);\n"
               << "    const float2 dy = ddy(uv);\n"
               << "    const uint mask = (uint)pc_color_grade.w;\n"
               << "    [branch] if ((mask & bit) != 0u) {\n"
               << "        const float2 skew = float2(3.46410162 * uv.x, 4.0 * uv.y - 2.0 * uv.x);\n"
               << "        const float2 vid = floor(skew);\n"
               << "        const float2 f = frac(skew);\n"
               << "        const float fz = 1.0 - f.x - f.y;\n"
               << "        float2 v0 = vid + float2(1.0, 1.0);\n"
               << "        float2 v1 = vid + float2(1.0, 0.0);\n"
               << "        float2 v2 = vid + float2(0.0, 1.0);\n"
               << "        float3 w = float3(-fz, 1.0 - f.y, 1.0 - f.x);\n"
               << "        if (fz > 0.0) { v0 = vid; v1 = vid + float2(0.0, 1.0); v2 = vid + float2(1.0, 0.0); w = float3(fz, f.y, f.x); }\n"
               << "        if ((mask & 4u) != 0u) { w *= w; w *= w; w /= (w.x + w.y + w.z); }\n"
               << "        return t.SampleGrad(s, uv + lcs_sto_hash(v0), dx, dy) * w.x +\n"
               << "               t.SampleGrad(s, uv + lcs_sto_hash(v1), dx, dy) * w.y +\n"
               << "               t.SampleGrad(s, uv + lcs_sto_hash(v2), dx, dy) * w.z;\n"
               << "    }\n"
               << "    return t.SampleGrad(s, uv, dx, dy);\n"
               << "}\n";
        }
        for (const auto& u : result.uniforms) {
            if (!u.is_sampler || u.gl_type != 0x8B5E) continue;
            ps << "float4 lcs_tex2d_" << u.name << "(float2 uv) { return ";
            if (u.sampler_slot < 2) {
                ps << "lcs_sample_sto(" << u.name << ", " << u.name << "_sampler, uv, "
                   << (1u << u.sampler_slot) << "u)";
            } else {
                ps << u.name << ".Sample(" << u.name << "_sampler, uv)";
            }
            ps << "; }\n";
        }
        ps << "\n";
        ps << "static float4 gl_FragColor;\n";
        for (const auto& v : varyings) {
            ps << "static " << hlsl_type_from_gl_type(v.gl_type) << " " << v.name << ";\n";
        }
        ps << "\n";

        std::string fs_body = strip_top_level_decls(fs_preprocessed);
        ps << rewrite_glsl_body_to_hlsl(fs_body, var_types) << "\n";

        ps << "float4 main(PSInput _in) : SV_TARGET {\n";
        for (const auto& v : varyings) {
            ps << "    " << v.name << " = _in." << v.name << ";\n";
        }
        ps << "    gl_FragColor = float4(0.0, 0.0, 0.0, 1.0);\n";
        ps << "    gl_main();\n";
        ps << "    return gl_FragColor;\n";
        ps << "}\n";
        result.ps_hlsl = ps.str();
    }

    return result;
}

bool compile_shader_source(const char* glsl_src, int stage) {
    (void)stage;
    if (!glsl_src) return false;
    std::string pp = preprocess_glsl(glsl_src);
    return !pp.empty();
}

} // namespace lcs::graphics
