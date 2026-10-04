CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
SRC_DIR := src

# SDL 无关的状态同步核心模块（C 终端与 GUI 应用共用）
CORE_SOURCES := $(SRC_DIR)/cjson.c $(SRC_DIR)/config.c $(SRC_DIR)/state_file.c $(SRC_DIR)/sync_client.c

# GUI 应用（需要 SDL2，容器内构建）
APP_TARGET := visual-window-app
APP_SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c
SDL_AVAILABLE := $(shell command -v sdl2-config >/dev/null 2>&1 && echo yes)
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)

# 无头 C 终端（本机即可构建/测试）
TERM_TARGET := termctl

.PHONY: all clean run term test check-sdl

ifeq ($(SDL_AVAILABLE),yes)
all: $(APP_TARGET) $(TERM_TARGET)
else
all: $(TERM_TARGET)
	@echo "note: sdl2-config not found; skipped $(APP_TARGET) (build it inside docker)"
endif

$(APP_TARGET): $(APP_SOURCES) $(CORE_SOURCES)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(APP_SOURCES) $(CORE_SOURCES) -o $@ $(SDL_LIBS) -lSDL2_image

$(TERM_TARGET): $(SRC_DIR)/termctl.c $(CORE_SOURCES)
	$(CC) $(CFLAGS) $(SRC_DIR)/termctl.c $(CORE_SOURCES) -o $@

term: $(TERM_TARGET)

# 无 SDL 环境下用桩头文件对 GUI 源码做编译检查
check-sdl:
	@for f in $(APP_SOURCES); do \
		$(CC) $(CFLAGS) -I tests/sdl_stub -c $$f -o /dev/null || exit 1; \
	done
	@echo "SDL sources compile OK (stub headers)"

run: $(APP_TARGET)
	./$(APP_TARGET)

test: $(TERM_TARGET) check-sdl
	python3 tests/test_sync.py

clean:
	rm -f $(APP_TARGET) $(TERM_TARGET)
