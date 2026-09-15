# Battery indicator logic. Source this file, then call battery_status PERCENT.
# The result is returned in BATTERY_STATUS, not printed.
battery_status() {
    if [ "$1" -le 20 ]; then
        BATTERY_STATUS=low
    elif [ "$1" -gt 80 ]; then
        BATTERY_STATUS=full
    else
        BATTERY_STATUS=ok
    fi
}
