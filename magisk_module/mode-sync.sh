#!/system/bin/sh

MODDIR=${0%/*}
BIN="$MODDIR/system/bin/ztp2mouse"
PIDFILE="$MODDIR/ztp2mouse.pid"
LOGFILE="$MODDIR/ztp2mouse_service.log"
TARGET_HOME="com.zte.usmartlauncher"
TOUCHPAD_NAME="ztp_input"

find_input_dev_by_name() {
    getevent -lp 2>/dev/null | awk -v target="$1" '
        /^add device [0-9]+: / {
            dev = $4
            sub(/:$/, "", dev)
            next
        }
        /^[[:space:]]*name:[[:space:]]*"/ {
            name = $0
            sub(/^[[:space:]]*name:[[:space:]]*"/, "", name)
            sub(/"$/, "", name)
            if (name == target) {
                print dev
                exit
            }
        }
    '
}

log_msg() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$LOGFILE"
}

is_ztp_running() {
    if pidof ztp2mouse >/dev/null 2>&1; then
        return 0
    fi

    if [ -f "$PIDFILE" ]; then
        pid="$(cat "$PIDFILE" 2>/dev/null)"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        rm -f "$PIDFILE"
    fi
    return 1
}

list_running_pids() {
    pids=""

    if [ -f "$PIDFILE" ]; then
        pid="$(cat "$PIDFILE" 2>/dev/null)"
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            pids="$pid"
        fi
    fi

    pidof_output="$(pidof ztp2mouse 2>/dev/null)"
    if [ -n "$pidof_output" ]; then
        for pid in $pidof_output; do
            case " $pids " in
                *" $pid "*)
                    ;;
                *)
                    pids="$pids $pid"
                    ;;
            esac
        done
    fi

    echo "$pids" | awk '{$1=$1; print}'
}

read_cmdline() {
    pid="$1"
    tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null
}

start_ztp() {
    touchpad_dev="$1"
    running_pid="$(list_running_pids | awk '{print $1}')"
    if [ -n "$running_pid" ]; then
        echo "$running_pid" > "$PIDFILE"
        return 0
    fi
    if is_ztp_running; then
        return 0
    fi
    if [ ! -x "$BIN" ]; then
        log_msg "ztp2mouse binary not found: $BIN"
        return 1
    fi

    "$BIN" -i "$touchpad_dev" >/dev/null 2>&1 &
    echo $! > "$PIDFILE"
    log_msg "started ztp2mouse for usmart launcher mode on $touchpad_dev"
}

stop_ztp() {
    stopped=0
    running_pids="$(list_running_pids)"

    if [ -n "$running_pids" ]; then
        for pid in $running_pids; do
            kill "$pid" 2>/dev/null
            stopped=1
        done
        sleep 1
        for pid in $running_pids; do
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null
            fi
        done
    fi

    rm -f "$PIDFILE"

    if [ "$stopped" = "1" ]; then
        log_msg "stopped ztp2mouse for non-usmart launcher mode"
    fi
}

current_home="$(settings get secure default_home 2>/dev/null)"
log_msg "detected default_home: ${current_home:-<empty>}"

if [ "$current_home" = "$TARGET_HOME" ]; then
    touchpad_dev="$(find_input_dev_by_name "$TOUCHPAD_NAME")"
    if [ -z "$touchpad_dev" ] || [ ! -e "$touchpad_dev" ]; then
        log_msg "touchpad device not found for name '$TOUCHPAD_NAME'"
        exit 1
    fi

    running_pids="$(list_running_pids)"
    running_pid="$(echo "$running_pids" | awk '{print $1}')"
    if [ -n "$running_pid" ]; then
        running_cmdline="$(read_cmdline "$running_pid")"
        case " $running_cmdline " in
            *" -i $touchpad_dev "*)
                echo "$running_pid" > "$PIDFILE"
                log_msg "ztp2mouse already running on $touchpad_dev"
                exit 0
                ;;
        esac
        log_msg "ztp2mouse device changed, restarting for $touchpad_dev"
        stop_ztp
    fi

    start_ztp "$touchpad_dev"
else
    if is_ztp_running; then
        stop_ztp
    fi
fi
