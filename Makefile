CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pthread
TARGET    = my_redis_server
SRC       = $(wildcard src/*.cpp)
OBJ       = $(SRC:src/%.cpp=build/%.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp $(wildcard include/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	python3 tests/test_server.py

clean:
	rm -rf build $(TARGET) dump.my_rdb dump.my_rdb.tmp

.PHONY: all run test clean
