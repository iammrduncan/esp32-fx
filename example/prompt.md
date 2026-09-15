Fix the battery indicator in this workspace.

Read SPEC.md, battery.sh, and test.sh with the available tools. Run the tests before
changing anything. Diagnose the failing boundary cases, then edit only battery.sh
to match the specification. Keep the function small and use only POSIX shell
builtins. Do not modify or bypass the tests, print fake results, access other files,
or execute external commands.

Run the tests again. If they fail, use their output to correct your change and rerun
them. Only report success after run_tests returns exit_code=0 and the actual output
contains EXAMPLE_PASS checks=14.

Finish with a short explanation of the bug, the change, and the observed test
result. Keep your final answer under 600 characters so it fits the e-paper display.
