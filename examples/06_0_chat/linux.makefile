CXX        := g++
CXX_FLAGS  := -std=c++2a -O3 -g -Wno-format-security -I../.. -pthread
CXX_OUT    := -o
EXE_EXT    := .bin

TARGETS := chat_server$(EXE_EXT) chat_client$(EXE_EXT)

all: $(TARGETS)

# ../../colib.h and chat_common.h are prerequisites (not just the .cpp) so an edit to either
# correctly invalidates the stale binary on the next `make` - same reasoning as tests/linux.makefile.
chat_server$(EXE_EXT): chat_server.cpp ../../colib.h chat_common.h
	${CXX} ${CXX_FLAGS} chat_server.cpp ${CXX_OUT} chat_server$(EXE_EXT)

chat_client$(EXE_EXT): chat_client.cpp ../../colib.h chat_common.h
	${CXX} ${CXX_FLAGS} chat_client.cpp ${CXX_OUT} chat_client$(EXE_EXT)

clean:
	rm -f chat_server.bin chat_client.bin chat_server chat_client
