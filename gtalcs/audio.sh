#!/usr/bin/env bash
# Copy data_music.wad from the owner's rooted GTA:LCS Android installation.

set -euo pipefail

PACKAGE=com.rockstargames.gtalcs
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
OUTPUT_DIR=${1:-"$SCRIPT_DIR/gamedata"}
ADB=${ADB:-adb}

echo "This copies music only from your own legally purchased GTA:LCS install."
if [ "${LCS_CONFIRM_OWNERSHIP:-}" != yes ]; then
    printf "Continue? [y/N] "
    read -r answer
    case "$answer" in y|Y|yes|YES) ;; *) exit 2 ;; esac
fi

command -v "$ADB" >/dev/null 2>&1 || {
    echo "adb was not found. Install Android platform tools first." >&2
    exit 1
}
"$ADB" get-state >/dev/null 2>&1 || {
    echo "Connect and authorize the rooted Android phone first." >&2
    exit 1
}
"$ADB" shell pm path "$PACKAGE" 2>/dev/null | grep -q '^package:' || {
    echo "GTA:LCS is not installed on the connected phone." >&2
    exit 1
}
[ "$("$ADB" shell su -c 'id -u' 2>/dev/null | tr -d '\r' | tail -1)" = 0 ] || {
    echo "Root permission was not granted on the phone." >&2
    exit 1
}

remote_paths=()
for root in \
    "/data/user/0/$PACKAGE" \
    "/data/data/$PACKAGE" \
    "/data/user_de/0/$PACKAGE" \
    "/data/misc/assetpacks/$PACKAGE" \
    "/data/misc_ce/0/assetpacks/$PACKAGE"; do
    while IFS= read -r candidate; do
        candidate=${candidate%$'\r'}
        [ -n "$candidate" ] || continue
        duplicate=0
        for existing in "${remote_paths[@]}"; do
            [ "$existing" != "$candidate" ] || duplicate=1
        done
        [ "$duplicate" -ne 0 ] || remote_paths+=("$candidate")
    done < <(
        "$ADB" shell su -c \
            "find '$root' -type f -name data_music.wad -print 2>/dev/null" \
            2>/dev/null || true
    )
done

if [ "${#remote_paths[@]}" -eq 0 ]; then
    echo "data_music.wad was not found. Open the Android game once while online." >&2
    exit 1
fi
if [ "${#remote_paths[@]}" -gt 1 ]; then
    echo "Multiple music files were found; remove old GTA:LCS installations." >&2
    printf '  %s\n' "${remote_paths[@]}" >&2
    exit 1
fi

REMOTE_PATH=${remote_paths[0]}
case "$REMOTE_PATH" in
    /data/*/data_music.wad) ;;
    *) echo "Refusing unexpected Android path." >&2; exit 2 ;;
esac
case "$REMOTE_PATH" in
    *"'"*) echo "Refusing unsafe Android path." >&2; exit 2 ;;
esac
case "$OUTPUT_DIR" in
    ''|/) echo "Refusing unsafe output directory." >&2; exit 2 ;;
esac

mkdir -p "$OUTPUT_DIR"
TEMP="$OUTPUT_DIR/.data_music.wad.$$"
DESTINATION="$OUTPUT_DIR/data_music.wad"
trap 'rm -f -- "$TEMP"' EXIT
"$ADB" exec-out su -c "cat '$REMOTE_PATH'" >"$TEMP"
[ -s "$TEMP" ] || {
    echo "The extracted music file is empty." >&2
    exit 1
}
chmod 600 "$TEMP"
mv -f -- "$TEMP" "$DESTINATION"
trap - EXIT

echo "Music ready: $DESTINATION"
echo "Copy the gtalcs folder to the SD card if needed, then launch the port."
echo "Do not upload or redistribute data_music.wad."
