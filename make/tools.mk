###############################################################
#
# Installers for tools required by the ground and flight builds
#
# NOTE: These are not tied to the default goals
#       and must be invoked manually
#
###############################################################

##############################
#
# Check that environmental variables are sane
#
##############################
# Checking for $(OPENOCD_FTDI) to be sane
ifdef OPENOCD_FTDI
  ifneq ($(OPENOCD_FTDI),yes)
    ifneq ($(OPENOCD_FTDI),no)
      $(error Only yes or no are allowed for OPENOCD_FTDI)
    endif
  endif
endif

# Qt 6 is required. Use an explicit QMAKE and a separate BUILD_DIR for a different SDK.
QT_VERSION ?= 6.12.0

# These bits are not user serviceable
QT_VERSION_LONG := $(word 1, $(subst -, , $(QT_VERSION)))
ifneq ($(words $(subst ., , $(QT_VERSION_LONG))),3)
  $(error Invalid Qt version!)
endif
QT_VERSION_SUFFIX := $(word 2, $(subst -, ,$(QT_VERSION)))
ifneq ($(subst _, ,$(QT_VERSION_SUFFIX)), 1)
  QT_VERSION_EXTRA := $(word 2, $(subst _, ,$(QT_VERSION_SUFFIX)))
  QT_VERSION_SUFFIX := $(word 1, $(subst _, ,$(QT_VERSION_SUFFIX)))
endif
QT_VERSION_FULL := $(QT_VERSION_LONG)
ifneq ($(strip $(QT_VERSION_SUFFIX)),)
  QT_VERSION_FULL := $(QT_VERSION_FULL)-$(QT_VERSION_SUFFIX)
endif
ifneq ($(strip $(QT_VERSION_EXTRA)),)
  QT_VERSION_EXTRA := _$(strip $(QT_VERSION_EXTRA))
else
  QT_VERSION_EXTRA :=
endif
QT_VERSION_SHORT := $(word 1, $(subst ., , $(QT_VERSION_FULL))).$(word 2, $(subst ., , $(QT_VERSION_FULL)))
ifeq ($(words $(filter alpha beta rc, $(QT_VERSION_SUFFIX))), 1)
  QT_VERSION_SOURCE := development
else
  QT_VERSION_SOURCE := official
endif
QT_PKG_VERSION := $(subst .,,$(QT_VERSION_LONG))
QT_SDK_DIR := $(TOOLS_DIR)/Qt$(QT_VERSION_FULL)


.PHONY: tools_required_qt
tools_required_qt:
	$(V1) qt_selected_version="$$("$(QMAKE)" -query QT_VERSION)" && \
	  case "$$qt_selected_version" in \
	    6.*) qt_selected_minor="$${qt_selected_version#6.}"; [ "$${qt_selected_minor%%.*}" -ge 10 ] ;; \
	    *) false ;; \
	  esac || { echo "Select Qt 6.10+ using QMAKE; release target is Qt 6.12.0" >&2; exit 1; }; \
	  qt_selected_qmake="$$(command -v "$(QMAKE)")"; \
	  for qt_build_makefile in "$(BUILD_DIR)/ground/gcs/Makefile" "$(BUILD_DIR)/ground/uavobjgenerator/Makefile"; do \
	    [ -f "$$qt_build_makefile" ] || continue; \
	    qt_build_version="$$(sed -n 's/^# Generated .* (Qt \(.*\))$$/\1/p' "$$qt_build_makefile")"; \
	    qt_build_qmake="$$(sed -n 's/^QMAKE[[:space:]]*=[[:space:]]*//p' "$$qt_build_makefile")"; \
	    if [ "$$qt_build_version" != "$$qt_selected_version" ] || [ ! "$$qt_build_qmake" -ef "$$qt_selected_qmake" ]; then \
	      echo "Existing build uses Qt $$qt_build_version ($$qt_build_qmake): $$qt_build_makefile" >&2; \
	      echo "Select a separate BUILD_DIR for Qt $$qt_selected_version ($$qt_selected_qmake)." >&2; \
	      exit 1; \
	    fi; \
	  done

# Set variables for Android SDK
ifdef LINUX
  ANDROID_SDK_DIR := $(TOOLS_DIR)/android-sdk-linux
  ANDROID_SDK_URL := http://dl.google.com/android/android-sdk_r23.0.2-linux.tgz
endif

# Build openocd without FTDI (yes | no)
OPENOCD_FTDI ?= yes


# $1 URL
# $2 save dir
# $3 save filename
define DOWNLOAD_template
  $(V1) (\
    if [ -e "$(2)/$(3)" ]; then \
      curl -z "$(2)/$(3)" -L -k -o "$(2)/$(3)" "$(1)"; \
    else \
      curl -L -k -o "$(2)/$(3)" "$(1)"; \
    fi;\
  )
endef


.PHONY: qt_sdk_install

# Use distribution packages or an independently installed SDK.
ifneq (,$(filter qt_sdk_install,$(MAKECMDGOALS)))
  $(error The legacy qt_sdk_install recipe is retired. Install Qt 6.10+ development packages or a separate SDK, then set QMAKE to its qmake executable)
endif

QT_SDK_QMAKE_PATH := $(QT_SDK_DIR)/$(QT_VERSION_FULL)/gcc_64/bin/qmake

.PHONY: qt_sdk_clean
qt_sdk_clean:
	$(V1) [ ! -d "$(QT_SDK_DIR)" ] || $(RM) -rf $(QT_SDK_DIR)

# Set up ARM (STM32) SDK
ARM_SDK_DIR := $(TOOLS_DIR)/gcc-arm-none-eabi-6-2017-q2-update

.PHONY: arm_sdk_install
ifdef LINUX
  arm_sdk_install: ARM_SDK_URL  := https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu-rm/6-2017q2/gcc-arm-none-eabi-6-2017-q2-update-linux.tar.bz2
endif

arm_sdk_install: ARM_SDK_FILE := $(notdir $(ARM_SDK_URL))
# order-only prereq on directory existance:
arm_sdk_install: | $(DL_DIR) $(TOOLS_DIR)
        # Pinned upstream archive, verified before extraction; preserve a working SDK on download failure.
	$(V1) test -f "$(DL_DIR)/$(ARM_SDK_FILE)" || curl -fL --retry 2 -o "$(DL_DIR)/$(ARM_SDK_FILE)" "$(ARM_SDK_URL)"
	$(V1) echo "e68e4b2fe348ecb567c27985355dff75b65319a0f6595d44a18a8c5e05887cc3  $(DL_DIR)/$(ARM_SDK_FILE)" | sha256sum -c -
	$(V1) tar -C $(TOOLS_DIR) -xjf "$(DL_DIR)/$(ARM_SDK_FILE)"

.PHONY: arm_sdk_clean
arm_sdk_clean:
	$(V1) [ ! -d "$(ARM_SDK_DIR)" ] || $(RM) -r $(ARM_SDK_DIR)

# Set up openocd tools
OPENOCD_DIR       := $(TOOLS_DIR)/openocd
OPENOCD_BUILD_DIR := $(DL_DIR)/openocd-build

.PHONY: openocd_install

openocd_install: | $(DL_DIR) $(TOOLS_DIR)
openocd_install: OPENOCD_URL     := git://git.code.sf.net/p/openocd/code
openocd_install: OPENOCD_TAG     := v0.9.0
openocd_install: OPENOCD_OPTIONS := --enable-maintainer-mode --prefix="$(OPENOCD_DIR)" --enable-buspirate --enable-stlink

ifeq ($(OPENOCD_FTDI), yes)
openocd_install: OPENOCD_OPTIONS := $(OPENOCD_OPTIONS) --enable-ftdi
endif

openocd_install: openocd_clean
        # download the source
	$(V0) @echo " DOWNLOAD     $(OPENOCD_URL) @ $(OPENOCD_TAG)"
	$(V1) [ ! -d "$(OPENOCD_BUILD_DIR)" ] || $(RM) -rf "$(OPENOCD_BUILD_DIR)"
	$(V1) mkdir -p "$(OPENOCD_BUILD_DIR)"
	$(V1) git clone --no-checkout $(OPENOCD_URL) "$(OPENOCD_BUILD_DIR)"
	$(V1) ( \
	  cd $(OPENOCD_BUILD_DIR) ; \
	  git checkout -q tags/$(OPENOCD_TAG) ; \
	)

        # build and install
	$(V0) @echo " BUILD        $(OPENOCD_DIR)"
	$(V1) mkdir -p "$(OPENOCD_DIR)"
	$(V1) ( \
	  cd $(OPENOCD_BUILD_DIR) ; \
	  ./bootstrap ; \
	  ./configure  $(OPENOCD_OPTIONS) ; \
	  $(MAKE) ; \
	  $(MAKE) install ; \
	)

        # delete the extracted source when we're done
	$(V1) [ ! -d "$(OPENOCD_BUILD_DIR)" ] || $(RM) -rf "$(OPENOCD_BUILD_DIR)"

.PHONY: openocd_clean
openocd_clean:
	$(V0) @echo " CLEAN        $(OPENOCD_DIR)"
	$(V1) [ ! -d "$(OPENOCD_DIR)" ] || $(RM) -r "$(OPENOCD_DIR)"

STM32FLASH_DIR := $(TOOLS_DIR)/stm32flash

.PHONY: stm32flash_install
stm32flash_install: STM32FLASH_URL := http://stm32flash.googlecode.com/svn/trunk
stm32flash_install: STM32FLASH_REV := 61
stm32flash_install: stm32flash_clean
        # download the source
	$(V0) @echo " DOWNLOAD     $(STM32FLASH_URL) @ r$(STM32FLASH_REV)"
	$(V1) svn export -q -r "$(STM32FLASH_REV)" "$(STM32FLASH_URL)" "$(STM32FLASH_DIR)"

        # build
	$(V0) @echo " BUILD        $(STM32FLASH_DIR)"
	$(V1) $(MAKE) --silent -C $(STM32FLASH_DIR) all

.PHONY: stm32flash_clean
stm32flash_clean:
	$(V0) @echo " CLEAN        $(STM32FLASH_DIR)"
	$(V1) [ ! -d "$(STM32FLASH_DIR)" ] || $(RM) -r "$(STM32FLASH_DIR)"

DFUUTIL_DIR := $(TOOLS_DIR)/dfu-util

.PHONY: dfuutil_install
dfuutil_install: DFUUTIL_URL  := http://dfu-util.sourceforge.net/releases/dfu-util-0.8.tar.gz
dfuutil_install: DFUUTIL_FILE := $(notdir $(DFUUTIL_URL))
dfuutil_install: | $(DL_DIR) $(TOOLS_DIR)
dfuutil_install: dfuutil_clean
        # download the source
	$(V0) @echo " DOWNLOAD     $(DFUUTIL_URL)"
	$(V1) wget -N -P "$(DL_DIR)" "$(DFUUTIL_URL)"

        # extract the source
	$(V0) @echo " EXTRACT      $(DFUUTIL_FILE)"
	$(V1) [ ! -d "$(DL_DIR)/dfuutil-build" ] || $(RM) -r "$(DL_DIR)/dfuutil-build"
	$(V1) mkdir -p "$(DL_DIR)/dfuutil-build"
	$(V1) tar -C $(DL_DIR)/dfuutil-build -xf "$(DL_DIR)/$(DFUUTIL_FILE)"

        # build
	$(V0) @echo " BUILD        $(DFUUTIL_DIR)"
	$(V1) mkdir -p "$(DFUUTIL_DIR)"
	$(V1) ( \
	  cd $(DL_DIR)/dfuutil-build/dfu-util-0.8 ; \
	  ./configure --prefix="$(DFUUTIL_DIR)" ; \
	  $(MAKE) ; \
	  $(MAKE) install ; \
	)

.PHONY: dfuutil_clean
dfuutil_clean:
	$(V0) @echo " CLEAN        $(DFUUTIL_DIR)"
	$(V1) [ ! -d "$(DFUUTIL_DIR)" ] || $(RM) -r "$(DFUUTIL_DIR)"

# see http://developer.android.com/sdk/ for latest versions
.PHONY: android_sdk_install
android_sdk_install: ANDROID_SDK_FILE := $(notdir $(ANDROID_SDK_URL))
# order-only prereq on directory existance:
android_sdk_install: | $(DL_DIR) $(TOOLS_DIR)
android_sdk_install: android_sdk_clean
        # download the source only if it's newer than what we already have
	$(V0) @echo " DOWNLOAD     $(ANDROID_SDK_URL)"
	$(V1) wget --no-check-certificate -N -P "$(DL_DIR)" "$(ANDROID_SDK_URL)"

        # binary only release so just extract it
	$(V0) @echo " EXTRACT      $(ANDROID_SDK_FILE)"
	$(V1) tar -C $(TOOLS_DIR) -xf "$(DL_DIR)/$(ANDROID_SDK_FILE)"

.PHONY: android_sdk_clean
android_sdk_clean:
	$(V0) @echo " CLEAN        $(ANDROID_SDK_DIR)"
	$(V1) [ ! -d "$(ANDROID_SDK_DIR)" ] || $(RM) -r $(ANDROID_SDK_DIR)

.PHONY: android_sdk_update
android_sdk_update:
	$(V0) @echo " UPDATE       $(ANDROID_SDK_DIR)"
	$(ANDROID_SDK_DIR)/tools/android update sdk --no-ui --all -t platform-tools,build-tools-20.0.0,android-19,addon-google_apis-google-19

# Set up Google Test (gtest) tools
GTEST_DIR       := $(TOOLS_DIR)/googletest-release-1.8.0

.PHONY: gtest_install
gtest_install: | $(DL_DIR) $(TOOLS_DIR)
gtest_install: GTEST_URL  := https://github.com/google/googletest/archive/release-1.8.0.zip
gtest_install: GTEST_FILE := $(notdir $(GTEST_URL))
gtest_install: gtest_clean
        # download the file unconditionally since google code gives back 404
        # for HTTP HEAD requests which are used when using the wget -N option
	$(V1) [ ! -f "$(DL_DIR)/$(GTEST_FILE)" ] || $(RM) -f "$(DL_DIR)/$(GTEST_FILE)"
	$(V1) wget -P "$(DL_DIR)" "$(GTEST_URL)"

        # extract the source
	$(V1) [ ! -d "$(GTEST_DIR)" ] || $(RM) -rf "$(GTEST_DIR)"
	$(V1) mkdir -p "$(GTEST_DIR)"
	$(V1) unzip -q -d "$(TOOLS_DIR)" "$(DL_DIR)/$(GTEST_FILE)"
	$(V1) mv $(GTEST_DIR)/googletest/* $(GTEST_DIR)

.PHONY: gtest_clean
gtest_clean:
	$(V0) @echo " CLEAN        $(GTEST_DIR)"
	$(V1) [ ! -d "$(GTEST_DIR)" ] || $(RM) -rf "$(GTEST_DIR)"

# Set up uncrustify tools
UNCRUSTIFY_DIR := $(TOOLS_DIR)/uncrustify-0.61
UNCRUSTIFY_BUILD_DIR := $(DL_DIR)/uncrustify

.PHONY: uncrustify_install
uncrustify_install: | $(DL_DIR) $(TOOLS_DIR)
uncrustify_install: UNCRUSTIFY_URL := http://downloads.sourceforge.net/project/uncrustify/uncrustify/uncrustify-0.61/uncrustify-0.61.tar.gz
uncrustify_install: UNCRUSTIFY_FILE := uncrustify-0.61.tar.gz
uncrustify_install: UNCRUSTIFY_OPTIONS := prefix=$(UNCRUSTIFY_DIR)
uncrustify_install: uncrustify_clean
	$(V0) @echo " DOWNLOAD     $(UNCRUSTIFY_URL)"
	$(V1) wget --no-check-certificate -N -P "$(DL_DIR)" "$(UNCRUSTIFY_URL)"
        # extract the src
	$(V0) @echo " EXTRACT      $(UNCRUSTIFY_FILE)"
	$(V1) tar -C $(TOOLS_DIR) -xf "$(DL_DIR)/$(UNCRUSTIFY_FILE)"

	$(V0) @echo " BUILD        $(UNCRUSTIFY_DIR)"
	$(V1) ( \
	  cd $(UNCRUSTIFY_DIR) ; \
	  ./configure --prefix="$(UNCRUSTIFY_DIR)" ; \
	  $(MAKE) ; \
	  $(MAKE) install ; \
	)
	      # delete the extracted source when we're done
	$(V1) [ ! -d "$(UNCRUSTIFY_BUILD_DIR)" ] || $(RM) -r "$(UNCRUSTIFY_BUILD_DIR)"

.PHONY: uncrustify_clean
uncrustify_clean:
	$(V0) @echo " CLEAN        $(UNCRUSTIFY_DIR)"
	$(V1) [ ! -d "$(UNCRUSTIFY_DIR)" ] || $(RM) -r "$(UNCRUSTIFY_DIR)"
	$(V0) @echo " CLEAN        $(UNCRUSTIFY_BUILD_DIR)"
	$(V1) [ ! -d "$(UNCRUSTIFY_BUILD_DIR)" ] || $(RM) -r "$(UNCRUSTIFY_BUILD_DIR)"

# Set up libkml

.PHONY: libkml_install
libkml_install: | $(DL_DIR) $(TOOLS_DIR)
libkml_install: LIBKML_URL := https://github.com/kubark42/libkml.git
libkml_install: LIBKML_REV  := e4e1c525363b8b1bbe5d6683827a07e7c252b815
libkml_install: LIBKML_INSTALL_DIR := $(TOOLS_DIR)/libkml
libkml_install: LIBKML_BUILD_DIR := $(DL_DIR)/libkml-build
libkml_install: libkml_clean
        # download the source
	$(V0) @echo " DOWNLOAD     $(LIBKML_URL) @ $(LIBKML_REV)"
	$(V1) [ ! -d "$(LIBKML_BUILD_DIR)" ] || $(RM) -rf "$(LIBKML_BUILD_DIR)"
	$(V1) mkdir -p "$(LIBKML_BUILD_DIR)"
	$(V1) git clone --no-checkout $(LIBKML_URL) "$(LIBKML_BUILD_DIR)"
	$(V1) ( \
	  cd $(LIBKML_BUILD_DIR) ; \
	  git checkout -q $(LIBKML_REV) ; \
	)

        # build and install
	$(V0) @echo " BUILD        $(LIBKML_INSTALL_DIR)"
	$(V1) mkdir -p "$(LIBKML_BUILD_DIR)/build"
	$(V1) ( \
	  cd $(LIBKML_BUILD_DIR) ; \
	  ./autogen.sh ; \
	  cd $(LIBKML_BUILD_DIR)/build ; \
	  ../configure --prefix="$(LIBKML_INSTALL_DIR)"; \
	  $(MAKE) ; \
	  $(MAKE) install ; \
	)

        # delete the extracted source when we're done
	$(V1) [ ! -d "$(LIBKML_BUILD_DIR)" ] || $(RM) -rf "$(LIBKML_BUILD_DIR)"

.PHONY: libkml_clean
libkml_clean:
	$(V0) @echo " CLEAN        $(LIBKML_INSTALL_DIR)"
	$(V1) [ ! -d "$(LIBKML_INSTALL_DIR)" ] || $(RM) -rf "$(LIBKML_INSTALL_DIR)"
	$(V0) @echo " CLEAN        $(LIBKML_BUILD_DIR)"
	$(V1) [ ! -d "$(LIBKML_BUILD_DIR)" ] || $(RM) -rf "$(LIBKML_BUILD_DIR)"

# ZIP download URL
zip_install: ZIP_URL  := http://pkgs.fedoraproject.org/repo/pkgs/zip/zip30.tar.gz/7b74551e63f8ee6aab6fbc86676c0d37/zip30.tar.gz

zip_install: ZIP_FILE := $(notdir $(ZIP_URL))

ZIP_DIR = $(TOOLS_DIR)/zip30

# order-only prereq on directory existance:
zip_install : | $(DL_DIR) $(TOOLS_DIR)
zip_install: zip_clean
	$(V1) curl -L -k -o "$(DL_DIR)/$(ZIP_FILE)" "$(ZIP_URL)"
	$(V1) tar --force-local -C $(TOOLS_DIR) -xzf "$(DL_DIR)/$(ZIP_FILE)"
	$(V1) cd "$(ZIP_DIR)" && $(MAKE) -f unix/Makefile generic_gcc

.PHONY: zip_clean
zip_clean:
	$(V1) [ ! -d "$(ZIP_DIR)" ] || $(RM) -rf $(ZIP_DIR)


# Pinned Qt 6 graphics library.
# Must match ground/gcs/src/libs/glc_lib/qt6.pri.
GLC6_REV := b292ccf7851576d71ee9bed7ced4c340b910159a
GLC6_REPO := https://github.com/laumaya/GLC_lib.git
GLC6_SOURCE_DIR := $(DL_DIR)/glc-qt6

.PHONY: glc6_download tools_required_glc6
glc6_download: | $(DL_DIR)
	$(V0) @echo " DOWNLOAD     $(GLC6_REPO) @ $(GLC6_REV)"
	$(V1) mkdir -p "$(GLC6_SOURCE_DIR)"
	$(V1) ( \
	  cd "$(GLC6_SOURCE_DIR)" && \
	  git init -q && \
	  (git cat-file -e "$(GLC6_REV)^{commit}" 2>/dev/null || git fetch --depth=1 "$(GLC6_REPO)" "$(GLC6_REV)") && \
	  git checkout -q --detach "$(GLC6_REV)" && \
	  test -f src/lib/lib.pro \
	)

tools_required_glc6:
	$(V1) test -f "$(GLC6_SOURCE_DIR)/src/lib/lib.pro" && \
	  test "$$(git -C "$(GLC6_SOURCE_DIR)" rev-parse HEAD 2>/dev/null)" = "$(GLC6_REV)" || \
	  { echo "Pinned Qt 6 GLC missing; run make glc6_download" >&2; exit 1; }

# Google breakpad
# Must match ground/tools.pri
BREAKPAD_REV := 6598c9c33fc02da7805401f3b0f1a733e6a24071
BREAKPAD_REPO := https://github.com/google/breakpad.git
BREAKPAD_LSS_REV := 29164a80da4d41134950d76d55199ea33fbb9613
BREAKPAD_LSS_REPO := https://chromium.googlesource.com/linux-syscall-support
BREAKPAD_DIR := $(TOOLS_DIR)/breakpad/$(BREAKPAD_REV)
BREAKPAD_SOURCE_DIR := $(DL_DIR)/breakpad-linux
BREAKPAD_BUILD_DIR := $(BUILD_DIR)/breakpad/$(BREAKPAD_REV)

.PHONY: breakpad_install breakpad_clean breakpad_test tools_required_breakpad

breakpad_install: | $(DL_DIR) $(TOOLS_DIR)
	$(V0) @echo " DOWNLOAD     $(BREAKPAD_REPO) @ $(BREAKPAD_REV)"
	$(V1) mkdir -p "$(BREAKPAD_SOURCE_DIR)"
	$(V1) ( \
	  cd "$(BREAKPAD_SOURCE_DIR)" && \
	  git init -q && \
	  (git cat-file -e "$(BREAKPAD_REV)^{commit}" 2>/dev/null || git fetch --depth=1 "$(BREAKPAD_REPO)" "$(BREAKPAD_REV)") && \
	  git checkout -q --detach "$(BREAKPAD_REV)" && \
	  mkdir -p src/third_party/lss && \
	  cd src/third_party/lss && \
	  git init -q && \
	  (git cat-file -e "$(BREAKPAD_LSS_REV)^{commit}" 2>/dev/null || git fetch --depth=1 "$(BREAKPAD_LSS_REPO)" "$(BREAKPAD_LSS_REV)") && \
	  git checkout -q --detach "$(BREAKPAD_LSS_REV)" \
	)

	$(V0) @echo " BUILDING     $(BREAKPAD_REPO) @ $(BREAKPAD_REV)"
	$(V1) mkdir -p "$(BREAKPAD_BUILD_DIR)"
	$(V1) ( \
	  cd "$(BREAKPAD_BUILD_DIR)" && \
	  "$(BREAKPAD_SOURCE_DIR)/configure" --prefix="$(BREAKPAD_DIR)" --disable-selftest CXXFLAGS="-O2 -g -fPIC" CFLAGS="-O2 -g -fPIC" && \
	  $(MAKE) && \
	  $(MAKE) install && \
	  touch "$(BREAKPAD_DIR)/.install_marker" \
	)

breakpad_clean:
	$(V0) @echo " CLEAN        $(BREAKPAD_DIR)"
	$(V1) [ ! -d "$(BREAKPAD_DIR)" ] || $(RM) -rf $(BREAKPAD_DIR)

tools_required_breakpad:
ifeq ($(words $(wildcard $(BREAKPAD_DIR)/.install_marker $(BREAKPAD_DIR)/lib/libbreakpad_client.a $(BREAKPAD_DIR)/include/breakpad/client/linux/handler/exception_handler.h $(BREAKPAD_DIR)/bin/dump_syms)),4)
	$(info "Breakpad found in $(BREAKPAD_DIR)")
else
	$(error "Breakpad not found, please run `make breakpad_install`")
endif

HOST_CXX ?= g++
HOST_PYTHON ?= python3
breakpad_test: | tools_required_breakpad
	$(V1) mkdir -p "$(BUILD_DIR)/breakpad-smoke"
	$(V1) $(HOST_CXX) -std=c++17 -g -O0 -I"$(BREAKPAD_DIR)/include/breakpad" \
	  "$(ROOT_DIR)/ground/tests/breakpad_smoke.cpp" "$(BREAKPAD_DIR)/lib/libbreakpad_client.a" \
	  -pthread -o "$(BUILD_DIR)/breakpad-smoke/smoke"
	$(V1) $(HOST_PYTHON) "$(ROOT_DIR)/ground/tests/breakpad_smoke.py" \
	  "$(BUILD_DIR)/breakpad-smoke/smoke" "$(BREAKPAD_DIR)/bin" "$(BUILD_DIR)/breakpad-smoke"


##############################
#
# Set up paths to tools
#
##############################

ifneq ($(wildcard $(QT_SDK_QMAKE_PATH)),)
  QMAKE ?= $(QT_SDK_QMAKE_PATH)
else
  QMAKE ?= $(or $(shell command -v qmake6 2>/dev/null),qmake)
endif
QT_PLUGINS_DIR := $(shell $(QMAKE) -query QT_INSTALL_PLUGINS 2>/dev/null)
export QT_SDK_BIN_PATH := $(shell $(QMAKE) -query QT_INSTALL_BINS 2>/dev/null)

ifeq ($(shell [ -d "$(ARM_SDK_DIR)" ] && echo "exists"), exists)
  ARM_SDK_PREFIX := $(ARM_SDK_DIR)/bin/arm-none-eabi-
else
  ifndef IGNORE_MISSING_TOOLCHAIN
    ifeq (,$(findstring _install,$(MAKECMDGOALS)))
      # Desktop-only goals do not compile firmware or require an ARM SDK.
      GROUND_ONLY_GOALS := help gcs gcs_clean gcs_ts gcs_clazy gcs_ut_test uavobjgenerator uavobjects uavobjects_test breakpad_test glc6_download tools_required_glc6 tools_required_qt
      GROUND_ONLY_GOALS += package_ground package_ground_compress package_ground_symbols ground_package standalone usb_id_udev
      ifneq (,$(filter-out $(GROUND_ONLY_GOALS),$(or $(MAKECMDGOALS),help)))
        $(error **WARNING** ARM-SDK not in $(ARM_SDK_DIR)  Please run 'make arm_sdk_install')
      endif
    endif
  endif
  # not installed, hope it's in the path...
  ARM_SDK_PREFIX ?= arm-none-eabi-
endif

ifeq ($(shell [ -d "$(ZIP_DIR)" ] && echo "exists"), exists)
  export ZIPBIN := $(ZIP_DIR)/zip
else
  export ZIPBIN := zip
endif

ifeq ($(shell [ -d "$(OPENOCD_DIR)" ] && echo "exists"), exists)
  OPENOCD := $(OPENOCD_DIR)/bin/openocd
else
  # not installed, hope it's in the path...
  OPENOCD ?= openocd
endif

ifeq ($(shell [ -d "$(ANDROID_SDK_DIR)" ] && echo "exists"), exists)
  ANDROID     := $(ANDROID_SDK_DIR)/tools/android
  ANDROID_DX  := $(ANDROID_SDK_DIR)/build-tools/20.0.0/dx
  ANDROID_ADB := $(ANDROID_SDK_DIR)/platform-tools/adb
else
  # not installed, hope it's in the path...
  ANDROID     ?= android
  ANDROID_DX  ?= dx
  ANDROID_ADB ?= adb
endif

ifeq ($(shell [ -d "$(ASTYLE_DIR)" ] && echo "exists"), exists)
  ASTYLE := $(ASTYLE_DIR)/bin/astyle
else
  # not installed, hope it's in the path...
  ASTYLE ?= astyle
endif

ifeq ($(shell [ -d "$(UNCRUSTIFY_DIR)" ] && echo "exists"), exists)
  UNCRUSTIFY := $(UNCRUSTIFY_DIR)/bin/uncrustify
else
  # not installed, hope it's in the path...
  UNCRUSTIFY ?= uncrustify
endif

ifeq ($(shell [ -d "$(BREAKPAD_DIR)" ] && echo "exists"), exists)
  DUMP_SYMBOLS_TOOL := $(BREAKPAD_DIR)/bin/dump_syms
else
  ifndef IGNORE_MISSING_TOOLCHAIN
    ifneq (,$(filter package%, $(MAKECMDGOALS)))
      $(error **ERROR** breakpad not in $(BREAKPAD_DIR)! Please run 'make breakpad_install')
    endif
  endif
  DUMP_SYMBOLS_TOOL := dump_syms
endif
