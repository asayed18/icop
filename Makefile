CMAKE ?= cmake
CTEST ?= ctest
POWERSHELL ?= powershell.exe
SH ?= sh
RELEASE_ROOT ?= releases
GENERATOR ?= Ninja
JOBS ?= 8
VERSION ?=
VLC_ROOT ?=
CONFIGURE_ARGS ?=
INSTALL_ARGS ?=

ifeq ($(OS),Windows_NT)
BUILD_DIR ?= build-ninja
else
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
BUILD_DIR ?= build-mac
else ifneq ($(wildcard /proc/sys/fs/binfmt_misc/WSLInterop),)
BUILD_DIR ?= build-wsl
else
BUILD_DIR ?= build-linux
endif
endif

VERSION_CONFIGURE_ARG = $(if $(strip $(VERSION)),-DICOP_RELEASE_VERSION=$(VERSION),)
VERSION_INSTALL_ARG = $(if $(strip $(VERSION)),-Version "$(VERSION)",)
VLC_ROOT_INSTALL_ARG = $(if $(strip $(VLC_ROOT)),-VlcRoot "$(VLC_ROOT)",)

ifeq ($(OS),Windows_NT)
INSTALL_COMMAND = "$(POWERSHELL)" -NoProfile -ExecutionPolicy Bypass -File "tools/install_icop_plugin.ps1" -ReleaseRoot "$(RELEASE_ROOT)" $(VERSION_INSTALL_ARG) $(VLC_ROOT_INSTALL_ARG) $(INSTALL_ARGS)
else
VERSION_INSTALL_ARG_POSIX = $(if $(strip $(VERSION)),--version "$(VERSION)",)
VLC_ROOT_INSTALL_ARG_POSIX = $(if $(strip $(VLC_ROOT)),--vlc-root "$(VLC_ROOT)",)
INSTALL_COMMAND = "$(SH)" "tools/install_icop_plugin.sh" --release-root "$(RELEASE_ROOT)" $(VERSION_INSTALL_ARG_POSIX) $(VLC_ROOT_INSTALL_ARG_POSIX) $(INSTALL_ARGS)
endif

.PHONY: all help configure build test benchmark release package install_plugin install-plugin

all: build

help:
	@echo icop build targets
	@echo   make build          Configure and build the VLC plugin and detector core
	@echo   make test           Build and run the test suite
	@echo   make benchmark      Build the benchmark executable
	@echo   make release        Build the versioned host-platform package
	@echo   make install_plugin Build the release and install it into VLC
	@echo Common overrides: BUILD_DIR, JOBS, VERSION, VLC_ROOT, CONFIGURE_ARGS, INSTALL_ARGS

configure:
	@if [ -f "$(BUILD_DIR)/CMakeCache.txt" ] && grep -q CMAKE_GENERATOR "$(BUILD_DIR)/CMakeCache.txt"; then \
		echo "Using existing build directory: $(BUILD_DIR)"; \
	else \
		"$(CMAKE)" -S . -B "$(BUILD_DIR)" -G "$(GENERATOR)" $(VERSION_CONFIGURE_ARG) $(CONFIGURE_ARGS); \
	fi

build: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --target icop_plugin icop_core -j "$(JOBS)"

test: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --target icop_plugin icop_core icop_test -j "$(JOBS)"
	"$(CTEST)" --test-dir "$(BUILD_DIR)" --output-on-failure

benchmark: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --target icop_benchmark -j "$(JOBS)"

release: configure
	"$(CMAKE)" --build "$(BUILD_DIR)" --target icop_package -j "$(JOBS)"

package: release

VLC_PLUGIN_DIR ?= $(shell sh tools/detect_vlc_plugin_dir.sh 2>/dev/null)

install_plugin: release
	$(INSTALL_COMMAND)
	$(if $(VLC_PLUGIN_DIR),\
		$(info Regenerating VLC plugin cache...); \
		VLC_CACHE_GEN=$$(command -v vlc-cache-gen 2>/dev/null || true); \
		if [ -n "$$VLC_CACHE_GEN" ]; then \
			if [ -w "$(VLC_PLUGIN_DIR)" ]; then \
				"$$VLC_CACHE_GEN" "$(VLC_PLUGIN_DIR)"; \
			else \
				pkexec "$$VLC_CACHE_GEN" "$(VLC_PLUGIN_DIR)"; \
			fi; \
		fi,)

install-plugin: install_plugin
