#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lcs::game {

// Resolves a canonical speaker name for a GTA LCS GXT key / table / text.
// Returns empty string when the key is an instruction/objective or unknown.
std::string resolve_dialogue_speaker(const std::string& gxt_key,
                                     const std::string& gxt_table,
                                     const std::string& clean_text,
                                     int ui_language);

// Splits dual-speaker Rockstar cutscene lines ("- Foo ~n~ - Bar") into sequential
// (speaker, clean_line) segments when a key embeds a fast two-person exchange.
std::vector<std::pair<std::string, std::string>> split_dialogue_segments(
    const std::string& gxt_key,
    const std::string& gxt_table,
    const std::string& formatted_text_with_newlines,
    int ui_language);

} // namespace lcs::game
