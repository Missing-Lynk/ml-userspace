# Build orchestrator for the userspace components.
#
#   make            everything (daemons, gstreamer + static pipeline, hud)
#   make daemons    the static musl aarch64 daemons (ml-linkd, ml-ledd) -> build/
#   make linkd      just ml-linkd
#   make ledd       just ml-ledd
#   make gst        the gstreamer pipeline/HUD binaries (delegates to gstreamer/src/build.sh)
#   make gst-static the standalone static ml-pipeline for the rootfs (build-static.sh)
#   make hud        the LVGL HUD binary (delegates to its CMake build)
#   make font       generate the BTFL OSD glyph atlas from betaflight.mcm (needs python3 + Pillow)
#   make clean      remove all build output (build/, gstreamer/build, ml-hud/build, font atlas)
#
# The daemons build into build/ at the repo root. gstreamer and hud own their build
# systems and write into their own build/ trees.

REPO  := $(abspath .)
BUILD := $(REPO)/build

all: daemons gst gst-static hud font

daemons:    $(BUILD)/ml-linkd $(BUILD)/ml-ledd $(BUILD)/ml-rf-bringup
linkd:      $(BUILD)/ml-linkd
ledd:       $(BUILD)/ml-ledd
rf-bringup: $(BUILD)/ml-rf-bringup

$(BUILD):
	mkdir -p $(BUILD)

# build/<name>: static musl aarch64 binary cross-built in the Alpine 3.24 container (same
# pin as the gstreamer stack and the slot-B rootfs, so musl and versions match the runtime).
#   $(1)=name  $(2)=extra apk packages  $(3)=extra gcc flags
define daemon_rule
$(BUILD)/$(1): $(1)/$(1).c $(wildcard $(1)/*.h) ml-shared/mlm.h | $(BUILD)
	docker run --rm --platform linux/arm64 -v $(REPO):/w -w /w \
	  alpine:3.24 sh -euc 'apk add -q build-base $(2); \
	    gcc -O2 -Wall -static $(3) -o build/$(1) $(1)/$(1).c'
	@ls -la $$@
endef
$(eval $(call daemon_rule,ml-linkd,,-pthread))
$(eval $(call daemon_rule,ml-ledd,linux-headers,))
$(eval $(call daemon_rule,ml-rf-bringup,linux-headers,))

gst:
	./gstreamer/src/build.sh

gst-static:
	./gstreamer/scripts/build-static.sh

hud:
	cmake -S ml-hud -B ml-hud/build -DCMAKE_TOOLCHAIN_FILE=$(REPO)/ml-hud/cmake/aarch64-static.cmake
	cmake --build ml-hud/build -j$(shell nproc)

font: assets/osd-fonts/font_BTFL_hd.png

assets/osd-fonts/font_BTFL_hd.png: assets/osd-fonts/betaflight.mcm assets/osd-fonts/mcm2png.py
	python3 assets/osd-fonts/mcm2png.py $< $@

clean:
	rm -rf $(BUILD) gstreamer/build ml-hud/build assets/osd-fonts/font_BTFL_hd.png

.PHONY: all daemons linkd ledd rf-bringup gst gst-static hud font clean
