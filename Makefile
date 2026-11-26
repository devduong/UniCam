HAS_TOOLS := $(wildcard tools/unicam_ctl/Makefile)

.PHONY: all acpi firmware kext cli verify clean

IASL ?= $(shell which iasl || echo /usr/local/bin/iasl)
BOARD_DIR := acpi/boards/dell-xps-7390-2in1

ifeq ($(strip $(HAS_TOOLS)),)
all: acpi kext
else
all: acpi kext cli
endif

acpi:
	$(IASL) -w2 -vw 4089 -p acpi/SSDT-CAM-DUMP acpi/SSDT-CAM-DUMP.dsl
	python3 acpi/scripts/gen_board_ssdt.py $(BOARD_DIR)/pinout_decoded.json \
		-o $(BOARD_DIR)/SSDT-CAM-Dell7390.dsl \
		--board-id "dell,xps-7390-2in1" \
		--board-slug Dell7390 \
		--oem-table-id "CAM7390 " \
		--compile
	$(IASL) -w2 -vw 4089 -p /tmp/uc_check_static acpi/templates/SSDT-CAM-Static.dsl
	$(IASL) -w2 -vw 4089 -p /tmp/uc_check_gnvs acpi/templates/SSDT-CAM-IntelGNVS.dsl
	@rm -f /tmp/uc_check_static.aml /tmp/uc_check_gnvs.aml

firmware:
	./kexts/UniCam/scripts/extract_ipu_fw.sh --download

kext:
	$(MAKE) -C kexts/UniCam all

ifneq ($(strip $(HAS_TOOLS)),)
cli:
	$(MAKE) -C tools/unicam_ctl all

verify: acpi kext cli
	$(MAKE) -C kexts/UniCam verify
	python3 acpi/scripts/unicam_ssdt_wizard.py --self-test
	python3 tools/decode_raw10.py --self-test
	./tools/unicam_ctl/build/unicam_ctl --doctor

clean:
	$(MAKE) -C kexts/UniCam clean
	$(MAKE) -C tools/unicam_ctl clean
else
verify: acpi kext
	$(MAKE) -C kexts/UniCam verify
	python3 acpi/scripts/unicam_ssdt_wizard.py --self-test

clean:
	$(MAKE) -C kexts/UniCam clean
endif
