# SPDX-License-Identifier: GPL-3.0-or-later
# Linux first-install runtime and build-matched full images. No device writes.
FIRMWARE_INSTALL_TARGETS ?= revolution radix omnibusf4sd
FIRMWARE_INSTALLER_DIR := $(TOOLS_DIR)/firmware-installer
FIRMWARE_RUNTIME_MARKER := $(FIRMWARE_INSTALLER_DIR)/.installed-0.11-1.0.29

.PHONY: firmware_installer_runtime firmware_install_bundle
.PHONY: ef_radix ef_radix_hex
.PHONY: ef_omnibusf4sd ef_omnibusf4sd_hex
ef_omnibusf4sd: ef_omnibusf4sd_hex
ef_omnibusf4sd_hex: fw_omnibusf4sd
	$(V1) $(HOST_PYTHON) "$(ROOT_DIR)/make/scripts/package-imported-board.py" --build "$(BUILD_DIR)" --profile omnibusf4sd
ef_radix: ef_radix_hex
ef_radix_hex: fw_radix
	$(V1) $(HOST_PYTHON) "$(ROOT_DIR)/make/scripts/package-imported-board.py" --build "$(BUILD_DIR)" --profile radix
firmware_installer_runtime: $(FIRMWARE_RUNTIME_MARKER)

$(FIRMWARE_RUNTIME_MARKER): $(ROOT_DIR)/shared/firmware/installer-sources.json $(ROOT_DIR)/make/firmware-installer.mk | $(DL_DIR) $(TOOLS_DIR)
	$(V1) test -f "$(DL_DIR)/libusb-1.0.29.tar.bz2" || curl -fL --retry 2 -o "$(DL_DIR)/libusb-1.0.29.tar.bz2" "https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.tar.bz2"
	$(V1) test -f "$(DL_DIR)/dfu-util-0.11.tar.gz" || curl -fL --retry 2 -o "$(DL_DIR)/dfu-util-0.11.tar.gz" "https://dfu-util.sourceforge.net/releases/dfu-util-0.11.tar.gz"
	$(V1) echo "5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85  $(DL_DIR)/libusb-1.0.29.tar.bz2" | sha256sum -c -
	$(V1) echo "b4b53ba21a82ef7e3d4c47df2952adf5fa494f499b6b0b57c58c5d04ae8ff19e  $(DL_DIR)/dfu-util-0.11.tar.gz" | sha256sum -c -
	$(V1) tar -xjf "$(DL_DIR)/libusb-1.0.29.tar.bz2" -C "$(DL_DIR)"
	$(V1) tar -xzf "$(DL_DIR)/dfu-util-0.11.tar.gz" -C "$(DL_DIR)"
	$(V1) cd "$(DL_DIR)/libusb-1.0.29" && ./configure --prefix="$(FIRMWARE_INSTALLER_DIR)" --disable-udev --enable-shared --disable-static
	+$(V1) $(MAKE) -C "$(DL_DIR)/libusb-1.0.29"
	+$(V1) $(MAKE) -C "$(DL_DIR)/libusb-1.0.29" install
	$(V1) cd "$(DL_DIR)/dfu-util-0.11" && PKG_CONFIG_PATH="$(FIRMWARE_INSTALLER_DIR)/lib/pkgconfig" LDFLAGS="-Wl,-rpath,'\$$\$$ORIGIN/../lib/droning'" ./configure --prefix="$(FIRMWARE_INSTALLER_DIR)"
	+$(V1) $(MAKE) -C "$(DL_DIR)/dfu-util-0.11"
	+$(V1) $(MAKE) -C "$(DL_DIR)/dfu-util-0.11" install
	$(V1) touch "$@"

firmware_install_bundle: firmware_installer_runtime $(addprefix ef_,$(FIRMWARE_INSTALL_TARGETS))
	$(V1) $(HOST_PYTHON) "$(ROOT_DIR)/ground/tests/firmware_compatibility.py"
	$(V1) $(HOST_PYTHON) "$(ROOT_DIR)/make/scripts/firmware-install-bundle.py" --root "$(ROOT_DIR)" --build "$(BUILD_DIR)" --runtime "$(FIRMWARE_INSTALLER_DIR)" $(FIRMWARE_INSTALL_TARGETS)
