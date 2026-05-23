#!/system/bin/sh

MODDIR=${0%/*}
SYNC_SCRIPT="$MODDIR/mode-sync.sh"
LOGFILE="$MODDIR/ztp2mouse_service.log"
HOTKEY_NAME="zkb_input"
HOTKEY_CODE="00fa"
SYNC_DELAY=2
DEBOUNCE_MS=1500
RETRY_DELAY=3
LAST_TRIGGER_FILE="$MODDIR/.last_hotkey_ms"

log_msg() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$LOGFILE"
}

now_ms() {
    date +%s%3N
}

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

should_handle_trigger() {
    current_ms="$(now_ms)"
    last_ms=0

    if [ -f "$LAST_TRIGGER_FILE" ]; then
        last_ms="$(cat "$LAST_TRIGGER_FILE" 2>/dev/null)"
    fi

    if [ -n "$last_ms" ] && [ "$last_ms" -gt 0 ] 2>/dev/null; then
        delta_ms=$((current_ms - last_ms))
        if [ "$delta_ms" -lt "$DEBOUNCE_MS" ]; then
            log_msg "hotkey ignored by debounce (${delta_ms}ms < ${DEBOUNCE_MS}ms)"
            return 1
        fi
    fi

    echo "$current_ms" > "$LAST_TRIGGER_FILE"
    return 0
}

until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 5
done

if [ ! -f "$SYNC_SCRIPT" ]; then
    log_msg "sync script missing: $SYNC_SCRIPT"
    exit 1
fi

sh "$SYNC_SCRIPT"

while :; do
    HOTKEY_DEV="$(find_input_dev_by_name "$HOTKEY_NAME")"

    if [ -z "$HOTKEY_DEV" ] || [ ! -e "$HOTKEY_DEV" ]; then
        log_msg "hotkey device not found for name '$HOTKEY_NAME', retry in ${RETRY_DELAY}s"
        sleep "$RETRY_DELAY"
        continue
    fi

    log_msg "listening on $HOTKEY_DEV for device '$HOTKEY_NAME' key $HOTKEY_CODE"

    getevent -l "$HOTKEY_DEV" 2>/dev/null | while IFS= read -r line; do
        case "$line" in
            *"EV_KEY"*"$HOTKEY_CODE"*UP*)
                if ! should_handle_trigger; then
                    continue
                fi
                log_msg "hotkey released, syncing after ${SYNC_DELAY}s"
                sleep "$SYNC_DELAY"
                sh "$SYNC_SCRIPT"
                ;;
        esac
    done

    log_msg "hotkey listener exited for $HOTKEY_DEV, retry in ${RETRY_DELAY}s"
    sleep "$RETRY_DELAY"
done
