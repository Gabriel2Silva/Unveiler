SRC_7Z = 7zip-main/CPP
SRC_C  = 7zip-main/C

CXX = g++
CC  = gcc

# ---- hardening flags ----
HARDEN_CFLAGS  = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security
HARDEN_LDFLAGS = -Wl,-z,relro,-z,now -Wl,-z,noexecstack

CXXFLAGS = -O2 -Wall -Wextra -DNDEBUG -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
           -fPIC $(HARDEN_CFLAGS)
CFLAGS   = -O2 -Wall -Wextra -DNDEBUG -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE \
           -fPIC $(HARDEN_CFLAGS)
LDFLAGS  = -lpthread -ldl $(HARDEN_LDFLAGS)

O = _o

# Same objects Client7z needs, minus Client7z.o itself
COMMON_OBJS = \
  $(O)/IntToString.o \
  $(O)/MyString.o \
  $(O)/MyVector.o \
  $(O)/NewHandler.o \
  $(O)/StringConvert.o \
  $(O)/StringToInt.o \
  $(O)/UTFConvert.o \
  $(O)/Wildcard.o

WIN_OBJS = \
  $(O)/DLL.o \
  $(O)/FileDir.o \
  $(O)/FileFind.o \
  $(O)/FileIO.o \
  $(O)/FileName.o \
  $(O)/PropVariant.o \
  $(O)/PropVariantConv.o \
  $(O)/TimeUtils.o

SYS_OBJS = $(O)/MyWindows.o

Z7_OBJS = $(O)/FileStreams.o

C_OBJS = $(O)/Alloc.o

BRIDGE_OBJS = $(O)/fm-bridge.o

ALL_OBJS = $(C_OBJS) $(COMMON_OBJS) $(WIN_OBJS) $(SYS_OBJS) $(Z7_OBJS) $(BRIDGE_OBJS)

all: $(O) $(O)/lib7zbridge.so $(O)/test-bridge $(O)/unveiler

$(O):
	mkdir -p $(O)

$(O)/lib7zbridge.so: $(ALL_OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

$(O)/test-bridge: $(O)/test-bridge.o $(ALL_OBJS)
	$(CXX) -pie -o $@ $^ $(LDFLAGS)

# Bridge source
$(O)/fm-bridge.o: bridge/fm-bridge.cpp bridge/fm-bridge-api.h bridge/StdAfx.h
	$(CXX) $(CXXFLAGS) -Ibridge -I$(SRC_7Z) -c -o $@ $<

$(O)/test-bridge.o: test-bridge.c bridge/fm-bridge-api.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 7-Zip Common
$(O)/IntToString.o: $(SRC_7Z)/Common/IntToString.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/MyString.o: $(SRC_7Z)/Common/MyString.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/MyVector.o: $(SRC_7Z)/Common/MyVector.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/NewHandler.o: $(SRC_7Z)/Common/NewHandler.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/StringConvert.o: $(SRC_7Z)/Common/StringConvert.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/StringToInt.o: $(SRC_7Z)/Common/StringToInt.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/UTFConvert.o: $(SRC_7Z)/Common/UTFConvert.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/Wildcard.o: $(SRC_7Z)/Common/Wildcard.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/MyWindows.o: $(SRC_7Z)/Common/MyWindows.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 7-Zip Windows shim
$(O)/DLL.o: $(SRC_7Z)/Windows/DLL.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/FileDir.o: $(SRC_7Z)/Windows/FileDir.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/FileFind.o: $(SRC_7Z)/Windows/FileFind.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/FileIO.o: $(SRC_7Z)/Windows/FileIO.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/FileName.o: $(SRC_7Z)/Windows/FileName.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/PropVariant.o: $(SRC_7Z)/Windows/PropVariant.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/PropVariantConv.o: $(SRC_7Z)/Windows/PropVariantConv.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(O)/TimeUtils.o: $(SRC_7Z)/Windows/TimeUtils.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 7-Zip streams
$(O)/FileStreams.o: $(SRC_7Z)/7zip/Common/FileStreams.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# C layer
$(O)/Alloc.o: $(SRC_C)/Alloc.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(O)

# ---- GTK4 app ----

GTK_CFLAGS = $(shell pkg-config --cflags gtk4 libadwaita-1)
GTK_LIBS   = $(shell pkg-config --libs   gtk4 libadwaita-1)

GTK_OBJS = $(O)/main.o $(O)/fm-window.o $(O)/fm-file-item.o

$(O)/unveiler: $(GTK_OBJS) $(ALL_OBJS)
	$(CC) -pie -o $@ $(GTK_OBJS) $(ALL_OBJS) $(GTK_LIBS) $(LDFLAGS) -lstdc++

$(O)/main.o: src/main.c src/fm-window.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc -c -o $@ $<

$(O)/fm-window.o: src/fm-window.c src/fm-window.h src/fm-file-item.h bridge/fm-bridge-api.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc -c -o $@ $<

$(O)/fm-file-item.o: src/fm-file-item.c src/fm-file-item.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -Isrc -c -o $@ $<

.PHONY: all clean install uninstall test

# ---- tests ----

$(O)/test-suite.o: tests/test-suite.c bridge/fm-bridge-api.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

$(O)/test-suite: $(O)/test-suite.o $(ALL_OBJS)
	$(CXX) -pie -o $@ $^ $(LDFLAGS)

test: $(O) $(O)/test-suite
	@python3 tests/gen-test-archives.py /tmp/unveiler-test-archives
	@$(O)/test-suite /tmp/unveiler-test-archives /tmp/unveiler-test-tmp
	@rm -rf /tmp/unveiler-test-archives

PREFIX ?= /usr/local

install: $(O)/unveiler
	install -Dm755 $(O)/unveiler $(DESTDIR)$(PREFIX)/bin/unveiler
	install -Dm755 7zip-main/CPP/7zip/Bundles/Format7zF/_o/7z.so $(DESTDIR)$(PREFIX)/lib/unveiler/7z.so
	install -Dm644 org.unveiler.App.desktop $(DESTDIR)$(PREFIX)/share/applications/org.unveiler.App.desktop
	-update-desktop-database $(DESTDIR)$(PREFIX)/share/applications 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/unveiler
	rm -f $(DESTDIR)$(PREFIX)/lib/unveiler/7z.so
	rm -f $(DESTDIR)$(PREFIX)/share/applications/org.unveiler.App.desktop
	-rmdir $(DESTDIR)$(PREFIX)/lib/unveiler
