CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
SRC_DIR := src
TEST_DIR := tests
BUILD_DIR := build
TARGET := visual-window-app

# Pure-logic objects (no SDL): always buildable and unit-tested.
PURE_SOURCES := $(SRC_DIR)/jsonutil.c $(SRC_DIR)/geometry.c \
                $(SRC_DIR)/config.c $(SRC_DIR)/migrate.c
PURE_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(PURE_SOURCES))

SDL_AVAILABLE := $(shell pkg-config --exists sdl2 SDL2_image && echo yes)
ifeq ($(SDL_AVAILABLE),yes)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_image)
SDL_LIBS := $(shell pkg-config --libs sdl2 SDL2_image)
GUI_SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c \
               $(SRC_DIR)/renderer.c $(SRC_DIR)/monitors.c
GUI_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(GUI_SOURCES))
else
SDL_CFLAGS :=
SDL_LIBS :=
GUI_OBJECTS :=
endif

LOGIC_TEST := $(BUILD_DIR)/test_sync_logic

.PHONY: all clean run test check c-test py-test gui

all: gui

gui:
ifeq ($(SDL_AVAILABLE),yes)
	$(MAKE) $(TARGET)
else
	@echo "SDL2/SDL2_image not installed; building logic tests only."
	$(MAKE) $(LOGIC_TEST)
endif

$(TARGET): $(PURE_OBJECTS) $(GUI_OBJECTS)
	$(CC) $(CFLAGS) $(GUI_OBJECTS) $(PURE_OBJECTS) -o $@ $(SDL_LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LOGIC_TEST): $(PURE_OBJECTS) $(TEST_DIR)/test_sync_logic.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $(TEST_DIR)/test_sync_logic.c \
		$(PURE_SOURCES) -o $@

c-test: $(LOGIC_TEST)
	$(LOGIC_TEST)

py-test:
	cd sync && PYTHONPATH=. python3 -m unittest discover -s tests

interop-test:
	bash tests/test_c_python_interop.sh

test check: c-test interop-test py-test

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
