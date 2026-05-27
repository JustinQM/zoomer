BUILD=./build
WAYLAND_CFLAGS=$(shell pkg-config --cflags wayland-client)
CFLAGS=-g -Wall -Wextra $(WAYLAND_CFLAGS) -I$(BUILD)
LDLIBS=$(shell pkg-config --libs wayland-client)
ZOOMER=$(BUILD)/zoomer

WLR_PROTOCOLS_DIR=$(shell pkg-config --variable=pkgdatadir wlr-protocols)
WL_LAYER_SHELL_UNSTABLE=wlr-layer-shell-unstable-v1
WL_XDG_SHELL=xdg-shell

WAYLAND_PROTOCOL_DIR=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WL_IMAGE_COPY_CAPTURE=ext-image-copy-capture
WL_IMAGE_CAPTURE_SOURCE=ext-image-capture-source
WL_FOREIGN_TOPLEVEL=ext-foreign-toplevel-list

run: $(ZOOMER)
	$(BUILD)/zoomer

$(ZOOMER): zoomer.c WAYLAND	
	gcc zoomer.c $(BUILD)/$(WL_LAYER_SHELL_UNSTABLE).c $(BUILD)/$(WL_IMAGE_COPY_CAPTURE).c $(BUILD)/$(WL_XDG_SHELL).c $(BUILD)/$(WL_IMAGE_CAPTURE_SOURCE).c $(BUILD)/$(WL_FOREIGN_TOPLEVEL).c -o $(ZOOMER) $(CFLAGS) $(LDLIBS)

WAYLAND: $(WL_LAYER_SHELL_UNSTABLE) $(WL_IMAGE_COPY_CAPTURE) $(WL_XDG_SHELL) $(WL_IMAGE_CAPTURE_SOURCE) $(WL_FOREIGN_TOPLEVEL)
	echo "finished wayland setup"

$(WL_LAYER_SHELL_UNSTABLE): $(BUILD)
	wayland-scanner private-code $(WLR_PROTOCOLS_DIR)/unstable/$(WL_LAYER_SHELL_UNSTABLE).xml $(BUILD)/$(WL_LAYER_SHELL_UNSTABLE).c
	wayland-scanner client-header $(WLR_PROTOCOLS_DIR)/unstable/$(WL_LAYER_SHELL_UNSTABLE).xml $(BUILD)/$(WL_LAYER_SHELL_UNSTABLE).h

$(WL_XDG_SHELL): $(BUILD)
	wayland-scanner private-code $(WAYLAND_PROTOCOL_DIR)/stable/$(WL_XDG_SHELL)/$(WL_XDG_SHELL).xml $(BUILD)/$(WL_XDG_SHELL).c
	wayland-scanner client-header $(WAYLAND_PROTOCOL_DIR)/stable/$(WL_XDG_SHELL)/$(WL_XDG_SHELL).xml $(BUILD)/$(WL_XDG_SHELL).h

$(WL_IMAGE_COPY_CAPTURE): $(BUILD)
	wayland-scanner private-code $(WAYLAND_PROTOCOL_DIR)/staging/$(WL_IMAGE_COPY_CAPTURE)/$(WL_IMAGE_COPY_CAPTURE)-v1.xml $(BUILD)/$(WL_IMAGE_COPY_CAPTURE).c
	wayland-scanner client-header $(WAYLAND_PROTOCOL_DIR)/staging/$(WL_IMAGE_COPY_CAPTURE)/$(WL_IMAGE_COPY_CAPTURE)-v1.xml $(BUILD)/$(WL_IMAGE_COPY_CAPTURE).h

$(WL_IMAGE_CAPTURE_SOURCE): $(BUILD)
	wayland-scanner private-code $(WAYLAND_PROTOCOL_DIR)/staging/$(WL_IMAGE_CAPTURE_SOURCE)/$(WL_IMAGE_CAPTURE_SOURCE)-v1.xml $(BUILD)/$(WL_IMAGE_CAPTURE_SOURCE).c
	wayland-scanner client-header $(WAYLAND_PROTOCOL_DIR)/staging/$(WL_IMAGE_CAPTURE_SOURCE)/$(WL_IMAGE_CAPTURE_SOURCE)-v1.xml $(BUILD)/$(WL_IMAGE_CAPTURE_SOURCE).h

$(WL_FOREIGN_TOPLEVEL): $(BUILD)
	wayland-scanner private-code $(WAYLAND_PROTOCOL_DIR)/staging/$(WL_FOREIGN_TOPLEVEL)/$(WL_FOREIGN_TOPLEVEL)-v1.xml $(BUILD)/$(WL_FOREIGN_TOPLEVEL).c
	wayland-scanner client-header $(WAYLAND_PROTOCOL_DIR)/staging/$(WL_FOREIGN_TOPLEVEL)/$(WL_FOREIGN_TOPLEVEL)-v1.xml $(BUILD)/$(WL_FOREIGN_TOPLEVEL).h


$(BUILD):
	mkdir -p $(BUILD)
