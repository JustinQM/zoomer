BUILD := build

CC      := gcc
CFLAGS  := -g -Wall -Wextra -I$(BUILD)
CFLAGS  += $(shell pkg-config --cflags wayland-client egl wayland-egl)
LDLIBS  := $(shell pkg-config --libs wayland-client egl wayland-egl gl)

WLR_PROTOCOLS_DIR     := $(shell pkg-config --variable=pkgdatadir wlr-protocols)
WAYLAND_PROTOCOLS_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

ZOOMER := $(BUILD)/zoomer

# Protocols whose generated headers zoomer.c actually includes/uses.
PROTO_USED := \
    wlr-layer-shell-unstable-v1 \
    ext-image-copy-capture \
    ext-image-capture-source \
    xdg-output

# Protocols zoomer.c never includes, but whose generated code is still required
# at LINK time: the protocols above reference interface symbols defined here.
#   - wlr-layer-shell  -> xdg_popup_interface              (xdg-shell)
#   - ext-image-capture-source -> ext_foreign_toplevel_handle_v1_interface
# Generate private-code only (no header) since we never include them.
PROTO_LINK := \
    xdg-shell \
    ext-foreign-toplevel-list

PROTO_USED_SRC := $(addprefix $(BUILD)/,$(addsuffix .c,$(PROTO_USED)))
PROTO_USED_HDR := $(addprefix $(BUILD)/,$(addsuffix .h,$(PROTO_USED)))
PROTO_LINK_SRC := $(addprefix $(BUILD)/,$(addsuffix .c,$(PROTO_LINK)))

PROTO_SRC := $(PROTO_USED_SRC) $(PROTO_LINK_SRC)

OBJ := $(BUILD)/zoomer.o $(BUILD)/glad.o $(PROTO_SRC:.c=.o)

.DEFAULT_GOAL := all
.PHONY: all clean run

all: $(ZOOMER)

run: $(ZOOMER)
	$(ZOOMER)

clean:
	$(RM) -r $(BUILD)

$(ZOOMER): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

# Our own sources (cwd) -> object files. zoomer.o additionally needs the
# generated protocol headers to exist (and rebuilds if they change).
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/zoomer.o: $(PROTO_USED_HDR)

# Generated protocol sources (already in $(BUILD)) -> object files.
$(BUILD)/%.o: $(BUILD)/%.c
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
