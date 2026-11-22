CXX ?= c++
BUILD ?= ../../build
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS := $(shell pkg-config --libs sdl2)
CXXFLAGS ?= -O2 -g

.PHONY: all clean
all: $(BUILD)/zeroteam

$(BUILD)/frontend.o: main.cpp progress.hpp zeroteam_board.h
	mkdir -p "$(BUILD)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Wpedantic $(SDL_CFLAGS) -c main.cpp -o "$@"

$(BUILD)/zeroteam: $(BUILD)/frontend.o $(BUILD)/libzeroteam_board.so
	$(CXX) $(LDFLAGS) "$<" -L"$(BUILD)" -lzeroteam_board $(SDL_LIBS) -Wl,-rpath,'$$ORIGIN' -o "$@"

clean:
	rm -f "$(BUILD)/frontend.o" "$(BUILD)/zeroteam"
