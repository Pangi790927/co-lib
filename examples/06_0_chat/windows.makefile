# See tests/windows.makefile for the reasoning behind pinning SHELL to cmd.exe.
SHELL       := cmd.exe
.SHELLFLAGS := /c

CXX        := cl
CXX_FLAGS  := /EHsc /await:strict /std:c++20 /Zi /I..\..
CXX_OUT    := /Fe:
LINK_FLAGS := /link
EXE_EXT    := .exe

TARGETS := chat_server$(EXE_EXT) chat_client$(EXE_EXT)

all: $(TARGETS)

# ../../colib.h and chat_common.h are prerequisites (not just the .cpp) so an edit to either
# correctly invalidates the stale binary on the next `make` - same reasoning as tests/windows.makefile.
chat_server$(EXE_EXT): chat_server.cpp ..\..\colib.h chat_common.h
	${CXX} ${CXX_FLAGS} chat_server.cpp ${CXX_OUT}chat_server$(EXE_EXT) ${LINK_FLAGS}

chat_client$(EXE_EXT): chat_client.cpp ..\..\colib.h chat_common.h
	${CXX} ${CXX_FLAGS} chat_client.cpp ${CXX_OUT}chat_client$(EXE_EXT) ${LINK_FLAGS}

clean:
	del /F /Q chat_server.exe chat_client.exe *.obj *.pdb *.ilk 2>nul
