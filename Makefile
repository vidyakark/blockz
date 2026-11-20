CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
LDFLAGS  ?= -pthread
BUILD    := build
CORE     := $(BUILD)/crc32.o $(BUILD)/huffman.o $(BUILD)/deflate_enc.o $(BUILD)/inflate.o $(BUILD)/gzip.o

all: $(BUILD)/blockz $(BUILD)/bench $(BUILD)/gen_data $(BUILD)/run_tests

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/blockz: $(CORE) $(BUILD)/main.o
	$(CXX) $^ -o $@ $(LDFLAGS)
$(BUILD)/bench: $(CORE) tools/bench.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
$(BUILD)/gen_data: tools/gen_data.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@
$(BUILD)/run_tests: $(CORE) tests/test_main.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Unit tests plus interoperability tests against the system gzip (and Python's zlib if present)
test: $(BUILD)/run_tests $(BUILD)/blockz $(BUILD)/gen_data
	./$(BUILD)/run_tests
	sh tests/interop.sh
clean:
	rm -rf $(BUILD)
.PHONY: all test clean
