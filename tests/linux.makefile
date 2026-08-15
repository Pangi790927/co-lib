CXX        := g++
CXX_FLAGS  := -std=c++2a -O3 -g -Wno-format-security -I..
CXX_OUT    := -o
EXE_EXT    := .bin

# Get list of all test files
TEST_FILES := $(wildcard *.cpp)

# Generate target names from test files (replace .cpp with executable name + extension)
TEST_TARGETS := $(patsubst %.cpp,%$(EXE_EXT),$(TEST_FILES))

# 'all' target runs every test regardless of earlier failures - a failing test moves on to the
# next one instead of stopping the run, with overall pass/fail reported via the exit code at the
# end. Driven by run_tests.py (shared with windows.makefile) rather than an inline shell loop.
all: $(TEST_TARGETS)
	@python3 run_tests.py $^

# For each test file, create a compile rule. ../colib.h and tests_common.h are listed as
# prerequisites (not just %.cpp) so a colib.h edit correctly invalidates every test's .bin -
# without this, `make`'s incremental rebuild would silently keep serving a stale binary built
# against an older colib.h.
$(TEST_TARGETS): %$(EXE_EXT): %.cpp ../colib.h tests_common.h
	${CXX} ${CXX_FLAGS} $< ${CXX_OUT} $@

# Special unix target with flag for Unix systems
# Note: This requires libkqueue to be installed
unix: CXX_FLAGS += -DCOLIB_OS_UNIX=true
unix: LINK_FLAGS += -lkqueue
unix: $(TEST_TARGETS)
	@python3 run_tests.py $^

# Special unix target that links with kqueue for Linux
unix_kqueue: CXX_FLAGS += -DCOLIB_OS_UNIX=true
unix_kqueue: LINK_FLAGS += -lkqueue
unix_kqueue: $(TEST_TARGETS)
	@python3 run_tests.py $^

# Clean target
clean:
	rm -f $(TEST_TARGETS)
	rm -f *.exe *.bin
	rm -f a.out
	rm -f *.obj *.pdb *.ilk
