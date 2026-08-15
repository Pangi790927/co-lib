#!/usr/bin/env python3
# Runs every test binary passed on argv, one after another, regardless of individual failures -
# a failing test doesn't stop the rest from running. Doesn't duplicate the [PASSED]/[FAILED] each
# binary already prints via print_test_result(); this just drives the run and tracks the overall
# exit code (nonzero if any binary failed), which is what `make all` actually needs and what
# cmd.exe's batch scripting (setlocal/delayed-expansion/errorlevel) turned out to be too unreliable
# to do correctly in a single make recipe line - see windows.makefile/linux.makefile.
import subprocess
import sys

if sys.platform == "win32":
    import ctypes

    _kernel32 = ctypes.windll.kernel32
    _STD_OUTPUT_HANDLE = -11
    _FG_RED = 0x0004
    _FG_GREEN = 0x0002
    _FG_BLUE = 0x0001
    _FG_INTENSITY = 0x0008
    _RESET_ATTR = _FG_RED | _FG_GREEN | _FG_BLUE
    _COLOR_ATTR = {"green": _FG_GREEN | _FG_INTENSITY, "red": _FG_RED | _FG_INTENSITY}

    def colored(text: str, color: str) -> None:
        handle = _kernel32.GetStdHandle(_STD_OUTPUT_HANDLE)
        _kernel32.SetConsoleTextAttribute(handle, _COLOR_ATTR[color])
        sys.stdout.write(text)
        sys.stdout.flush()
        _kernel32.SetConsoleTextAttribute(handle, _RESET_ATTR)
else:
    _ANSI = {"green": "\033[32m", "red": "\033[31m"}

    def colored(text: str, color: str) -> None:
        sys.stdout.write(f"{_ANSI[color]}{text}\033[0m")


def main() -> int:
    binaries = sys.argv[1:]
    failures = []
    for binary in binaries:
        print(f"Running {binary}...")
        ret = subprocess.call(["./" + binary])
        if ret != 0:
            failures.append(binary)
    passed = len(binaries) - len(failures)

    print("All tests completed!")
    sys.stdout.write("[")
    colored(f"PASSED: {passed}", "green")
    sys.stdout.write("]/[")
    colored(f"FAILED: {len(failures)}", "red")
    sys.stdout.write("]\n")
    if failures:
        print("Failed: " + ", ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
