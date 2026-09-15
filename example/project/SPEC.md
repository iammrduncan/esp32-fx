# Battery indicator

Implement `battery_status PERCENT` in `battery.sh` using POSIX shell builtins.
The caller supplies one decimal integer from 0 through 100, without leading zeros.
Input validation outside that range is not part of this exercise.

Set the shell variable `BATTERY_STATUS` to:

- `low` for 0 through 19, inclusive;
- `ok` for 20 through 79, inclusive;
- `full` for 80 through 100, inclusive.

Each call must replace the previous result. Do not print anything, exit the shell,
start another process, read other files, or change unrelated variables. The project
needs neither a compiler nor external commands. Only `battery.sh` may be edited.
