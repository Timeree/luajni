LUA_VERSION ?= 5.4.9
BUILD       ?= build
JDK         ?= $(shell dirname $$(dirname $$(readlink -f $$(command -v javac))))
JAVAC       ?= javac

LUA_SRC ?= $(BUILD)/lua-$(LUA_VERSION)
LUA_INC ?= $(LUA_SRC)/src
LUA     ?= $(LUA_SRC)/src/lua

CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
override CFLAGS += -fPIC -I$(LUA_INC) -I$(JDK)/include -I$(JDK)/include/linux
override LDFLAGS += -shared
LDLIBS  += -ldl

CLASSES := $(BUILD)/test-classes
MODULE  := $(BUILD)/luajni/jni.so

all: $(MODULE) $(BUILD)/luajni.lua

$(MODULE): src/luajni.c $(LUA_INC)/lua.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(LUA_INC)/lua.h: $(LUA)

$(BUILD)/luajni.lua: lua/luajni.lua
	@mkdir -p $(@D)
	cp $< $@

lua: $(LUA)

$(LUA): $(BUILD)/lua-$(LUA_VERSION).tar.gz
	tar -xzf $< -C $(BUILD)
	env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(LUA_SRC) linux MYCFLAGS=-fPIC

$(BUILD)/lua-$(LUA_VERSION).tar.gz:
	@mkdir -p $(@D)
	curl -fsSL https://www.lua.org/ftp/lua-$(LUA_VERSION).tar.gz -o $@.tmp
	mv $@.tmp $@

$(CLASSES)/demo/Demo.class: test/java/demo/Demo.java
	@mkdir -p $(CLASSES)
	$(JAVAC) --release 17 -d $(CLASSES) $<

test: all $(CLASSES)/demo/Demo.class $(LUA)
	LUA_CPATH="$(abspath $(BUILD))/?.so;;" \
	LUA_PATH="$(abspath $(BUILD))/?.lua;;" \
	LUAJNI_CLASSPATH="$(abspath $(CLASSES))" \
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
