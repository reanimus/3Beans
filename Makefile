NAME := 3beans
BUILD := build
META := meta
SRCS := src src/core src/core/arm src/core/convert src/core/dsp src/core/gpu src/core/io src/core/memory src/desktop src/scripting
LUA_DIR := third_party/lua
LUA_LIB := $(BUILD)/$(LUA_DIR)/liblua.a
LUA_CFLAGS := -O2 -std=c++11 -x c++ -include third_party/lua_cpp.h
ARGS := -O3 -flto -std=c++11 -DLOG_LEVEL=0
LIBS := $(shell pkg-config --libs portaudio-2.0 epoxy libpng)
INCS := -I$(LUA_DIR) $(shell pkg-config --cflags portaudio-2.0 epoxy libpng)

APPNAME := 3Beans
PKGNAME := com.hydra.threebeans
DESTDIR ?= /usr
CXX ?= g++

ifeq ($(OS),Windows_NT)
  ARGS += -static -DWINDOWS
  LIBS += $(shell pkg-config --static --libs libpng)
  LIBS += $(shell wx-config-static --libs --gl-libs) -lole32 -lsetupapi -lwinmm
  INCS += $(shell wx-config-static --cxxflags)
else
  LIBS += $(shell wx-config --libs --gl-libs)
  INCS += $(shell wx-config --cxxflags)
  ifeq ($(shell uname -s),Darwin)
    LUA_CFLAGS += -DLUA_USE_MACOSX
    ARGS += -DMACOS
    LIBS += -headerpad_max_install_names
  else
    ARGS += -no-pie
    LUA_CFLAGS += -DLUA_USE_LINUX
    LIBS += -lGL -ldl -lm -Wl,--dynamic-list=third_party/lua.exports
  endif
endif

CPPFILES := $(foreach dir,$(SRCS),$(wildcard $(dir)/*.cpp))
HFILES := $(foreach dir,$(SRCS),$(wildcard $(dir)/*.h))
OFILES := $(patsubst %.cpp,$(BUILD)/%.o,$(CPPFILES))

ifeq ($(OS),Windows_NT)
  OFILES += $(BUILD)/icon-windows.o
  # MinGW LTO can discard inline wxWidgets virtual thunks referenced by its
  # static libraries. Compile the desktop normally; keep LTO for the core.
  $(BUILD)/src/desktop/%.o: ARGS := $(filter-out -flto%,$(ARGS))
endif

all: $(NAME)

ifneq ($(OS),Windows_NT)
ifeq ($(uname -s),Darwin)

install: $(NAME)
	$(META)/mac-bundle.sh
	cp -r $(APPNAME).app /Applications/

uninstall:
	rm -rf /Applications/$(APPNAME).app

else

flatpak:
	flatpak-builder --repo=repo --force-clean build-flatpak $(META)/$(PKGNAME).yml
	flatpak build-bundle repo $(NAME).flatpak $(PKGNAME)

flatpak-clean:
	rm -rf .flatpak-builder
	rm -rf build-flatpak
	rm -rf repo
	rm -f $(NAME).flatpak

install: $(NAME)
	install -Dm755 $(NAME) "$(DESTDIR)/bin/$(NAME)"
	install -Dm644 third_party/lua/LICENSE "$(DESTDIR)/share/licenses/$(NAME)/Lua-LICENSE.txt"
	install -Dm644 $(META)/$(PKGNAME).desktop "$(DESTDIR)/share/applications/$(PKGNAME).desktop"
	install -Dm644 icon/linux.png "$(DESTDIR)/share/icons/hicolor/256x256/apps/$(PKGNAME).png"

uninstall:
	rm -f "$(DESTDIR)/bin/$(NAME)"
	rm -f "$(DESTDIR)/share/licenses/$(NAME)/Lua-LICENSE.txt"
	rm -f "$(DESTDIR)/share/applications/$(PKGNAME).desktop"
	rm -f "$(DESTDIR)/share/icons/hicolor/256x256/apps/$(PKGNAME).png"

endif
endif

$(NAME): $(OFILES) $(LUA_LIB)
	$(CXX) -o $@ $(ARGS) $^ $(LIBS)

$(BUILD)/%.o: %.cpp $(HFILES) $(wildcard $(LUA_DIR)/*.h) $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) -c -o $@ $(ARGS) $(INCS) $<

$(LUA_LIB): $(patsubst %.c,$(BUILD)/%.o,$(wildcard $(LUA_DIR)/*.c))
	$(AR) rcs $@ $^

$(BUILD)/$(LUA_DIR)/%.o: $(LUA_DIR)/%.c $(wildcard $(LUA_DIR)/*.h) third_party/lua_cpp.h
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LUA_CFLAGS) -c -o $@ $<

$(BUILD)/icon-windows.o:
	windres $(shell wx-config-static --cppflags) icon/windows.rc $@

$(BUILD):
	for dir in $(SRCS); do mkdir -p $(BUILD)/$$dir; done

clean:
	rm -rf $(BUILD)
	rm -f $(NAME)

.PHONY: test
$(BUILD)/tests/session-test: tests/session_test.cpp $(filter-out $(BUILD)/src/desktop/% $(BUILD)/icon-windows.o,$(OFILES)) $(LUA_LIB)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $(ARGS) $(INCS) $^ $(LIBS)

test: $(NAME) $(BUILD)/tests/session-test
	python3 tests/test_scripting.py ./$(NAME) ./$(BUILD)/tests/session-test
