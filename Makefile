LUA_VERSION ?= 5.4.9
BUILD       ?= build
JAVAC       ?= javac
JDK         ?= $(shell dirname $$(dirname $$(readlink -f $$(command -v $(JAVAC)))))

LUA_SRC     ?= $(BUILD)/lua-$(LUA_VERSION)
BUNDLED_LUA := $(LUA_SRC)/src/lua
LUA_INC     ?= $(LUA_SRC)/src
LUA         ?= $(BUNDLED_LUA)

ifeq ($(LUA),$(BUNDLED_LUA))
LUA_DEP := $(BUNDLED_LUA)
endif

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
override CFLAGS += -fPIC -I$(LUA_INC) -I$(JDK)/include -I$(JDK)/include/linux
override LDFLAGS += -shared
LDLIBS  += -ldl

CLASSES   := $(BUILD)/test-classes
CHECK_JNI ?=
MODULE  := $(BUILD)/luajni/jni.so

all: $(MODULE) $(BUILD)/luajni.lua

$(MODULE): src/luajni.c $(LUA_INC)/lua.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

ifeq ($(LUA_INC),$(LUA_SRC)/src)
$(LUA_INC)/lua.h: $(BUNDLED_LUA)
endif

$(BUILD)/luajni.lua: lua/luajni.lua
	@mkdir -p $(@D)
	cp $< $@

lua: $(BUNDLED_LUA)

$(BUNDLED_LUA): $(BUILD)/lua-$(LUA_VERSION).tar.gz
	tar -xzf $< -C $(BUILD)
	env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(LUA_SRC) linux MYCFLAGS=-fPIC

$(BUILD)/lua-$(LUA_VERSION).tar.gz:
	@mkdir -p $(@D)
	curl -fsSL https://www.lua.org/ftp/lua-$(LUA_VERSION).tar.gz -o $@.tmp
	mv $@.tmp $@

$(CLASSES)/demo/Demo.class: test/java/demo/Demo.java
	@mkdir -p $(CLASSES)
	$(JAVAC) --release 17 -d $(CLASSES) $<

test: all $(CLASSES)/demo/Demo.class $(LUA_DEP)
	LUA_CPATH="$(abspath $(BUILD))/?.so;;" \
	LUA_PATH="$(abspath $(BUILD))/?.lua;;" \
	LUAJNI_CLASSPATH="$(abspath $(CLASSES))" \
	LUAJNI_CHECK_JNI="$(CHECK_JNI)" \
	$(LUA) test/test.lua

PREFIX      ?= /usr/local
LUA_LIBDIR  ?= $(PREFIX)/lib/lua/5.4
LUA_SHAREDIR ?= $(PREFIX)/share/lua/5.4

install: all
	install -Dm755 $(MODULE) $(LUA_LIBDIR)/luajni/jni.so
	install -Dm644 $(BUILD)/luajni.lua $(LUA_SHAREDIR)/luajni.lua

clean:
	rm -rf $(BUILD)

.PHONY: all lua test install clean
