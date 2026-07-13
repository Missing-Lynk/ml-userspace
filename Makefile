# Build orchestrator for the userspace components.
#
#   make            everything (daemons, gstreamer, hud)
#   make daemons    the static musl aarch64 daemons (ml-linkd, ml-ledd) -> build/
#   make linkd      just ml-linkd
#   make ledd       just ml-ledd
#   make gst        the gstreamer pipeline/HUD binaries (delegates to gstreamer/src/build.sh)
#   make hud        the LVGL HUD binary (delegates to its CMake build)
#   make clean      remove build/ (gstreamer and hud clean via their own build trees)
#
# The daemons build into build/ at the repo root. gstreamer and hud own their build
# systems and write into their own build/ trees.

REPO  := $(abspath .)
BUILD := $(REPO)/build

all: daemons gst hud

daemons: $(BUILD)/ml-linkd $(BUILD)/ml-ledd
linkd:   $(BUILD)/ml-linkd
ledd:    $(BUILD)/ml-ledd

$(BUILD):
	mkdir -p $(BUILD)

# build/<name>: static musl aarch64 binary cross-built in the Alpine 3.24 container (same
# pin as the gstreamer stack and the slot-B rootfs, so musl and versions match the runtime).
#   $(1)=name  $(2)=extra apk packages  $(3)=extra gcc flags
define daemon_rule
$(BUILD)/$(1): $(1)/$(1).c ml-shared/mlm.h | $(BUILD)
	docker run --rm --platform linux/arm64 -v $(REPO):/w -w /w \
	  alpine:3.24 sh -euc 'apk add -q build-base $(2); \
	    gcc -O2 -Wall -static $(3) -o build/$(1) $(1)/$(1).c'
	@ls -la $$@
endef
$(eval $(call daemon_rule,ml-linkd,,-pthread))
$(eval $(call daemon_rule,ml-ledd,linux-headers,))

gst:
	./gstreamer/src/build.sh

hud:
	cmake -S hud -B hud/build -DCMAKE_TOOLCHAIN_FILE=$(REPO)/hud/cmake/aarch64-static.cmake
	cmake --build hud/build -j$(shell nproc)

clean:
	rm -rf $(BUILD)

.PHONY: all daemons linkd ledd gst hud clean
