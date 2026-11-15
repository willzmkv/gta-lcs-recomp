#!/usr/bin/env bash
# Prepare private files from a legally owned GTA:LCS 2.4.379 Android install.
# Source archives are never modified or deleted.

set -u

GAMEDIR=${1:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)}
case "$GAMEDIR" in ''|/) echo "LCS setup: refusing unsafe game directory" >&2; exit 2 ;; esac

GAMEDATA="$GAMEDIR/gamedata"
OUTPUT="$GAMEDIR/gamefiles"
STAGE="$GAMEDIR/.setup"
MARKER="$OUTPUT/.lcs-2.4.379-ready"

EXPECTED_GAME_SHA256=d581f68eaa8c6831c3796e241e11deea2be32f442b4e587f85dc34bd2cd856ab
EXPECTED_OPENAL_SHA256=2cb52c89b85e8ba1e397068e43fdc80ec5694e06600522c74e9fb51aaf598583
EXPECTED_MPG123_SHA256=92d407574a05e30c34644c6fe649b20345f1e6833608733e699f79d5865ad80a
EXPECTED_MAIN_SHA256=e9a61648b6f28427a95f3b3038051c01469c8bcf4cecfcd91321e74ca1d81220

mkdir -p "$GAMEDATA" "$OUTPUT/android-libs" "$OUTPUT/assets" || exit 1

for tool in awk cp find mkdir mv rm tr unzip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "LCS setup: missing required tool: $tool" >&2
        exit 1
    }
done

if command -v sha256sum >/dev/null 2>&1; then
    hash_file() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum >/dev/null 2>&1; then
    hash_file() { shasum -a 256 "$1" | awk '{print $1}'; }
else
    echo "LCS setup: sha256sum or shasum is required" >&2
    exit 1
fi

install_direct_music() {
    local candidate music_tmp="$OUTPUT/assets/.data_music.$$"
    [ ! -s "$OUTPUT/assets/data_music.wad" ] || return 0
    for candidate in \
        "$GAMEDATA/data_music.wad" \
        "$GAMEDATA/data_music/data_music.wad" \
        "$GAMEDATA/data_music/assets/data_music.wad"; do
        [ -s "$candidate" ] || continue
        echo "LCS setup: installing user-supplied data_music.wad"
        rm -f -- "$music_tmp"
        cp -p "$candidate" "$music_tmp" || {
            rm -f -- "$music_tmp"
            return 1
        }
        mv -f -- "$music_tmp" "$OUTPUT/assets/data_music.wad" || {
            rm -f -- "$music_tmp"
            return 1
        }
        return 0
    done
    return 0
}

install_direct_music || exit 1

data_files_present() {
    [ -s "$OUTPUT/android-libs/libGame.so" ] &&
        [ -s "$OUTPUT/android-libs/libopenal.so" ] &&
        [ -s "$OUTPUT/android-libs/libVendor_mpg123.so" ] &&
        [ -s "$OUTPUT/assets/data_main.wad" ]
}

marker_has_line() {
    local expected=$1 line
    [ -f "$MARKER" ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        [ "$line" = "$expected" ] && return 0
    done <"$MARKER"
    return 1
}

marker_has_prefix() {
    local expected=$1 line
    [ -f "$MARKER" ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in "$expected"*) return 0 ;; esac
    done <"$MARKER"
    return 1
}

marker_music_is_current() {
    if [ -s "$OUTPUT/assets/data_music.wad" ]; then
        marker_has_prefix 'data_music.sha256='
    else
        ! marker_has_prefix 'data_music.sha256='
    fi
}

marker_is_current() {
    marker_has_line 'package=com.rockstargames.gtalcs' &&
        marker_has_line 'version=2.4.379' &&
        marker_has_line "libGame.sha256=$EXPECTED_GAME_SHA256" &&
        marker_has_line "libopenal.sha256=$EXPECTED_OPENAL_SHA256" &&
        marker_has_line "libVendor_mpg123.sha256=$EXPECTED_MPG123_SHA256" &&
        marker_has_line "data_main.sha256=$EXPECTED_MAIN_SHA256" &&
        marker_music_is_current
}

if data_files_present && marker_is_current; then
    exit 0
fi

validate_build() {
    local library=$1 openal=$2 mpg123=$3 data_main=$4
    local game_hash openal_hash mpg123_hash main_hash
    echo "LCS setup: verifying libGame.so"
    game_hash=$(hash_file "$library") || return 1
    echo "LCS setup: verifying libopenal.so"
    openal_hash=$(hash_file "$openal") || return 1
    echo "LCS setup: verifying libVendor_mpg123.so"
    mpg123_hash=$(hash_file "$mpg123") || return 1
    echo "LCS setup: verifying data_main.wad (this can take a minute)"
    main_hash=$(hash_file "$data_main") || return 1
    if [ "$game_hash" != "$EXPECTED_GAME_SHA256" ] ||
       [ "$openal_hash" != "$EXPECTED_OPENAL_SHA256" ] ||
       [ "$mpg123_hash" != "$EXPECTED_MPG123_SHA256" ] ||
       [ "$main_hash" != "$EXPECTED_MAIN_SHA256" ]; then
        echo "LCS setup: unsupported or mismatched Android files" >&2
        echo "  libGame.so:          $game_hash" >&2
        echo "  libopenal.so:        $openal_hash" >&2
        echo "  libVendor_mpg123.so: $mpg123_hash" >&2
        echo "  data_main.wad: $main_hash" >&2
        echo "Expected the matching LCS 2.4.379 ARM64 files." >&2
        if [ "${LCS_ALLOW_UNTESTED:-0}" != 1 ]; then
            return 1
        fi
        echo "LCS setup: continuing because LCS_ALLOW_UNTESTED=1" >&2
    fi
    VALID_GAME_HASH=$game_hash
    VALID_OPENAL_HASH=$openal_hash
    VALID_MPG123_HASH=$mpg123_hash
    VALID_MAIN_HASH=$main_hash
    return 0
}

write_ready_marker() {
    local marker_tmp="$OUTPUT/.lcs-ready.$$"
    rm -f -- "$marker_tmp"
    {
        echo "package=com.rockstargames.gtalcs"
        echo "version=2.4.379"
        echo "libGame.sha256=$VALID_GAME_HASH"
        echo "libopenal.sha256=$VALID_OPENAL_HASH"
        echo "libVendor_mpg123.sha256=$VALID_MPG123_HASH"
        echo "data_main.sha256=$VALID_MAIN_HASH"
        if [ -s "$OUTPUT/assets/data_music.wad" ]; then
            echo "data_music.sha256=$(hash_file "$OUTPUT/assets/data_music.wad")"
        fi
    } >"$marker_tmp" || {
        rm -f -- "$marker_tmp"
        return 1
    }
    mv -f -- "$marker_tmp" "$MARKER"
}

# Adopt files prepared with the desktop extractor without copying them again.
if data_files_present; then
    validate_build "$OUTPUT/android-libs/libGame.so" \
        "$OUTPUT/android-libs/libopenal.so" \
        "$OUTPUT/android-libs/libVendor_mpg123.so" \
        "$OUTPUT/assets/data_main.wad" || exit 1
    write_ready_marker || exit 1
    echo "LCS setup: existing private game files are ready"
    exit 0
fi

rm -rf -- "$STAGE"
mkdir -p "$STAGE/splits" "$STAGE/android-libs" "$STAGE/assets" || exit 1
cleanup_stage() { rm -rf -- "$STAGE"; }
trap cleanup_stage EXIT
trap 'exit 130' INT TERM HUP

sources=()
add_source() {
    local file=$1 lower
    [ -f "$file" ] || return 0
    lower=$(printf '%s' "$file" | tr '[:upper:]' '[:lower:]')
    case "$lower" in
        *.apk|*.apks|*.apkm|*.xapk|*.zip) sources+=("$file") ;;
    esac
}

while IFS= read -r -d '' file; do add_source "$file"; done < <(
    find "$GAMEDATA" -maxdepth 1 -type f \( \
        -iname '*.apk' -o -iname '*.apks' -o -iname '*.apkm' -o \
        -iname '*.xapk' -o -iname '*.zip' \) -print0
)
while IFS= read -r -d '' file; do add_source "$file"; done < <(
    find "$GAMEDIR" -maxdepth 1 -type f \( \
        -iname '*.apk' -o -iname '*.apks' -o -iname '*.apkm' -o \
        -iname '*.xapk' -o -iname '*.zip' \) -print0
)

[ ${#sources[@]} -gt 0 ] || {
    echo "LCS setup: no Android APK or split bundle found in $GAMEDATA" >&2
    exit 1
}

# Expand APKs nested in APKS/APKM/XAPK/ZIP bundles into the private staging
# directory. Individual split APK files are also accepted directly.
original_sources=("${sources[@]}")
split_number=0
for bundle in "${original_sources[@]}"; do
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        split_number=$((split_number + 1))
        split="$STAGE/splits/${split_number}-${entry##*/}"
        if unzip -p "$bundle" "$entry" >"$split" 2>/dev/null && [ -s "$split" ]; then
            sources+=("$split")
        else
            rm -f -- "$split"
        fi
    done < <(unzip -Z1 "$bundle" 2>/dev/null | awk 'tolower($0) ~ /[.]apk$/')
done

extract_first() {
    local destination=$1
    shift
    local source entry
    [ ! -s "$destination" ] || return 0
    for source in "${sources[@]}"; do
        for entry in "$@"; do
            if unzip -p "$source" "$entry" >"$destination" 2>/dev/null && [ -s "$destination" ]; then
                echo "LCS setup: extracted ${destination#"$STAGE/"}"
                return 0
            fi
            rm -f -- "$destination"
        done
    done
    return 1
}

if [ -s "$OUTPUT/android-libs/libGame.so" ]; then
    LIBGAME_CANDIDATE="$OUTPUT/android-libs/libGame.so"
else
    extract_first "$STAGE/android-libs/libGame.so" 'lib/arm64-v8a/libGame.so' || {
        echo "LCS setup: ARM64 libGame.so was not found; copy the arm64-v8a split too" >&2
        exit 1
    }
    LIBGAME_CANDIDATE="$STAGE/android-libs/libGame.so"
fi

for library in libVendor_mpg123.so libopenal.so libz.so; do
    [ -s "$OUTPUT/android-libs/$library" ] || \
        extract_first "$STAGE/android-libs/$library" "lib/arm64-v8a/$library" || true
done

if [ -s "$OUTPUT/android-libs/libopenal.so" ]; then
    LIBOPENAL_CANDIDATE="$OUTPUT/android-libs/libopenal.so"
elif [ -s "$STAGE/android-libs/libopenal.so" ]; then
    LIBOPENAL_CANDIDATE="$STAGE/android-libs/libopenal.so"
else
    echo "LCS setup: ARM64 libopenal.so was not found" >&2
    exit 1
fi
if [ -s "$OUTPUT/android-libs/libVendor_mpg123.so" ]; then
    LIBMPG123_CANDIDATE="$OUTPUT/android-libs/libVendor_mpg123.so"
elif [ -s "$STAGE/android-libs/libVendor_mpg123.so" ]; then
    LIBMPG123_CANDIDATE="$STAGE/android-libs/libVendor_mpg123.so"
else
    echo "LCS setup: ARM64 libVendor_mpg123.so was not found" >&2
    exit 1
fi

if [ -s "$OUTPUT/assets/data_main.wad" ]; then
    DATA_MAIN_CANDIDATE="$OUTPUT/assets/data_main.wad"
else
    extract_first "$STAGE/assets/data_main.wad" 'assets/data_main.wad' 'data_main.wad' || {
        echo "LCS setup: data_main.wad was not found; copy the data_main asset split too" >&2
        exit 1
    }
    DATA_MAIN_CANDIDATE="$STAGE/assets/data_main.wad"
fi

# A future complete split bundle may contain the fast-follow pack even though
# the currently tested Play installation keeps it in protected app storage.
# Preserve an existing recovered pack; otherwise normalize one from the APKs.
if [ ! -s "$OUTPUT/assets/data_music.wad" ]; then
    extract_first "$STAGE/assets/data_music.wad" \
        'assets/data_music.wad' 'data_music.wad' || true
fi

validate_build "$LIBGAME_CANDIDATE" "$LIBOPENAL_CANDIDATE" \
    "$LIBMPG123_CANDIDATE" "$DATA_MAIN_CANDIDATE" || exit 1

# Invalidate readiness before publishing any staged file. If power is lost
# between the per-file atomic renames, the next launch must validate again.
rm -f -- "$MARKER"
mkdir -p "$OUTPUT/android-libs" "$OUTPUT/assets"
for file in "$STAGE/android-libs"/*; do
    if [ -f "$file" ]; then
        mv -f -- "$file" "$OUTPUT/android-libs/" || exit 1
    fi
done
if [ -s "$STAGE/assets/data_main.wad" ]; then
    mv -f -- "$STAGE/assets/data_main.wad" \
        "$OUTPUT/assets/data_main.wad" || exit 1
fi
if [ -s "$STAGE/assets/data_music.wad" ]; then
    mv -f -- "$STAGE/assets/data_music.wad" \
        "$OUTPUT/assets/data_music.wad" || exit 1
fi
data_files_present || {
    echo "LCS setup: install did not publish every required private file" >&2
    exit 1
}
write_ready_marker || exit 1

echo "LCS setup: private game files are ready"
if [ ! -s "$OUTPUT/assets/data_music.wad" ]; then
    echo "LCS setup: data_music is absent; bring-up stages can continue without it"
fi
exit 0
