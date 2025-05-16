ifeq ($(OS),Windows_NT)
	CXX := cl
	CXX_FLAGS := /EHsc /await:strict /std:c++20 /Zi
else
	CXX := g++-13
	CXX_FLAGS := -std=c++2a -O3 -Wno-format-security
endif

all:
	${CXX} ${CXX_FLAGS} tests.cpp
