PREFIX  ?= $(shell pkg-config --variable=prefix libxfce4panel-2.0 2>/dev/null || echo /usr/local)
DESTDIR ?=
LIBDIR  ?= $(shell pkg-config --variable=libdir libxfce4panel-2.0 2>/dev/null || echo $(PREFIX)/lib)
DATADIR ?= $(PREFIX)/share

CC      ?= cc
INSTALL ?= install

PLUGIN_NAME := openai-ask
PLUGIN_SO := lib$(PLUGIN_NAME).so

SRC_DIR := src
DATA_DIR := data
BUILD_DIR := build

PLUGIN_SOURCES := \
	$(SRC_DIR)/openai-ask-plugin.c \
	$(SRC_DIR)/openai-client.c \
	$(SRC_DIR)/markdown-pango.c \
	$(SRC_DIR)/keyring.c \
	$(SRC_DIR)/log.c

PLUGIN_OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(PLUGIN_SOURCES))

PKGS := gtk+-3.0 libxfce4panel-2.0 libsoup-3.0 json-glib-1.0 libsecret-1
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -fPIC
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDFLAGS ?=
LDLIBS += $(shell pkg-config --libs $(PKGS))

XFCE_PANEL_PLUGINDIR  := $(DESTDIR)$(LIBDIR)/xfce4/panel/plugins
XFCE_PANEL_DESKTOPDIR := $(DESTDIR)$(DATADIR)/xfce4/panel/plugins

.PHONY: all clean install uninstall dirs

all: $(BUILD_DIR)/$(PLUGIN_SO)

dirs:
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PLUGIN_SO): $(PLUGIN_OBJECTS)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)

install: all
	$(INSTALL) -d "$(XFCE_PANEL_PLUGINDIR)" "$(XFCE_PANEL_DESKTOPDIR)"
	$(INSTALL) -m 0755 "$(BUILD_DIR)/$(PLUGIN_SO)" "$(XFCE_PANEL_PLUGINDIR)/$(PLUGIN_SO)"
	$(INSTALL) -m 0644 "$(DATA_DIR)/$(PLUGIN_NAME).desktop.in" "$(XFCE_PANEL_DESKTOPDIR)/$(PLUGIN_NAME).desktop"

uninstall:
	rm -f "$(XFCE_PANEL_PLUGINDIR)/$(PLUGIN_SO)" "$(XFCE_PANEL_DESKTOPDIR)/$(PLUGIN_NAME).desktop"

clean:
	rm -rf "$(BUILD_DIR)"
