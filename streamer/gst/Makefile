BINARY_RELEASE = streaming-app
BINARY_DEBUG   = $(BINARY_RELEASE)-debug

INCLUDE = -I include
PKG_CONFIG = glib-2.0 gstreamer-1.0 libsoup-2.4 json-glib-1.0 glib-2.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0
VERSION = $(shell git describe --abbrev=8 --dirty --always --tags)

CFLAGS_COMMON = $(INCLUDE) -Wall -Wextra -Werror -MD -MP -fPIC `pkg-config --cflags $(PKG_CONFIG)` -DVERSION=\"$(VERSION)\"
CFLAGS_RELEASE  = -O3 -s -fexpensive-optimizations $(CFLAGS_COMMON)
CFLAGS_DEBUG = -O0 -g $(CFLAGS_COMMON)
LDFLAGS = `pkg-config --libs $(PKG_CONFIG)`

SRC = $(shell find src -type f)
OBJ_RELEASE = $(patsubst src/%.c, .build/%.o, $(SRC))
DEP_RELEASE = $(patsubst src/%.c, .build/%.d, $(SRC))
OBJ_DEBUG = $(patsubst src/%.c, .build/debug/%.o, $(SRC))
DEP_DEBUG = $(patsubst src/%.c, .build/debug/%.d, $(SRC))

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
CFGDIR  = $(PREFIX)/share/ct-camera
DOCSDIR = $(PREFIX)/share/doc/ct-camera/pipelines
SYSTEMDDIR = /lib/systemd/system/

CONFIGS = $(shell find -type f -name "*.cfg")

all : release

release : $(BINARY_RELEASE)

debug : $(BINARY_DEBUG)

.build/%.o: src/%.c
	mkdir -p $(basename $@)
	$(CC) -c $(CFLAGS_RELEASE) -o $@ $<

.build/debug/%.o: src/%.c
	mkdir -p $(basename $@)
	$(CC) -c $(CFLAGS_DEBUG) -o $@ $<

$(BINARY_RELEASE) : $(OBJ_RELEASE)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINARY_DEBUG) : $(OBJ_DEBUG)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf .build $(BINARY_RELEASE) $(BINARY_DEBUG)

install: $(BINARY_RELEASE)
	install -d $(DESTDIR)/$(BINDIR)
	install $(BINARY_RELEASE) $(DESTDIR)/$(BINDIR)
	install -d -m 777 $(DESTDIR)/$(CFGDIR)
	install -m 777 $(CONFIGS) $(DESTDIR)/$(CFGDIR)
	install -d -m 777 $(DESTDIR)/$(DOCSDIR)
	install -m 777 README.md $(DESTDIR)/$(DOCSDIR)
	mkdir -p $(DESTDIR)/$(SYSTEMDDIR)/
	install -m 644 ct-camera.service $(DESTDIR)/$(SYSTEMDDIR)/
	ln -nsf test.cfg $(DESTDIR)/$(CFGDIR)/default.cfg

uninstall:
	rm -rf $(addprefix $(BINDIR)/,$(BINARY_RELEASE))
	rm -rf $(addprefix $(CFGDIR)/,$(CONFIGS))
	rm -rf $(DOCSDIR)

-include $(DEP) $(DEP_DEBUG)

.PHONY : all release debug clean install uninstall
