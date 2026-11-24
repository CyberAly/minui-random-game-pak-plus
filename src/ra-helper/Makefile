# Build ra-helper (host, arm64, arm) with rcheevos (rc_hash) + libchdr + miniz

RCHEEVOS_TAG := v12.5.0
LIBCHDR_COMMIT := 47d794151b2b64f6aba60304e6480f4f5a2bc24b

DEPS_DIR := deps
BUILD_DIR := build
OUT_BIN_DIR := ../../bin

RCHEEVOS_DIR := $(DEPS_DIR)/rcheevos
LIBCHDR_DIR := $(DEPS_DIR)/libchdr

RCHEEVOS_SRCS := \
	$(RCHEEVOS_DIR)/src/rhash/cdreader.c \
	$(RCHEEVOS_DIR)/src/rhash/hash.c \
	$(RCHEEVOS_DIR)/src/rhash/hash_disc.c \
	$(RCHEEVOS_DIR)/src/rhash/hash_rom.c \
	$(RCHEEVOS_DIR)/src/rhash/hash_zip.c \
	$(RCHEEVOS_DIR)/src/rhash/md5.c \
	$(RCHEEVOS_DIR)/src/rc_compat.c \
	$(RCHEEVOS_DIR)/src/rc_util.c \
	$(RCHEEVOS_DIR)/src/rc_version.c

APP_SRCS := ra_helper.c chd_reader.c

COMMON_CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -DRC_HASH_NO_ENCRYPTED \
	-I$(RCHEEVOS_DIR)/include \
	-I$(LIBCHDR_DIR)/include \
	-I$(LIBCHDR_DIR)/deps/miniz-3.1.2

# Detect native host architecture so ubuntu-24.04-arm CI uses native gcc for arm64
HOST_ARCH := $(shell uname -m)
ifeq ($(HOST_ARCH),aarch64)
	ARM64_CC ?= gcc
	ARM64_AR ?= ar
	ARM64_STRIP ?= strip
else
	ARM64_CC ?= aarch64-linux-gnu-gcc
	ARM64_AR ?= aarch64-linux-gnu-ar
	ARM64_STRIP ?= aarch64-linux-gnu-strip
endif

ARM_CC ?= arm-linux-gnueabihf-gcc
ARM_AR ?= arm-linux-gnueabihf-ar
ARM_STRIP ?= arm-linux-gnueabihf-strip

HOST_CC ?= gcc

.PHONY: all deps host arm64 arm clean distclean

all: arm64 arm

deps: $(RCHEEVOS_DIR)/include/rc_hash.h $(LIBCHDR_DIR)/CMakeLists.txt

$(RCHEEVOS_DIR)/include/rc_hash.h:
	mkdir -p $(DEPS_DIR)
	rm -rf $(RCHEEVOS_DIR)
	git clone --depth 1 --branch $(RCHEEVOS_TAG) https://github.com/RetroAchievements/rcheevos.git $(RCHEEVOS_DIR)

$(LIBCHDR_DIR)/CMakeLists.txt:
	mkdir -p $(DEPS_DIR)
	rm -rf $(LIBCHDR_DIR)
	git clone https://github.com/rtissera/libchdr.git $(LIBCHDR_DIR)
	git -C $(LIBCHDR_DIR) checkout $(LIBCHDR_COMMIT)

define build_target
	@echo "[ra-helper] Building target: $(1) (CC=$(2))"
	mkdir -p $(BUILD_DIR)/$(1)/libchdr $(dir $(5))
	cmake -S $(LIBCHDR_DIR) -B $(BUILD_DIR)/$(1)/libchdr \
		-DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_C_COMPILER=$(2) \
		-DCMAKE_AR=$(shell command -v $(3)) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DWITH_SYSTEM_ZLIB=OFF \
		-DWITH_SYSTEM_ZSTD=OFF \
		-DCHDR_WANT_TESTS=OFF \
		-DMINIZ_ARCHIVE_APIS=ON \
		-DMINIZ_STDIO=ON
	cmake --build $(BUILD_DIR)/$(1)/libchdr --target chdr-static -j$$(nproc 2>/dev/null || echo 2)
	$(2) $(COMMON_CFLAGS) $(4) \
		$(APP_SRCS) $(RCHEEVOS_SRCS) \
		-Wl,--start-group \
		$$(find $(BUILD_DIR)/$(1)/libchdr -name '*.a' | sort) \
		-Wl,--end-group \
		-lm -lpthread \
		-o $(5)
	$(6) $(5) || true
	ls -lh $(5)
endef

host: deps
	$(call build_target,host,$(HOST_CC),ar,,$(BUILD_DIR)/host/ra-helper,true)

arm64: deps
	$(call build_target,arm64,$(ARM64_CC),$(ARM64_AR),-static,$(OUT_BIN_DIR)/arm64/ra-helper,$(ARM64_STRIP))

arm: deps
	$(call build_target,arm,$(ARM_CC),$(ARM_AR),-static,$(OUT_BIN_DIR)/arm/ra-helper,$(ARM_STRIP))

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(DEPS_DIR)
