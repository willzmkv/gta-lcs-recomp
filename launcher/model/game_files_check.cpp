#include "game_files_check.h"

#include "system.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        std::string toLower(std::string s)
        {
            for (char &c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        uint16_t readU16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
        uint32_t readU32(const uint8_t *p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        // Lightweight ZIP central-directory scanner (no zlib needed, reads only entry filenames).
        bool listZipEntries(const fs::path &zipPath, std::vector<std::string> &outNames)
        {
            outNames.clear();
            std::ifstream f(zipPath, std::ios::binary | std::ios::ate);
            if (!f)
                return false;
            const std::streamoff size = f.tellg();
            if (size < 22)
                return false;

            const size_t tailSize = static_cast<size_t>(std::min<std::streamoff>(size, 65557));
            std::vector<uint8_t> tail(tailSize);
            f.seekg(size - static_cast<std::streamoff>(tailSize));
            if (!f.read(reinterpret_cast<char *>(tail.data()), static_cast<std::streamsize>(tailSize)))
                return false;

            // Locate End of Central Directory (PK\x05\x06)
            size_t eocd = std::string::npos;
            for (size_t i = tailSize - 22;; --i)
            {
                if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 0x05 && tail[i + 3] == 0x06)
                {
                    eocd = i;
                    break;
                }
                if (i == 0)
                    break;
            }
            if (eocd == std::string::npos)
                return false;

            const uint32_t cdSize = readU32(&tail[eocd + 12]);
            const uint32_t cdOffset = readU32(&tail[eocd + 16]);
            if (cdSize == 0 || static_cast<std::streamoff>(cdOffset) + static_cast<std::streamoff>(cdSize) > size)
                return false;
            if (cdSize > 64 * 1024 * 1024)
                return false;

            std::vector<uint8_t> cd(cdSize);
            f.seekg(static_cast<std::streamoff>(cdOffset));
            if (!f.read(reinterpret_cast<char *>(cd.data()), static_cast<std::streamsize>(cdSize)))
                return false;

            size_t pos = 0;
            while (pos + 46 <= cd.size())
            {
                if (cd[pos] != 0x50 || cd[pos + 1] != 0x4B || cd[pos + 2] != 0x01 || cd[pos + 3] != 0x02)
                    break;
                const uint16_t nameLen = readU16(&cd[pos + 28]);
                const uint16_t extraLen = readU16(&cd[pos + 30]);
                const uint16_t commentLen = readU16(&cd[pos + 32]);
                if (pos + 46 + nameLen > cd.size())
                    break;
                outNames.emplace_back(reinterpret_cast<const char *>(&cd[pos + 46]), nameLen);
                pos += 46 + nameLen + extraLen + commentLen;
            }
            return !outNames.empty();
        }

        bool validateElfArm64(const fs::path &elfPath, std::string &codeOut, std::string &msgOut)
        {
            std::error_code ec;
            if (!fs::is_regular_file(elfPath, ec))
            {
                codeOut = "not_found";
                msgOut = "libGame.so not found.";
                return false;
            }
            std::ifstream f(elfPath, std::ios::binary);
            uint8_t hdr[20] = {};
            if (!f.read(reinterpret_cast<char *>(hdr), sizeof(hdr)))
            {
                codeOut = "invalid_elf";
                msgOut = "Could not read ELF header from libGame.so.";
                return false;
            }
            if (hdr[0] != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F')
            {
                codeOut = "invalid_elf";
                msgOut = "File is not a valid ELF binary.";
                return false;
            }
            if (hdr[4] != 2)
            {
                codeOut = "wrong_arch";
                msgOut = "libGame.so is 32-bit (ARMv7); 64-bit ARM64 (arm64-v8a) is required.";
                return false;
            }
            const uint16_t eMachine = readU16(&hdr[18]);
            if (eMachine != 183) // EM_AARCH64
            {
                codeOut = "wrong_arch";
                msgOut = "libGame.so architecture is not ARM64 (EM_AARCH64=183).";
                return false;
            }
            codeOut = "ok";
            msgOut = "Android ARM64 (libGame.so + data_main.wad) verified";
            return true;
        }
    } // namespace

    AndroidSourceResult checkAndroidSource(const fs::path &root, const fs::path &picked)
    {
        AndroidSourceResult r;
        std::error_code ec;

        const fs::path defaultLib = root / "gtalcs" / "gamefiles" / "android-libs" / "libGame.so";
        const fs::path defaultMain = root / "gtalcs" / "gamefiles" / "assets" / "data_main.wad";
        const fs::path defaultMusic = root / "gtalcs" / "gamefiles" / "assets" / "data_music.wad";

        if (fs::is_regular_file(defaultLib, ec))
            r.libGamePath = defaultLib;
        if (fs::is_regular_file(defaultMain, ec))
            r.dataMainPath = defaultMain;
        if (fs::is_regular_file(defaultMusic, ec))
            r.dataMusicPath = defaultMusic;

        if (!picked.empty())
        {
            if (!fs::exists(picked, ec))
            {
                r.code = "not_found";
                r.message = "Selected Android file does not exist.";
                return r;
            }
            const std::string ext = toLower(pathText(picked.extension()));
            if (ext == ".so")
            {
                std::string c, m;
                if (!validateElfArm64(picked, c, m))
                {
                    r.code = c;
                    r.message = m;
                    return r;
                }
                r.libGamePath = picked;
            }
            else if (ext == ".wad")
            {
                const std::string name = toLower(pathText(picked.filename()));
                if (name.find("music") != std::string::npos)
                    r.dataMusicPath = picked;
                else
                    r.dataMainPath = picked;
            }
            else if (ext == ".apk" || ext == ".obb" || ext == ".zip")
            {
                std::vector<std::string> entries;
                if (!listZipEntries(picked, entries))
                {
                    r.code = "not_android";
                    r.message = "Selected file is not a valid APK/OBB/ZIP archive.";
                    return r;
                }
                bool hasLib = false, hasMain = false, hasMusic = false;
                for (const std::string &e : entries)
                {
                    const std::string low = toLower(e);
                    if (low.find("arm64-v8a/libgame.so") != std::string::npos || low == "libgame.so")
                        hasLib = true;
                    if (low.find("data_main.wad") != std::string::npos)
                        hasMain = true;
                    if (low.find("data_music.wad") != std::string::npos)
                        hasMusic = true;
                }
                if (!hasLib && !hasMain && !hasMusic)
                {
                    r.code = "not_android";
                    r.message = "Archive does not contain libGame.so (arm64-v8a) or data_main.wad.";
                    return r;
                }
                r.archivePath = picked;
                if (hasLib && r.libGamePath.empty())
                    r.needsExtract = true;
                if (hasMain && r.dataMainPath.empty())
                    r.needsExtract = true;
                if (hasMusic && r.dataMusicPath.empty())
                    r.needsExtract = true;
                if ((hasLib || !r.libGamePath.empty()) && (hasMain || !r.dataMainPath.empty()))
                {
                    r.ok = true;
                    r.code = r.needsExtract ? "archive_ready" : "ok";
                    r.message = r.needsExtract
                                    ? "Android APK/OBB verified (ready to extract)"
                                    : "Android ARM64 (libGame.so + data_main.wad) verified";
                    return r;
                }
            }
            else
            {
                r.code = "not_android";
                r.message = "Expected an Android .apk, .obb, .zip, libGame.so, or .wad file.";
                return r;
            }
        }

        // Validate extracted state
        if (r.libGamePath.empty())
        {
            r.code = "not_found";
            r.message = "Missing Android ARM64 binary (libGame.so). Select your GTA: LCS Android .apk or libGame.so.";
            return r;
        }
        std::string elfCode, elfMsg;
        if (!validateElfArm64(r.libGamePath, elfCode, elfMsg))
        {
            r.code = elfCode;
            r.message = elfMsg;
            return r;
        }
        if (r.dataMainPath.empty())
        {
            r.code = "missing_wad";
            r.message = "libGame.so verified, but data_main.wad is missing. Select the Android .obb or data_main.wad.";
            return r;
        }

        r.ok = true;
        r.code = "ok";
        r.message = r.dataMusicPath.empty()
                        ? "Android ARM64 verified (libGame.so + data_main.wad)"
                        : "Android ARM64 verified (libGame.so + data_main.wad + data_music.wad)";
        return r;
    }

    IpadSourceResult checkIpadSource(const fs::path &root, const fs::path &picked)
    {
        IpadSourceResult r;
        std::error_code ec;

        const fs::path ipadDir = root / "assets" / "ipad";
        if (fs::is_directory(ipadDir, ec))
        {
            int glslCount = 0;
            for (const auto &entry : fs::directory_iterator(ipadDir, ec))
            {
                if (entry.is_regular_file(ec) && toLower(pathText(entry.path().extension())) == ".glsl")
                    ++glslCount;
            }
            if (fs::is_regular_file(ipadDir / "WorldPs.glsl", ec) && glslCount >= 10)
            {
                r.extractedDir = ipadDir;
                r.shaderCount = glslCount;
                r.hasTimecyc = fs::is_regular_file(ipadDir / "Data" / "TIMECYC.DAT", ec) ||
                               fs::is_regular_file(ipadDir / "TIMECYC.DAT", ec);
            }
        }

        fs::path ipaToCheck = picked;
        if (ipaToCheck.empty() && fs::is_directory(root, ec))
        {
            for (const auto &entry : fs::directory_iterator(root, ec))
            {
                if (entry.is_regular_file(ec) && toLower(pathText(entry.path().extension())) == ".ipa")
                {
                    ipaToCheck = entry.path();
                    break;
                }
            }
        }

        if (!ipaToCheck.empty())
        {
            if (!fs::is_regular_file(ipaToCheck, ec))
            {
                r.code = "not_found";
                r.message = "Selected iPad .ipa file was not found.";
                return r;
            }
            std::vector<std::string> entries;
            if (!listZipEntries(ipaToCheck, entries))
            {
                r.code = "not_ipa";
                r.message = "File is not a valid iPad .ipa (ZIP) archive.";
                return r;
            }
            bool hasWorldPs = false;
            bool hasTimecyc = false;
            int glslInZip = 0;
            for (const std::string &e : entries)
            {
                const std::string low = toLower(e);
                if (low.find("worldps.glsl") != std::string::npos)
                    hasWorldPs = true;
                if (low.find("timecyc.dat") != std::string::npos)
                    hasTimecyc = true;
                if (low.size() > 5 && low.compare(low.size() - 5, 5, ".glsl") == 0)
                    ++glslInZip;
            }
            if (!hasWorldPs)
            {
                r.code = "missing_assets";
                r.message = "IPA is missing Payload/GTALcs.app/WorldPs.glsl (not GTA: Liberty City Stories iPad).";
                return r;
            }
            r.ipaPath = ipaToCheck;
            r.shaderCount = std::max(r.shaderCount, glslInZip);
            r.hasTimecyc = r.hasTimecyc || hasTimecyc;
            r.needsExtract = r.extractedDir.empty();
            r.ok = true;
            r.code = "ok";
            r.message = "iPad HD IPA verified (" + std::to_string(r.shaderCount) + " shaders" +
                        (r.hasTimecyc ? " + TIMECYC.DAT)" : ")");
            return r;
        }

        if (!r.extractedDir.empty())
        {
            r.ok = true;
            r.code = "ok";
            r.message = "iPad HD assets verified (" + std::to_string(r.shaderCount) + " shaders in assets/ipad)";
            return r;
        }

        r.code = "not_found";
        r.message = "Missing iPad HD assets (GTALcs.ipa). Select or drop GTALcs.ipa.";
        return r;
    }

    SourceKind classifyDroppedFile(const fs::path &path)
    {
        const std::string ext = toLower(pathText(path.extension()));
        if (ext == ".ipa")
            return SourceKind::Ipad;
        if (ext == ".apk" || ext == ".obb" || ext == ".so" || ext == ".wad")
            return SourceKind::Android;
        if (ext == ".zip")
        {
            std::vector<std::string> entries;
            if (listZipEntries(path, entries))
            {
                for (const std::string &e : entries)
                {
                    const std::string low = toLower(e);
                    if (low.find("payload/") != std::string::npos && low.find("worldps.glsl") != std::string::npos)
                        return SourceKind::Ipad;
                    if (low.find("libgame.so") != std::string::npos || low.find("data_main.wad") != std::string::npos)
                        return SourceKind::Android;
                }
            }
        }
        return SourceKind::Unknown;
    }

    namespace
    {
        std::vector<fs::path> scanCandidates(const fs::path &root, const std::vector<std::string> &exts)
        {
            std::vector<fs::path> out;
            std::vector<fs::path> dirs = {
                root,
                root / "gtalcs" / "gamedata",
                knownFolder(Folder::Downloads),
                knownFolder(Folder::Desktop),
                knownFolder(Folder::Documents),
            };
            std::error_code ec;
            for (const fs::path &d : dirs)
            {
                if (d.empty() || !fs::is_directory(d, ec))
                    continue;
                for (const auto &entry : fs::directory_iterator(d, ec))
                {
                    if (!entry.is_regular_file(ec))
                        continue;
                    const std::string ext = toLower(pathText(entry.path().extension()));
                    if (std::find(exts.begin(), exts.end(), ext) != exts.end())
                        out.push_back(entry.path());
                }
            }
            return out;
        }
    } // namespace

    std::vector<fs::path> androidCandidates(const fs::path &root)
    {
        return scanCandidates(root, {".apk", ".obb", ".so", ".wad"});
    }

    std::vector<fs::path> ipadCandidates(const fs::path &root)
    {
        return scanCandidates(root, {".ipa"});
    }
}
