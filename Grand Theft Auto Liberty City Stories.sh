#!/usr/bin/env bash
# PortMaster entry point for the GTA: Liberty City Stories compatibility port.

PORT_TITLE="Grand Theft Auto: Liberty City Stories"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
XDG_DATA_HOME=${XDG_DATA_HOME:-${HOME:-/tmp}/.local/share}

# Load PortMaster's device/control contract when it is available. The local
# fallbacks keep diagnostic stages usable from a plain console as well.
controlfolder=
for candidate in \
    /opt/system/Tools/PortMaster \
    /opt/tools/PortMaster \
    "$XDG_DATA_HOME/PortMaster" \
    "$SCRIPT_DIR/PortMaster" \
    /roms/ports/PortMaster; do
    if [ -f "$candidate/control.txt" ]; then
        controlfolder=$candidate
        # shellcheck disable=SC1090
        source "$controlfolder/control.txt"
        if [ -n "${CFW_NAME:-}" ] && [ -f "$controlfolder/mod_${CFW_NAME}.txt" ]; then
            # shellcheck disable=SC1090
            source "$controlfolder/mod_${CFW_NAME}.txt"
        fi
        if declare -F get_controls >/dev/null 2>&1; then
            get_controls
        fi
        break
    fi
done

ESUDO=${ESUDO:-}
sdl_controllerconfig=${sdl_controllerconfig:-}

resolve_game_dir() {
    local candidate
    for candidate in \
        "$SCRIPT_DIR/gtalcs" \
        "${directory:+/${directory#/}/ports/gtalcs}" \
        /roms/ports/gtalcs \
        /sdcard/ports/gtalcs \
        /mnt/mmc/ports/gtalcs; do
        [ -n "$candidate" ] || continue
        if [ -d "$candidate" ]; then
            CDPATH= cd -- "$candidate" && pwd -P
            return 0
        fi
    done
    return 1
}

if ! GAMEDIR=$(resolve_game_dir); then
    echo "$PORT_TITLE: the gtalcs data directory could not be found." >&2
    exit 1
fi
if ! CDPATH= cd -- "$GAMEDIR"; then
    echo "$PORT_TITLE: cannot enter the game directory: $GAMEDIR" >&2
    exit 1
fi

if ! mkdir -p "$GAMEDIR/logs" "$GAMEDIR/userdata" 2>/dev/null; then
    echo "$PORT_TITLE: cannot create persistent logs/userdata in $GAMEDIR." >&2
    exit 1
fi
LOG="$GAMEDIR/logs/launcher.log"
if [ -f "$LOG" ]; then
    mv -f -- "$LOG" "$LOG.1" 2>/dev/null || true
fi
if command -v tee >/dev/null 2>&1; then
    exec > >(tee -a "$LOG") 2>&1
else
    exec >>"$LOG" 2>&1
fi

find_tty() {
    local tty_path
    for tty_path in /dev/tty1 /dev/tty0 /dev/console; do
        [ -e "$tty_path" ] || continue
        printf '%s\n' "$tty_path"
        return 0
    done
    return 1
}

CURR_TTY=$(find_tty 2>/dev/null || true)

run_privileged() {
    if [ -n "$ESUDO" ]; then
        # ESUDO is supplied by PortMaster and may itself include arguments.
        # shellcheck disable=SC2086
        $ESUDO "$@"
    else
        "$@"
    fi
}

tty_write() {
    if [ -n "$CURR_TTY" ]; then
        if ! printf '%s\n' "$*" >"$CURR_TTY" 2>/dev/null; then
            printf '%s\n' "$*" | run_privileged tee "$CURR_TTY" \
                >/dev/null 2>&1 || printf '%s\n' "$*"
        fi
    else
        printf '%s\n' "$*"
    fi
}

notify_failure() {
    local message=$1
    echo "ERROR: $message"
    tty_write "$PORT_TITLE"
    tty_write "$message"
    if declare -F pm_message >/dev/null 2>&1; then
        pm_message "$message" 2>/dev/null || true
    fi
}

CONFIG="$GAMEDIR/lcs.conf"
DEFAULT_CONFIG="$GAMEDIR/lcs.conf.default"
if [ ! -f "$CONFIG" ] && [ -f "$DEFAULT_CONFIG" ]; then
    cp -- "$DEFAULT_CONFIG" "$CONFIG" 2>/dev/null || true
fi

CONFIG_STAGE=game
UNBIND_VT=auto
CONSOLE_WAIT=4
PROOF_MODE=off
PROOF_SECONDS=60

trim_space() {
    local value=$1
    value=${value#"${value%%[![:space:]]*}"}
    value=${value%"${value##*[![:space:]]}"}
    printf '%s' "$value"
}

load_config() {
    local line key value
    [ -f "$CONFIG" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%$'\r'}
        line=${line%%#*}
        [ -n "$(trim_space "$line")" ] || continue
        case "$line" in
            *=*) ;;
            *) echo "Ignoring malformed config line: $line"; continue ;;
        esac
        key=$(trim_space "${line%%=*}")
        value=$(trim_space "${line#*=}")
        case "$key" in
            stage) CONFIG_STAGE=$value ;;
            unbind_vt) UNBIND_VT=$value ;;
            console_wait) CONSOLE_WAIT=$value ;;
            proof_mode) PROOF_MODE=$value ;;
            proof_seconds) PROOF_SECONDS=$value ;;
            *) echo "Ignoring unknown config key: $key" ;;
        esac
    done <"$CONFIG"
}

load_config
STAGE=${LCS_STAGE:-$CONFIG_STAGE}
case "$STAGE" in
    map|relocate|jni|render|game) ;;
    *)
        notify_failure "Invalid stage '$STAGE'. Use map, relocate, jni, render, or game."
        sleep "$CONSOLE_WAIT" 2>/dev/null || true
        exit 2
        ;;
esac
case "$UNBIND_VT" in auto|on|off) ;; *) UNBIND_VT=auto ;; esac
case "$CONSOLE_WAIT" in ''|*[!0-9]*) CONSOLE_WAIT=4 ;; esac
case "$PROOF_MODE" in on|off) ;; *) PROOF_MODE=off ;; esac
case "$PROOF_SECONDS" in
    ''|*[!0-9]*|??????*) PROOF_SECONDS=60 ;;
    *)
        if [ "$PROOF_SECONDS" -lt 1 ] || [ "$PROOF_SECONDS" -gt 86400 ]; then
            PROOF_SECONDS=60
        fi
        ;;
esac

RUNTIME=
for candidate in "$GAMEDIR/lcs_runtime" "$GAMEDIR/runtime/lcs_runtime"; do
    if [ -s "$candidate" ]; then
        RUNTIME=$candidate
        break
    fi
done
if [ -z "$RUNTIME" ]; then
    notify_failure "lcs_runtime is missing. Reinstall the port package."
    sleep "$CONSOLE_WAIT"
    exit 1
fi
chmod +x "$RUNTIME" 2>/dev/null || true

case "$(uname -m 2>/dev/null || true)" in
    aarch64|arm64) ;;
    *)
        notify_failure "This Android build requires an AArch64 Linux device."
        sleep "$CONSOLE_WAIT"
        exit 1
        ;;
esac

SETUP="$GAMEDIR/setup.sh"
if [ ! -x "$SETUP" ]; then
    chmod +x "$SETUP" 2>/dev/null || true
fi
if [ ! -x "$SETUP" ] || ! "$SETUP" "$GAMEDIR"; then
    notify_failure "Legal LCS 2.4.379 game data is incomplete. See gtalcs/gamedata/README.txt."
    sleep "$CONSOLE_WAIT"
    exit 1
fi

GAMEFILES="$GAMEDIR/gamefiles"
LIBGAME="$GAMEFILES/android-libs/libGame.so"
DATA_MAIN="$GAMEFILES/assets/data_main.wad"
if [ ! -s "$LIBGAME" ] || [ ! -s "$DATA_MAIN" ]; then
    notify_failure "setup completed without libGame.so or data_main.wad."
    sleep "$CONSOLE_WAIT"
    exit 1
fi

export LCS_STAGE=$STAGE
export LCS_GAME_DIR=$GAMEFILES
export LCS_DATA_MAIN=$DATA_MAIN
unset LCS_DATA_MUSIC
if [ -s "$GAMEFILES/assets/data_music.wad" ]; then
    export LCS_DATA_MUSIC="$GAMEFILES/assets/data_music.wad"
fi

export XDG_DATA_HOME="$GAMEDIR/userdata/data"
export XDG_CONFIG_HOME="$GAMEDIR/userdata/config"
export XDG_CACHE_HOME="$GAMEDIR/userdata/cache"
export XDG_STATE_HOME="$GAMEDIR/userdata/state"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" \
         "$XDG_STATE_HOME" || {
    notify_failure "The persistent userdata directories could not be created."
    sleep "$CONSOLE_WAIT"
    exit 1
}
# The native API treats this as an Android files-dir prefix and appends names
# to it, so retain the trailing slash. Package updates never replace userdata.
export LCS_PRIVATE_FILES_DIR="$GAMEDIR/userdata/"
case "$STAGE" in
    jni|render) export LCS_SAVE_PROBE=${LCS_SAVE_PROBE:-1} ;;
esac
if [ "$PROOF_MODE" = on ]; then
    export LCS_SAVE_PROBE=${LCS_SAVE_PROBE:-1}
    if [ "$STAGE" = game ]; then
        export LCS_REQUIRE_GAMEPLAY_PROOF=1
        export LCS_LIFECYCLE_PROBE=1
        export LCS_PROOF_SECONDS=$PROOF_SECONDS
    fi
fi

# Only Linux DSOs belong on the host loader's search path. In particular, do
# not add gamefiles/android-libs: those Bionic libraries are manually mapped
# by lcs_runtime and can crash the glibc process if ld.so loads them directly.
LIB_PATH="$GAMEDIR/libs"
[ -n "$controlfolder" ] && LIB_PATH="$LIB_PATH:$controlfolder/libs"
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    export LD_LIBRARY_PATH="$LIB_PATH:$LD_LIBRARY_PATH"
else
    export LD_LIBRARY_PATH="$LIB_PATH"
fi

# The audio bridge requires Linux OpenAL Soft, never the APK's Android
# libopenal.so. Prefer a user/package-provided versioned DSO, then let the
# runtime probe the target system's normal libopenal.so.1 locations.
if [ -z "${LCS_OPENAL_LIBRARY:-}" ]; then
    for candidate in \
        "$GAMEDIR/libs/libopenal.so.1" \
        "${controlfolder:+$controlfolder/libs/libopenal.so.1}"; do
        [ -n "$candidate" ] && [ -s "$candidate" ] || continue
        export LCS_OPENAL_LIBRARY="$candidate"
        break
    done
fi
if [ -f "$GAMEDIR/alsoft.conf" ]; then
    export ALSOFT_CONF="${ALSOFT_CONF:-$GAMEDIR/alsoft.conf}"
fi
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-alsa}
export ALSOFT_DRIVERS=${ALSOFT_DRIVERS:-alsa}
export SDL_VIDEO_ALLOW_SCREENSAVER=0
export SDL_NO_SIGNAL_HANDLERS=1
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}

# Leave an existing firmware/user selection intact.  When no driver is set,
# platform_sdl performs the KMSDRM preference itself, remembers that it forced
# the choice, and retries SDL's automatic selection if KMSDRM initialization
# fails.  Pre-setting kmsdrm here made that safe fallback unreachable.
export SDL_VIDEO_GL_DRIVER=${SDL_VIDEO_GL_DRIVER:-libGLESv2.so}
export SDL_VIDEO_EGL_DRIVER=${SDL_VIDEO_EGL_DRIVER:-libEGL.so}

RUNTIME_PID=
WATCHDOG_PID=
VT_UNBOUND=
FINISHED=0
TIMEOUT_MARKER="$GAMEDIR/logs/.stage-timeout"
case "$STAGE" in
    map) STAGE_TIMEOUT=30 ;;
    relocate) STAGE_TIMEOUT=90 ;;
    jni) STAGE_TIMEOUT=180 ;;
    render) STAGE_TIMEOUT=300 ;;
    game) STAGE_TIMEOUT=0 ;;
esac
if [ "$STAGE" = game ] && [ "$PROOF_MODE" = on ]; then
    # Ordinary play is unbounded. A validation hang must still restore the VT.
    STAGE_TIMEOUT=$((PROOF_SECONDS + 300))
fi

restore_vt() {
    local node
    for node in $VT_UNBOUND; do
        if [ -w "$node" ]; then
            printf '1\n' >"$node" 2>/dev/null || true
        else
            printf '1\n' | run_privileged tee "$node" >/dev/null 2>&1 || true
        fi
    done
    VT_UNBOUND=
}

cleanup() {
    [ "$FINISHED" -eq 0 ] || return 0
    FINISHED=1
    if [ -n "$WATCHDOG_PID" ]; then
        kill "$WATCHDOG_PID" 2>/dev/null || true
        wait "$WATCHDOG_PID" 2>/dev/null || true
        WATCHDOG_PID=
    fi
    if [ -n "$RUNTIME_PID" ] && kill -0 "$RUNTIME_PID" 2>/dev/null; then
        kill -TERM "$RUNTIME_PID" 2>/dev/null || true
        for ignored in 1 2 3 4 5; do
            kill -0 "$RUNTIME_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$RUNTIME_PID" 2>/dev/null || true
        wait "$RUNTIME_PID" 2>/dev/null || true
    fi
    restore_vt
    if [ -n "$CURR_TTY" ]; then
        printf '\033c\033[?25h' >"$CURR_TTY" 2>/dev/null || true
    fi
    if declare -F pm_finish >/dev/null 2>&1; then
        pm_finish 2>/dev/null || true
    fi
}

run_runtime() {
    rm -f -- "$TIMEOUT_MARKER"
    "$@" &
    RUNTIME_PID=$!
    if [ "$STAGE_TIMEOUT" -gt 0 ]; then
        (
            elapsed=0
            while [ "$elapsed" -lt "$STAGE_TIMEOUT" ]; do
                sleep 1
                kill -0 "$RUNTIME_PID" 2>/dev/null || exit 0
                elapsed=$((elapsed + 1))
            done
            if kill -0 "$RUNTIME_PID" 2>/dev/null; then
                echo "WATCHDOG: stage '$STAGE' exceeded ${STAGE_TIMEOUT}s"
                : >"$TIMEOUT_MARKER"
                kill -TERM "$RUNTIME_PID" 2>/dev/null || true
                sleep 3
                kill -KILL "$RUNTIME_PID" 2>/dev/null || true
            fi
        ) &
        WATCHDOG_PID=$!
    fi
    wait "$RUNTIME_PID"
    EXIT_CODE=$?
    RUNTIME_PID=
    if [ -n "$WATCHDOG_PID" ]; then
        kill "$WATCHDOG_PID" 2>/dev/null || true
        wait "$WATCHDOG_PID" 2>/dev/null || true
        WATCHDOG_PID=
    fi
    if [ -f "$TIMEOUT_MARKER" ]; then
        EXIT_CODE=124
        rm -f -- "$TIMEOUT_MARKER"
    fi
}

snapshot_user_files() {
    local destination=$1 file

    : >"$destination" || return 1
    find "$GAMEDIR/userdata" -type f -print 2>/dev/null | \
        LC_ALL=C sort | while IFS= read -r file; do
            if command -v sha256sum >/dev/null 2>&1; then
                sha256sum "$file"
            elif command -v shasum >/dev/null 2>&1; then
                shasum -a 256 "$file"
            else
                ls -ln "$file"
            fi
        done >"$destination"
}

DIAGNOSTIC_SNAPSHOTS=off
if [ "$PROOF_MODE" = on ] ||
   { [ -n "${LCS_DIAGNOSTICS:-}" ] && [ "${LCS_DIAGNOSTICS:-0}" != 0 ]; }; then
    DIAGNOSTIC_SNAPSHOTS=on
fi

trap 'status=$?; cleanup; exit "$status"' EXIT
trap 'exit 130' INT TERM HUP

GRAPHICAL=0
case "$STAGE" in render|game) GRAPHICAL=1 ;; esac
if [ "$GRAPHICAL" -eq 1 ]; then
    if declare -F pm_platform_helper >/dev/null 2>&1; then
        pm_platform_helper "$RUNTIME" 2>/dev/null || true
    fi
    if [ -n "$CURR_TTY" ]; then
        printf '\033c\033[?25l' >"$CURR_TTY" 2>/dev/null || true
    fi
    if [ "$UNBIND_VT" = on ] || [ "$UNBIND_VT" = auto ]; then
        for node in /sys/class/vtconsole/vtcon0/bind /sys/class/vtconsole/vtcon1/bind; do
            [ -e "$node" ] || continue
            [ "$(cat "$node" 2>/dev/null || true)" = 1 ] || continue
            if [ -w "$node" ]; then
                printf '0\n' >"$node" 2>/dev/null || continue
            else
                printf '0\n' | run_privileged tee "$node" >/dev/null 2>&1 || continue
            fi
            VT_UNBOUND="$VT_UNBOUND $node"
        done
    fi
fi

echo "=== $PORT_TITLE ==="
echo "timestamp=$(date -Iseconds 2>/dev/null || date 2>/dev/null || echo unavailable)"
echo "stage=$STAGE"
echo "game_dir=$GAMEFILES"
echo "data_music=${LCS_DATA_MUSIC:-missing (optional during bring-up)}"
echo "private_files=$LCS_PRIVATE_FILES_DIR"
echo "openal_host=${LCS_OPENAL_LIBRARY:-system libopenal.so.1 search}"
echo "openal_config=${ALSOFT_CONF:-system default}"
echo "runtime=$RUNTIME"
echo "stage_timeout=${STAGE_TIMEOUT}s (0 means disabled)"
echo "proof_mode=$PROOF_MODE"
echo "proof_seconds=$PROOF_SECONDS"
echo

if [ "$DIAGNOSTIC_SNAPSHOTS" = on ]; then
    snapshot_user_files "$GAMEDIR/logs/userdata-before.txt" || true
fi
run_runtime "$RUNTIME" --stage "$STAGE" --game-dir "$GAMEFILES" "$LIBGAME"

# Stage-one binaries built before the staged CLI accept only libGame.so. Keep
# those mapper tests useful, while never silently downgrading a later stage.
if [ "$EXIT_CODE" -eq 2 ] && [ "$STAGE" = map ]; then
    echo "Runtime does not advertise staged CLI; retrying legacy map invocation."
    run_runtime "$RUNTIME" "$LIBGAME"
fi

if [ "$DIAGNOSTIC_SNAPSHOTS" = on ]; then
    snapshot_user_files "$GAMEDIR/logs/userdata-after.txt" || true
    echo "Userdata changes"
    echo "----------------"
    if command -v diff >/dev/null 2>&1; then
        diff -u "$GAMEDIR/logs/userdata-before.txt" \
            "$GAMEDIR/logs/userdata-after.txt" || true
    else
        echo "Snapshots saved as logs/userdata-before.txt and userdata-after.txt"
    fi
fi

echo
echo "runtime_exit=$EXIT_CODE"
sync

if [ "$EXIT_CODE" -ne 0 ]; then
    notify_failure "LCS stage '$STAGE' failed (exit $EXIT_CODE). See logs/launcher.log."
    sleep "$CONSOLE_WAIT"
elif [ "$GRAPHICAL" -eq 0 ]; then
    tty_write "LCS stage '$STAGE': PASS. See gtalcs/logs/launcher.log."
    sleep "$CONSOLE_WAIT"
fi

exit "$EXIT_CODE"
