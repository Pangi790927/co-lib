ifeq ($(OS),Windows_NT)
	CXX := cl
	CXX_FLAGS := /EHsc /await:strict /std:c++20 /Zi
else
	CXX := g++-13
	CXX_FLAGS := -std=c++2a -O3 -Wno-format-security
endif

all:
	${CXX} ${CXX_FLAGS} tests.cpp

# # For this to work I've done:
# git clone https://github.com/mheily/libkqueue
# cd libkqueue && mkdir build && cd build
# cmake -G "Unix Makefiles" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib ..
# make
# sudo make install
# sudo mv /usr/include/kqueue/sys/event.h /usr/include/sys/
# # Maybe not nice, but does the job
unix:
	${CXX} -DCOLIB_OS_UNIX=true ${CXX_FLAGS} tests.cpp -lkqueue

doc:
	doxygen doxfile
	make -C latex

clean:
	rm -f tests.exe
	rm -f a.out
	rm -rf latex
	rm -rf html
