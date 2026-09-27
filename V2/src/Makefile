# ==============================================================================
# Makefile for Smart Weather Alarm Clock (src/Makefile)
# ==============================================================================

DRIVER_DIR = driver
APP_DIR    = app

.PHONY: all driver app debug clean install

all: driver app
	@echo "=================================================="
	@echo "  PROJECT 2 BUILD COMPLETED SUCCESSFULLY"
	@echo "=================================================="

debug:
	@echo "--- [1/1] Building Userspace Application (Debug Mode) ---"
	$(MAKE) -C $(APP_DIR) debug

driver:
	@echo "--- [1/2] Building Kernel Drivers ---"
	$(MAKE) -C $(DRIVER_DIR)

app:
	@echo "--- [2/2] Building Userspace Application ---"
	$(MAKE) -C $(APP_DIR)

clean:
	@echo "--- Cleaning Kernel Drivers ---"
	$(MAKE) -C $(DRIVER_DIR) clean
	@echo "--- Cleaning Userspace Application ---"
	$(MAKE) -C $(APP_DIR) clean

install:
	@echo "Installing binaries to system paths..."
	install -d /usr/bin
	install -m 755 $(APP_DIR)/smartclock /usr/bin/smartclock
	install -d /etc/smartclock