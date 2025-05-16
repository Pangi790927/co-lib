ifeq ($(OS),Windows_NT)
	CXX := cl
	CXX_FLAGS := /EHsc /await:strict /std:c++20 /Zi
endif
ifeq ($(OS),Linux)
	CXX := g++-11
	CXX_FLAGS := -std=c++2a -O3
endif

all:
	${CXX} ${CXX_FLAGS} tests.cpp
