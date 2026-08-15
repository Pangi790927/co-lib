# Pinning SHELL to cmd.exe (always present on any Windows install, unlike a POSIX shell, which
# needs Git-for-Windows/MSYS's usr/bin on PATH) means every recipe line below - even a single bare
# command like `del` - reliably runs through a real shell instead of GNU Make trying (and failing)
# to CreateProcess the first word directly, the way it does when no shell can be found. `cl` itself
# still needs a Developer Command Prompt / vcvars environment on PATH; that's a compiler
# requirement no makefile can paper over.
SHELL       := cmd.exe
.SHELLFLAGS := /c

CXX        := cl
CXX_FLAGS  := /EHsc /await:strict /std:c++20 /Zi /I..
CXX_OUT    := /Fe:
LINK_FLAGS := /link
EXE_EXT    := .exe
RM         := del /F /Q

# Get list of all test files
TEST_FILES := $(wildcard *.cpp)

# Generate target names from test files (replace .cpp with executable name + extension)
TEST_TARGETS := $(patsubst %.cpp,%$(EXE_EXT),$(TEST_FILES))

# 'all' target runs every test regardless of earlier failures - a failing test moves on to the
# next one instead of stopping the run, with overall pass/fail reported via the exit code at the
# end. Driven by run_tests.py rather than a cmd.exe batch loop: cmd's setlocal/delayed-expansion/
# errorlevel handling turned out to be too unreliable to get this right in a single make recipe
# line (silently lost the failure flag across the for loop even with /v:on) - see run_tests.py.
all: $(TEST_TARGETS)
	@python run_tests.py $(TEST_TARGETS)

# For each test file, create a compile rule. ../colib.h and tests_common.h are listed as
# prerequisites (not just %.cpp) so a colib.h edit correctly invalidates every test's .exe -
# without this, `make`'s incremental rebuild would silently keep serving a stale binary built
# against an older colib.h.
$(TEST_TARGETS): %$(EXE_EXT): %.cpp ../colib.h tests_common.h
	${CXX} ${CXX_FLAGS} $< ${CXX_OUT}$@ ${LINK_FLAGS}

# Clean target
clean:
	$(RM) $(TEST_TARGETS)
	$(RM) *.exe
	$(RM) *.obj
	$(RM) *.pdb
	$(RM) *.ilk
