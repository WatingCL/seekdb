CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRC := $(wildcard src/*.cpp)
HDR := $(wildcard src/*.hpp)

minivectordb: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -pthread $(SRC) -o $@

clean:
	rm -f minivectordb

.PHONY: clean
