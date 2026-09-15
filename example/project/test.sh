#!/bin/sh
# Trusted acceptance tests: do not edit to make the example pass.
set -eu
. ./battery.sh

failures=0
checks=0
check() {
    BATTERY_STATUS=unset
    battery_status "$1"
    checks=$((checks + 1))
    if [ "$BATTERY_STATUS" = "$2" ]; then
        printf 'PASS %s -> %s\n' "$1" "$2"
    else
        printf 'FAIL %s: expected %s, got %s\n' "$1" "$2" "$BATTERY_STATUS"
        failures=$((failures + 1))
    fi
}

check 0 low
check 1 low
check 19 low
check 20 ok
check 21 ok
check 50 ok
check 79 ok
check 80 full
check 81 full
check 99 full
check 100 full
# Exercise transitions as well as isolated inputs.
check 80 full
check 19 low
check 20 ok

if [ "$failures" -ne 0 ]; then
    printf 'EXAMPLE_FAIL checks=%s failures=%s\n' "$checks" "$failures"
    exit 1
fi
printf 'EXAMPLE_PASS checks=%s\n' "$checks"
