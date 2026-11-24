#include "game/lcs_dialogue_speakers.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <string_view>

namespace lcs::game {
namespace {
const char* code_to_name(char c, int ui_lang) {
    const bool pt = (ui_lang == 7);
    const bool es = (ui_lang == 4);
    const bool fr = (ui_lang == 1);
    const bool de = (ui_lang == 2);
    const bool it = (ui_lang == 3);
    switch (c) {
        case 'S': return "Salvatore Leone";
        case 'T': return "Toni Cipriani";
        case 'V': return "Vincenzo Cilli";
        case 'M': return "Ma Cipriani";
        case 'R': return "Maria";
        case 'J': return "JD O'Toole";
        case 'D': return "Donald Love";
        case 'N': return "Ned Burner";
        case 'Y': return "Ray Machowski";
        case 'L': return "Leon McAffrey";
        case 'K': return "Toshiko Kasen";
        case 'U': return pt ? "Tio Leone" : es ? "Tío Leone" : fr ? "Oncle Leone" : de ? "Onkel Leone" : it ? "Zio Leone" : "Uncle Leone";
        case 'P': return "Lionel";
        case 'O': return "Mayor O'Donovan";
        case 'B': return pt ? "Jornalista" : es ? "Reportero" : fr ? "Journaliste" : de ? "Reporter" : it ? "Giornalista" : "Reporter";
        case 'W': return pt ? "Polícia" : es ? "Policía" : fr ? "Police" : de ? "Polizei" : it ? "Polizia" : "Police";
        case 'H': return "Jane Hopper";
        case 'E': return "8-Ball";
        case 'A': return "Miguel";
        case 'C': return "Giovanni Casa";
        case 'Z': return "Wayne";
        case 'G': return pt ? "Capanga" : es ? "Matón" : fr ? "Homme de main" : "Associate";
        default:  return "";
    }
}
static const std::unordered_map<std::string_view, char> kExactMap = {
    {"BIK_01", 'T'}, {"BIK_02", 'R'}, {"BIK_03", 'T'}, {"BIK_04", 'T'}, {"BIK_05", 'T'}, {"BIK_06", 'T'},
    {"BIK_08", 'T'}, {"BIK_09", 'R'}, {"BIK_10", 'T'}, {"BIK_11", 'R'}, {"BIK_12", 'T'}, {"BIK_13", 'R'},
    {"BIK_14", 'R'}, {"BIK_15", 'R'}, {"BIK_16", 'T'}, {"BIK_17", 'T'}, {"BIK_18", 'R'}, {"BIK_19", 'R'},
    {"BIK_20", 'T'}, {"BIK_22", 'T'}, {"BIK_23", 'R'}, {"BIK_24", 'R'}, {"BONE_01", 'T'}, {"BONE_02", 'T'},
    {"BONE_03", 'T'}, {"BONE_04", 'J'}, {"BONE_05", 'J'}, {"BONE_06", 'T'}, {"BONE_07", 'J'}, {"BONE_08", 'J'},
    {"BONE_09", 'T'}, {"BONE_10", 'J'}, {"BONE_11", 'J'}, {"BONE_12", 'J'}, {"BONE_14", 'T'}, {"BONE_16", 'J'},
    {"BONE_17", 'J'}, {"BONE_18", 'J'}, {"BONE_19", 'J'}, {"BONE_20", 'J'}, {"BONE_21", 'J'}, {"CAD4_AA", 'S'},
    {"CAD4_AB", 'S'}, {"CAD4_AC", 'S'}, {"CAD4_AD", 'S'}, {"CAD4_AE", 'S'}, {"CAD4_AF", 'S'}, {"CAD5_AA", 'S'},
    {"CAD5_AB", 'S'}, {"CAD5_AC", 'S'}, {"CAD5_AD", 'S'}, {"CAD5_AE", 'S'}, {"CAD5_AF", 'S'}, {"CAMP_01", 'D'},
    {"CAMP_02", 'T'}, {"CAMP_03", 'D'}, {"CAMP_04", 'D'}, {"CAMP_05", 'D'}, {"CAMP_06", 'D'}, {"CAMP_07", 'T'},
    {"CAMP_08", 'D'}, {"CAMP_09", 'D'}, {"CAMP_10", 'D'}, {"CAMP_11", 'D'}, {"CAMP_12", 'D'}, {"CAMP_13", 'T'},
    {"CAMP_14", 'D'}, {"CAMP_15", 'D'}, {"CAMP_16", 'T'}, {"CAMP_17", 'D'}, {"CAMP_18", 'T'}, {"CAMP_19", 'T'},
    {"CAMP_20", 'D'}, {"CAMP_21", 'D'}, {"CAMP_22", 'D'}, {"CAMP_23", 'T'}, {"CAMP_24", 'D'}, {"CAMP_25", 'D'},
    {"CAMP_26", 'D'}, {"CASH_01", 'T'}, {"CASH_02", 'K'}, {"CASH_03", 'T'}, {"CASH_04", 'T'}, {"CASH_06", 'K'},
    {"CASH_07", 'T'}, {"CASH_08", 'K'}, {"CASH_09", 'T'}, {"CASH_10", 'K'}, {"CASH_11", 'T'}, {"CASH_12", 'T'},
    {"CASH_13", 'T'}, {"CASH_14", 'K'}, {"CASH_15", 'K'}, {"CASH_16", 'K'}, {"CASH_17", 'K'}, {"CASH_18", 'K'},
    {"CONT_01", 'S'}, {"CONT_02", 'S'}, {"CONT_04", 'S'}, {"CONT_06", 'R'}, {"CONT_07", 'S'}, {"CONT_08", 'S'},
    {"CONT_09", 'S'}, {"CONT_10", 'R'}, {"CONT_12", 'R'}, {"CONT_13", 'R'}, {"CONT_15", 'R'}, {"CONT_16", 'R'},
    {"CONT_17", 'S'}, {"CONT_18", 'S'}, {"CONT_19", 'S'}, {"CONT_20", 'S'}, {"CONT_21", 'S'}, {"CONT_22", 'R'},
    {"CONT_23", 'S'}, {"CONT_24", 'S'}, {"CONT_25", 'S'}, {"CONT_26", 'R'}, {"CONT_27", 'R'}, {"CONT_28", 'R'},
    {"CONT_29", 'R'}, {"CONT_31", 'R'}, {"CONT_33", 'S'}, {"CONT_34", 'S'}, {"CONT_35", 'S'}, {"CONT_36", 'S'},
    {"CONT_37", 'S'}, {"CONT_38", 'S'}, {"CONT_39", 'S'}, {"CONT_40", 'S'}, {"CONT_41", 'S'}, {"CONT_42", 'S'},
    {"CONT_43", 'S'}, {"CRAZ_01", 'L'}, {"CRAZ_02", 'L'}, {"CRAZ_03", 'L'}, {"CRAZ_04", 'L'}, {"CRAZ_05", 'Y'},
    {"CRAZ_06", 'Y'}, {"CRAZ_07", 'L'}, {"CRAZ_08", 'L'}, {"CRAZ_09", 'L'}, {"CRAZ_10", 'L'}, {"CRAZ_11", 'Y'},
    {"CRAZ_14", 'Y'}, {"CRAZ_15", 'L'}, {"CRAZ_16", 'L'}, {"CRAZ_17", 'L'}, {"CRAZ_18", 'L'}, {"CRAZ_19", 'Y'},
    {"DEAD_01", 'K'}, {"DEAD_02", 'K'}, {"DEAD_03", 'T'}, {"DEAD_04", 'K'}, {"DEAD_05", 'K'}, {"DEAD_06", 'T'},
    {"DEAD_07", 'K'}, {"DEAD_08", 'K'}, {"DEAD_09", 'K'}, {"DEAD_10", 'K'}, {"DEAD_11", 'T'}, {"DEAD_12", 'K'},
    {"DEAD_13", 'K'}, {"DEAD_14", 'K'}, {"DEAD_15", 'T'}, {"DEAD_16", 'K'}, {"DEAD_17", 'K'}, {"DEAD_18", 'K'},
    {"DEAD_19", 'K'}, {"DONS7AA", 'D'}, {"DONS7AB", 'D'}, {"DONS7AC", 'T'}, {"DONS7AD", 'T'}, {"DONS7AE", 'D'},
    {"DONS7AF", 'D'}, {"DONS7AG", 'D'}, {"DRIV_01", 'S'}, {"DRIV_02", 'T'}, {"DRIV_03", 'S'}, {"DRIV_04", 'S'},
    {"DRIV_06", 'S'}, {"DRIV_07", 'S'}, {"DRIV_08", 'S'}, {"DRIV_09", 'T'}, {"DRIV_10", 'S'}, {"DRIV_11", 'S'},
    {"DRIV_12", 'S'}, {"DRIV_13", 'S'}, {"DRIV_14", 'S'}, {"DRIV_15", 'S'}, {"DRIV_16", 'S'}, {"DRIV_17", 'S'},
    {"ELEC_01", 'T'}, {"ELEC_02", 'T'}, {"ELEC_03", 'D'}, {"ELEC_04", 'T'}, {"ELEC_05", 'T'}, {"ELEC_06", 'D'},
    {"ELEC_07", 'B'}, {"ELEC_08", 'B'}, {"ELEC_09", 'B'}, {"ELEC_11", 'B'}, {"ELEC_12", 'B'}, {"ELEC_13", 'B'},
    {"ELEC_14", 'D'}, {"ELEC_15", 'D'}, {"ELEC_16", 'T'}, {"ELEC_17", 'D'}, {"ELEC_18", 'T'}, {"ELEC_19", 'D'},
    {"ELEC_20", 'D'}, {"ELEC_21", 'D'}, {"ELEC_22", 'D'}, {"ELEC_23", 'D'}, {"FIN1_01", 'S'}, {"FIN1_02", 'S'},
    {"FIN1_03", 'O'}, {"FIN1_04", 'S'}, {"FIN1_05", 'O'}, {"FIN1_06", 'S'}, {"FIN1_07", 'S'}, {"FIN1_08", 'O'},
    {"FIN1_09", 'S'}, {"FIN1_10", 'S'}, {"FIN1_11", 'O'}, {"FIN1_12", 'S'}, {"FIN1_13", 'S'}, {"FIN1_14", 'S'},
    {"FIN1_15", 'S'}, {"FIN2_01", 'S'}, {"FIN2_02", 'S'}, {"FIN2_03", 'U'}, {"FIN2_04", 'S'}, {"FIN2_05", 'U'},
    {"FIN2_07", 'U'}, {"FIN2_08", 'S'}, {"FIN2_09", 'S'}, {"FIN2_10", 'T'}, {"FIN2_11", 'S'}, {"FIN2_12", 'S'},
    {"FIN2_13", 'S'}, {"FIN2_14", 'T'}, {"FIN2_15", 'S'}, {"FIN2_16", 'S'}, {"FIN2_18", 'S'}, {"FIN2_19", 'S'},
    {"FIN2_20", 'S'}, {"FIN2_21", 'S'}, {"FIN2_22", 'S'}, {"FIN2_23", 'S'}, {"HOMS_01", 'S'}, {"HOMS_02", 'S'},
    {"HOMS_03", 'S'}, {"HOMS_04", 'S'}, {"HOMS_05", 'S'}, {"HOMS_06", 'S'}, {"HOMS_07", 'S'}, {"HOMS_08", 'S'},
    {"HOMS_09", 'S'}, {"HOMS_10", 'T'}, {"HOMS_11", 'T'}, {"HOMS_12", 'T'}, {"HOMS_13", 'T'}, {"HOMS_14", 'S'},
    {"HOMS_15", 'S'}, {"HOMS_16", 'S'}, {"HOMS_17", 'S'}, {"HOMS_18", 'T'}, {"HOMS_19", 'S'}, {"HOMS_20", 'S'},
    {"HOMS_21", 'V'}, {"HOMS_22", 'V'}, {"HOTW_01", 'V'}, {"HOTW_02", 'V'}, {"HOTW_03", 'V'}, {"HOTW_04", 'V'},
    {"HOTW_05", 'V'}, {"HOTW_06", 'V'}, {"HOTW_07", 'V'}, {"HOTW_09", 'V'}, {"HOTW_11", 'T'}, {"HOTW_12", 'V'},
    {"HOTW_13", 'V'}, {"HOTW_15", 'V'}, {"HOTW_16", 'T'}, {"HOTW_17", 'V'}, {"HOTW_18", 'V'}, {"HOTW_19", 'V'},
    {"HOTW_20", 'V'}, {"HOTW_21", 'T'}, {"HOTW_22", 'V'}, {"HOTW_23", 'V'}, {"HOTW_24", 'V'}, {"HOTW_25", 'V'},
    {"HOTW_28", 'T'}, {"KIDN_02", 'J'}, {"KIDN_03", 'J'}, {"KIDN_04", 'T'}, {"KIDN_06", 'J'}, {"KIDN_07", 'J'},
    {"KIDN_08", 'J'}, {"KIDN_09", 'J'}, {"KIDN_10", 'J'}, {"KIDN_11", 'T'}, {"KIDN_13", 'J'}, {"KIDN_14", 'J'},
    {"KIDN_15", 'J'}, {"KIDN_16", 'J'}, {"KIDN_17", 'J'}, {"KIDN_18", 'J'}, {"KIDN_19", 'J'}, {"KIDN_20", 'T'},
    {"KIDN_21", 'J'}, {"KIDN_23", 'T'}, {"LAND_01", 'D'}, {"LAND_02", 'D'}, {"LAND_03", 'T'}, {"LAND_04", 'D'},
    {"LAND_05", 'D'}, {"LAND_06", 'D'}, {"LAND_07", 'D'}, {"LAND_08", 'D'}, {"LAND_09", 'T'}, {"LAND_10", 'D'},
    {"LAND_11", 'T'}, {"LAND_12", 'D'}, {"LAND_13", 'D'}, {"LAND_14", 'D'}, {"LAND_16", 'D'}, {"LAND_18", 'D'},
    {"LAND_19", 'D'}, {"MORG_01", 'D'}, {"MORG_02", 'D'}, {"MORG_03", 'T'}, {"MORG_04", 'D'}, {"MORG_05", 'D'},
    {"MORG_06", 'D'}, {"MORG_08", 'T'}, {"MORG_09", 'D'}, {"MORG_10", 'D'}, {"MORG_11", 'D'}, {"MORG_12", 'D'},
    {"MORG_13", 'D'}, {"MORG_14", 'D'}, {"MORG_15", 'D'}, {"MPHPCS2", 'S'}, {"MPHPCS3", 'S'}, {"MPHPCS4", 'S'},
    {"MPHPCS5", 'S'}, {"MPHPCS6", 'S'}, {"MPHPCS7", 'S'}, {"MPRCS2", 'S'}, {"MPRCS3", 'S'}, {"MPRCS4", 'S'},
    {"MPRCS5", 'S'}, {"MPRCS6", 'S'}, {"MPTCS2", 'S'}, {"MPTCS3", 'S'}, {"MPTCS4", 'S'}, {"MPTCS5", 'S'},
    {"MPTCS6", 'S'}, {"MPWCS3", 'S'}, {"MPWCS4", 'S'}, {"MPWCS5", 'S'}, {"MPWCS6", 'S'}, {"MPWCS7", 'S'},
    {"OVER_01", 'R'}, {"OVER_02", 'T'}, {"OVER_03", 'R'}, {"OVER_04", 'T'}, {"OVER_05", 'R'}, {"OVER_06", 'R'},
    {"OVER_07", 'R'}, {"OVER_08", 'R'}, {"OVER_09", 'R'}, {"OVER_10", 'R'}, {"OVER_11", 'R'}, {"OVER_12", 'T'},
    {"OVER_13", 'R'}, {"OVER_14", 'R'}, {"OVER_15", 'R'}, {"OVER_17", 'R'}, {"OVER_18", 'R'}, {"OVER_19", 'R'},
    {"OVER_20", 'R'}, {"OVER_21", 'R'}, {"POMOB1A", 'M'}, {"POMOB1B", 'M'}, {"POMOB1C", 'M'}, {"POMOB1D", 'M'},
    {"POMOB2A", 'R'}, {"POMOB2B", 'R'}, {"POMOB2C", 'R'}, {"POMOB3A", 'J'}, {"POMOB3B", 'J'}, {"POMOB3C", 'J'},
    {"POMOB4A", 'T'}, {"POMOB4B", 'T'}, {"POMOB4C", 'V'}, {"POMOB4D", 'V'}, {"POMOB4E", 'T'}, {"POMOB4F", 'T'},
    {"POMOB4G", 'V'}, {"POMOB4H", 'V'}, {"POMOB4I", 'V'}, {"POMOB4J", 'V'}, {"POMOB5A", 'S'}, {"POMOB6A", 'S'},
    {"POMOB6B", 'S'}, {"POMOB6C", 'S'}, {"PO_MOB1", 'M'}, {"PO_MOB2", 'R'}, {"PO_MOB3", 'J'}, {"PO_MOB4", 'V'},
    {"PO_MOB5", 'S'}, {"PO_MOB6", 'S'}, {"RMN_AA", 'B'}, {"RMN_AB", 'B'}, {"RMN_AC", 'B'}, {"RMN_AD", 'B'},
    {"RMN_AE", 'B'}, {"RMN_AF", 'B'}, {"RMN_AG", 'B'}, {"RMN_AH", 'B'}, {"RMN_AI", 'B'}, {"RUFJ_01", 'G'},
    {"RUFJ_02", 'S'}, {"RUFJ_03", 'G'}, {"RUFJ_04", 'S'}, {"RUFJ_05", 'G'}, {"RUFJ_06", 'S'}, {"RUFJ_07", 'S'},
    {"RUFJ_08", 'S'}, {"RUFJ_09", 'S'}, {"RUFJ_10", 'P'}, {"RUFJ_11", 'P'}, {"RUFJ_12", 'P'}, {"RUFJ_13", 'S'},
    {"RUFJ_14", 'S'}, {"RUFJ_15", 'S'}, {"RUFJ_16", 'S'}, {"RUFJ_18", 'S'}, {"RUFJ_19", 'S'}, {"RUFJ_20", 'P'},
    {"RUFJ_22", 'S'}, {"RUFJ_23", 'S'}, {"RUFJ_24", 'S'}, {"RUFJ_25", 'P'}, {"RUFJ_26", 'S'}, {"SAL1_D1", 'G'},
    {"SAL1_D2", 'G'}, {"SAL1_D3", 'G'}, {"SAL1_D4", 'G'}, {"SAL1_M1", 'T'}, {"SAL1_M2", 'H'}, {"SAL1_M3", 'H'},
    {"SAL5_AA", 'A'}, {"SAL5_AB", 'T'}, {"SAL5_AD", 'A'}, {"SAL5_AE", 'T'}, {"SAL5_AF", 'A'}, {"SAL5_AI", 'A'},
    {"SAL5_AJ", 'T'}, {"SAL812A", 'T'}, {"SAL8_18", 'S'}, {"SAL8_22", 'S'}, {"SAL8_28", 'T'}, {"SHMOB1A", 'T'},
    {"SHMOB1B", 'S'}, {"SHMOB4C", 'S'}, {"SHMOB5A", 'D'}, {"SHMOB5B", 'D'}, {"SHMOB5C", 'D'}, {"SHMOB5D", 'T'},
    {"SHMOB5E", 'D'}, {"SHMOB5F", 'D'}, {"SHMOB6A", 'E'}, {"SH_MOB2", 'T'}, {"SH_MOB4", 'S'}, {"SH_MOB5", 'D'},
    {"SH_MOB6", 'E'}, {"SICI_01", 'S'}, {"SICI_02", 'S'}, {"SICI_04", 'S'}, {"SICI_05", 'S'}, {"SICI_06", 'T'},
    {"SICI_07", 'S'}, {"SICI_08", 'S'}, {"SICI_09", 'S'}, {"SICI_10", 'S'}, {"SICI_11", 'S'}, {"SICI_12", 'S'},
    {"SICI_13", 'S'}, {"SICI_14", 'T'}, {"SICI_15", 'S'}, {"SICI_16", 'S'}, {"STMO10A", 'L'}, {"STMO10B", 'L'},
    {"STMO10C", 'L'}, {"STMO10D", 'L'}, {"STMO10E", 'L'}, {"STMOB1A", 'D'}, {"STMOB1B", 'D'}, {"STMOB2A", 'L'},
    {"STMOB2B", 'L'}, {"STMOB2C", 'L'}, {"STMOB3A", 'S'}, {"STMOB3B", 'S'}, {"STMOB3C", 'S'}, {"STMOB3D", 'S'},
    {"STMOB3E", 'S'}, {"STMOB3F", 'S'}, {"STMOB4A", 'S'}, {"STMOB4B", 'S'}, {"STMOB4C", 'S'}, {"STMOB4D", 'S'},
    {"STMOB4E", 'S'}, {"STMOB5A", 'S'}, {"STMOB5B", 'S'}, {"STMOB5C", 'S'}, {"STMOB5D", 'S'}, {"STMOB5E", 'S'},
    {"STMOB6A", 'S'}, {"STMOB6B", 'S'}, {"STMOB6C", 'S'}, {"STMOB7A", 'S'}, {"STMOB7B", 'S'}, {"STMOB7C", 'S'},
    {"STMOB8A", 'S'}, {"STMOB8B", 'S'}, {"STMOB8C", 'S'}, {"STMOB9A", 'M'}, {"STMOB9B", 'M'}, {"STMOB9C", 'M'},
    {"STMOB9D", 'M'}, {"STMOB9E", 'M'}, {"STMOB9F", 'M'}, {"STMOB9G", 'M'}, {"STMOB9H", 'M'}, {"STMOB9I", 'M'},
    {"ST_MOB1", 'D'}, {"ST_MOB2", 'T'}, {"ST_MOB3", 'T'}, {"ST_MOB9", 'T'}, {"THEO_01", 'G'}, {"THEO_02", 'S'},
    {"THEO_04", 'S'}, {"THEO_05", 'S'}, {"THEO_06", 'S'}, {"THEO_07", 'S'}, {"THEO_08", 'S'}, {"THEO_09", 'S'},
    {"THEO_10", 'S'}, {"THEO_11", 'S'}, {"THEO_12", 'T'}, {"THEO_13", 'S'}, {"THEO_14", 'S'}, {"THEO_15", 'S'},
    {"THEO_16", 'S'}, {"THEO_17", 'S'}, {"THEO_18", 'S'}, {"THEO_19", 'S'}, {"THEO_20", 'S'}, {"THEO_21", 'S'},
    {"THEO_22", 'S'}, {"THEO_23", 'S'}, {"THEO_24", 'S'}, {"THEO_26", 'S'}, {"THEO_27", 'S'}, {"THEO_28", 'T'},
    {"THEO_29", 'S'}, {"THEO_30", 'S'}, {"VIC1C10", 'V'}, {"VIC1C11", 'V'}, {"VIC1I11", 'V'}, {"VIC1I12", 'V'},
    {"VIC1I13", 'V'}, {"VIC1I14", 'T'}, {"VIC1_9A", 'V'}, {"VIC1_C1", 'V'}, {"VIC1_C2", 'T'}, {"VIC1_C3", 'V'},
    {"VIC1_C4", 'V'}, {"VIC1_C5", 'V'}, {"VIC1_C6", 'V'}, {"VIC1_C7", 'V'}, {"VIC1_C8", 'V'}, {"VIC1_C9", 'V'},
    {"VIC1_D1", 'V'}, {"VIC1_D2", 'V'}, {"VIC1_D3", 'V'}, {"VIC2M1", 'V'}, {"VIC2M2", 'V'}, {"VIC2M3", 'V'},
    {"VIC2M4", 'V'}, {"VIC2M5", 'T'}, {"VIC2M6", 'V'}, {"VIC2M7", 'V'}, {"VIC2_E", 'T'}, {"VIC2_E2", 'G'},
    {"VIC2_E3", 'G'}, {"VIC2_E4", 'T'}, {"VIC2_EX", 'G'}, {"VIC2_G", 'G'}, {"VIC2_G2", 'G'}, {"VIC2_G3", 'G'},
    {"VIC2_G4", 'G'}, {"VIC2_G5", 'G'}, {"VIC2_I", 'G'}, {"VIC3_AA", 'T'}, {"VIC3_AB", 'V'}, {"VIC3_AD", 'V'},
    {"VIC3_AE", 'V'}, {"VIC3_AF", 'V'}, {"VIC3_AG", 'T'}, {"VIC3_AH", 'V'}, {"VIC3_AJ", 'V'}, {"VIC3_AK", 'V'},
    {"VIC3_AL", 'V'}, {"VIC3_AM", 'T'}, {"VIC3_AN", 'V'}, {"VIC3_BA", 'T'}, {"VIC3_BB", 'T'}, {"VIC3_BC", 'T'},
    {"VIC4_AA", 'V'}, {"VIC4_AB", 'T'}, {"VIC4_AC", 'V'}, {"VIC4_AD", 'V'}, {"VIC4_AE", 'T'}, {"VIC4_AG", 'V'},
    {"VIC4_AH", 'V'}, {"VIC4_AI", 'V'}, {"VIC4_AJ", 'V'}, {"VIC4_AK", 'V'}, {"VIC4_AL", 'V'}, {"VIC4_AM", 'V'},
    {"VIC4_AN", 'V'}, {"VIC4_BA", 'J'}, {"VIC4_BB", 'T'}, {"VIC4_BC", 'J'}, {"VIC4_BD", 'J'}, {"VIC4_BE", 'T'},
    {"VIC4_BF", 'J'}, {"VIC4_BG", 'J'}, {"VIC4_BH", 'J'}, {"VIC4_CA", 'G'}, {"VIC4_CB", 'G'}, {"VIC5I10", 'V'},
    {"VIC5I11", 'V'}, {"VIC5_G1", 'G'}, {"VIC5_G2", 'G'}, {"VIC5_G3", 'G'}, {"VIC5_G4", 'G'}, {"VIC5_G5", 'G'},
    {"VIC5_G6", 'G'}, {"VIC5_I0", 'V'}, {"VIC5_I1", 'T'}, {"VIC5_I2", 'V'}, {"VIC5_I3", 'V'}, {"VIC5_I4", 'V'},
    {"VIC5_I6", 'V'}, {"VIC5_I7", 'T'}, {"VIC5_I8", 'V'}, {"VIC5_I9", 'V'}, {"VIC6_AA", 'V'}, {"VIC6_AB", 'T'},
    {"VIC6_AC", 'V'}, {"VIC6_AD", 'V'}, {"VIC6_AE", 'V'}, {"VIC6_AF", 'V'}, {"VIC6_AG", 'V'}, {"VIC6_AH", 'V'},
    {"VIC6_AI", 'V'}, {"VIC6_AJ", 'V'}, {"VIC6_AK", 'T'}, {"VIC6_AL", 'T'}, {"VIC6_AM", 'T'}, {"VIC6_AN", 'T'},
    {"VIC6_AO", 'V'}, {"VIC6_BA", 'T'}, {"VIC6_BB", 'G'}, {"VIC710A", 'V'}, {"VIC710B", 'V'}, {"VIC711A", 'V'},
    {"VIC7_1", 'T'}, {"VIC7_10", 'V'}, {"VIC7_11", 'V'}, {"VIC7_12", 'V'}, {"VIC7_2", 'V'}, {"VIC7_3", 'V'},
    {"VIC7_3A", 'V'}, {"VIC7_3B", 'T'}, {"VIC7_3C", 'V'}, {"VIC7_3D", 'V'}, {"VIC7_3E", 'V'}, {"VIC7_3F", 'T'},
    {"VIC7_3G", 'T'}, {"VIC7_4", 'G'}, {"VIC7_5", 'G'}, {"VIC7_6", 'G'}, {"VIC7_7", 'V'}, {"VIC7_8", 'T'},
    {"VIC7_9", 'V'},
};
struct DualPair { char a; char b; };
static const std::unordered_map<std::string_view, DualPair> kDualMap = {
    {"BIK_06", {'T', 'R'}}, {"BIK_20", {'R', 'T'}}, {"BONE_12", {'J', 'T'}}, {"BONE_14", {'J', 'T'}}, {"CONT_02", {'S', 'R'}},
    {"CONT_04", {'R', 'S'}}, {"CONT_10", {'R', 'S'}}, {"CONT_13", {'R', 'S'}}, {"CONT_31", {'R', 'S'}}, {"CONT_43", {'S', 'R'}},
    {"CRAZ_11", {'L', 'Y'}}, {"DRIV_04", {'T', 'S'}}, {"FIN2_05", {'U', 'S'}}, {"FIN2_16", {'S', 'T'}}, {"HOTW_07", {'V', 'X'}},
    {"HOTW_09", {'V', 'T'}}, {"HOTW_13", {'V', 'T'}}, {"KIDN_04", {'J', 'T'}}, {"KIDN_11", {'J', 'T'}}, {"LAND_14", {'D', 'T'}},
    {"LAND_16", {'T', 'D'}}, {"MORG_06", {'D', 'T'}}, {"OVER_15", {'R', 'T'}}, {"RUFJ_20", {'S', 'P'}}, {"SICI_02", {'S', 'T'}},
    {"THEO_24", {'S', 'T'}},
};
char fallback_code(const std::string& key, const std::string& tbl, const std::string& txt) {
    auto sw = [&](const char* p) { return key.rfind(p, 0) == 0 || tbl.rfind(p, 0) == 0; };
    if (sw("VIC") || sw("HOMS") || sw("HOTW")) return 'V';
    if (sw("SALH") || sw("SALS") || sw("SAL") || sw("THEO") || sw("CONT") || sw("DRIV") || sw("SS1") || sw("CAD4") || sw("CAD5") || sw("MPRCS") || sw("MPTCS") || sw("MPWCS") || sw("MPHP") || sw("FIN") || sw("SICI")) return 'S';
    if (sw("JDT") || sw("BONE") || sw("KIDN") || key.rfind("JDREF", 0) == 0) return 'J';
    if (sw("MAC")) return 'M';
    if (sw("MAR") || sw("BIK_") || sw("OVER_")) return 'R';
    if (sw("DONS") || sw("DONH") || sw("MORG") || sw("CAMP") || sw("DS4") || sw("DS5") || sw("LAND") || sw("ELEC")) return 'D';
    if (sw("NEDS")) return 'N';
    if (sw("RAYS")) {
        if (txt.find("Toni") != std::string::npos || txt.find("Cipriani") != std::string::npos) return 'L';
        return 'L';
    }
    if (sw("TOSH") || sw("DEAD") || sw("CASH")) return 'K';
    if (sw("RMN")) return 'B';
    if (key.rfind("ANG_", 0) == 0) return 'G';
    return 0;
}
} // namespace

std::string resolve_dialogue_speaker(const std::string& gxt_key, const std::string& gxt_table, const std::string& clean_text, int ui_language) {
    if (!gxt_key.empty()) {
        if (auto it = kExactMap.find(gxt_key); it != kExactMap.end()) {
            const char* n = code_to_name(it->second, ui_language);
            if (n && *n) return n;
        }
    }
    const char c = fallback_code(gxt_key, gxt_table, clean_text);
    if (c) return code_to_name(c, ui_language);
    if (clean_text.find("Salvatore") != std::string::npos || clean_text.find("Sal ") != std::string::npos) return "Toni Cipriani";
    return {};
}

std::vector<std::pair<std::string, std::string>> split_dialogue_segments(
    const std::string& gxt_key, const std::string& gxt_table, const std::string& text, int ui_language) {
    std::vector<std::pair<std::string, std::string>> out;
    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '-')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        return s;
    };
    if (auto dit = kDualMap.find(gxt_key); dit != kDualMap.end()) {
        const size_t sep = text.find("\n");
        if (sep != std::string::npos) {
            std::string p1 = trim(text.substr(0, sep));
            std::string p2 = trim(text.substr(sep + 1));
            if (!p1.empty()) out.emplace_back(code_to_name(dit->second.a, ui_language), std::move(p1));
            if (!p2.empty()) out.emplace_back(code_to_name(dit->second.b, ui_language), std::move(p2));
            if (!out.empty()) return out;
        }
    }
    std::string single = text;
    std::replace(single.begin(), single.end(), '\n', ' ');
    out.emplace_back(resolve_dialogue_speaker(gxt_key, gxt_table, single, ui_language), std::move(single));
    return out;
}
} // namespace lcs::game
