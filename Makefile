BUILD  := build
SRCDIR := src
CC      := gcc
CFLAGS  := -g -Wall -Wextra -I$(SRCDIR) -I. -I$(BUILD)
CFLAGS  += $(shell pkg-config --cflags wayland-client egl wayland-egl)
LDLIBS  := $(shell pkg-config --libs wayland-client egl wayland-egl gl wayland-cursor)

WLR_PROTOCOLS_DIR     := $(shell pkg-config --variable=pkgdatadir wlr-protocols)
WAYLAND_PROTOCOLS_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

ZOOMER := $(BUILD)/zoomer

# Every translation unit lives in $(SRCDIR) (zoomer.c, core_wayland.c, capture.c,
# capture_wlr.c, glad.c). Compile everything in there.
OWN_SRC := $(wildcard $(SRCDIR)/*.c)
OWN_OBJ := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(OWN_SRC))

# Protocols whose generated headers our sources actually include/use.
PROTO_USED := \
    wlr-layer-shell-unstable-v1 \
    ext-image-copy-capture \
    ext-image-capture-source \
    xdg-output

# Protocols our sources never include, but whose generated code is still required
# at LINK time: the protocols above reference interface symbols defined here.
#   - wlr-layer-shell          -> xdg_popup_interface              (xdg-shell)
#   - ext-image-capture-source -> ext_foreign_toplevel_handle_v1_interface
# Generate private-code only (no header) since we never include them.
PROTO_LINK := \
    xdg-shell \
    ext-foreign-toplevel-list

PROTO_USED_SRC := $(addprefix $(BUILD)/,$(addsuffix .c,$(PROTO_USED)))
PROTO_USED_HDR := $(addprefix $(BUILD)/,$(addsuffix .h,$(PROTO_USED)))
PROTO_LINK_SRC := $(addprefix $(BUILD)/,$(addsuffix .c,$(PROTO_LINK)))
PROTO_SRC := $(PROTO_USED_SRC) $(PROTO_LINK_SRC)

OBJ := $(OWN_OBJ) $(PROTO_SRC:.c=.o)

.DEFAULT_GOAL := all
.PHONY: all clean run install

all: $(ZOOMER)

run: $(ZOOMER)
	$(ZOOMER)

clean:
	$(RM) -r $(BUILD)

install: $(ZOOMER)
	cp $(ZOOMER) /usr/local/bin/zoomer

$(ZOOMER): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

# Our own sources ($(SRCDIR)) -> object files. They additionally need the
# generated protocol headers to exist (and rebuild if those headers change).
$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(OWN_OBJ): $(PROTO_USED_HDR)

# Generated protocol sources (already in $(BUILD)) -> object files.
$(BUILD)/%.o: $(BUILD)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Per-protocol codegen. Source XML paths are irregular (stable/unstable/staging,
# with or without -v1 suffixes), so each protocol gets its own rule. Grouped
# targets (&:) run the recipe once and produce both outputs (GNU make >= 4.3).
$(BUILD)/wlr-layer-shell-unstable-v1.c $(BUILD)/wlr-layer-shell-unstable-v1.h &: \
		$(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml | $(BUILD)
	wayland-scanner private-code  $< $(BUILD)/wlr-layer-shell-unstable-v1.c
	wayland-scanner client-header $< $(BUILD)/wlr-layer-shell-unstable-v1.h

$(BUILD)/ext-image-copy-capture.c $(BUILD)/ext-image-copy-capture.h &: \
		$(WAYLAND_PROTOCOLS_DIR)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml | $(BUILD)
	wayland-scanner private-code  $< $(BUILD)/ext-image-copy-capture.c
	wayland-scanner client-header $< $(BUILD)/ext-image-copy-capture.h

$(BUILD)/ext-image-capture-source.c $(BUILD)/ext-image-capture-source.h &: \
		$(WAYLAND_PROTOCOLS_DIR)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml | $(BUILD)
	wayland-scanner private-code  $< $(BUILD)/ext-image-capture-source.c
	wayland-scanner client-header $< $(BUILD)/ext-image-capture-source.h

$(BUILD)/xdg-output.c $(BUILD)/xdg-output.h &: \
		$(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-output/xdg-output-unstable-v1.xml | $(BUILD)
	wayland-scanner private-code  $< $(BUILD)/xdg-output.c
	wayland-scanner client-header $< $(BUILD)/xdg-output.h

# Link-only protocols: private-code only, no header.
$(BUILD)/xdg-shell.c: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml | $(BUILD)
	wayland-scanner private-code $< $@

$(BUILD)/ext-foreign-toplevel-list.c: $(WAYLAND_PROTOCOLS_DIR)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml | $(BUILD)
	wayland-scanner private-code $< $@

$(BUILD):
	mkdir -p $(BUILD)
