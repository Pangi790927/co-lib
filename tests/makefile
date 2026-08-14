# CXX_OUT must butt directly against the output filename with no space for cl's /Fe:, but g++'s -o
# needs a space before the filename - the trailing "$(EMPTY)" below preserves that trailing space
# (GNU Make would otherwise strip it as trailing whitespace on the assignment line).
EMPTY :=

ifeq ($(OS),Windows_NT)
	CXX := cl
	CXX_FLAGS := /EHsc /await:strict /std:c++20 /Zi /I..
	CXX_OUT := /Fe:
	LINK_FLAGS := /link
	EXE_EXT := .exe
else
	CXX := g++
	CXX_FLAGS := -std=c++2a -O3 -g -Wno-format-security -I..
	CXX_OUT := -o $(EMPTY)
	EXE_EXT := .bin
endif

# Get list of all test files
TEST_FILES := $(wildcard *.cpp)

# Generate target names from test files (replace .cpp with executable name + extension)
TEST_TARGETS := $(patsubst %.cpp,%$(EXE_EXT),$(TEST_FILES))

# 'all' target runs all tests
all: $(TEST_TARGETS)
	@echo "Running all tests..."
	@for test in $^; do \
		echo "Running $$test..."; \
		./$$test || exit 1; \
		done
	@echo "All tests completed!"

# For each test file, create a compile rule. ../colib.h and tests_common.h are listed as
# prerequisites (not just %.cpp) so a colib.h edit correctly invalidates every test's .exe/.bin -
# without this, `make`'s incremental rebuild would silently keep serving stale binaries built
# against an older colib.h.
$(TEST_TARGETS): %$(EXE_EXT): %.cpp ../colib.h tests_common.h
	${CXX} ${CXX_FLAGS} $< ${CXX_OUT}$@ ${LINK_FLAGS}

# Special unix target with flag for Unix systems
# Note: This requires libkqueue to be installed
unix: CXX_FLAGS += -DCOLIB_OS_UNIX=true
unix: LINK_FLAGS += -lkqueue
unix: $(TEST_TARGETS)
	@echo "Running all tests (unix mode)..."
	@for test in $^; do \
		echo "Running $$test (unix mode)..."; \
		./$$test || exit 1; \
		done
	@echo "All tests completed (unix mode)!"

# Special unix target that links with kqueue for Linux
unix_kqueue: CXX_FLAGS += -DCOLIB_OS_UNIX=true
unix_kqueue: LINK_FLAGS += -lkqueue
unix_kqueue: $(TEST_TARGETS)
	@echo "Running all tests (unix kqueue mode)..."
	@for test in $^; do \
		echo "Running $$test (unix kqueue mode)..."; \
		./$$test || exit 1; \
		done
	@echo "All tests completed (unix kqueue mode)!"

# Clean target
clean:
	rm -f $(TEST_TARGETS)
	rm -f *.exe *.bin
	rm -f a.out
	rm -f *.obj *.pdb *.ilk
