SDK_PATH := $(shell xcrun --show-sdk-path)
MACKERNEL_SDK ?=
KERNEL_HDRS := $(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers
LIB_SEARCH := $(SDK_PATH)/usr/lib

ifneq ($(MACKERNEL_SDK),)
  ifneq ($(wildcard $(MACKERNEL_SDK)/Library/universal/libkmod.a),)
    LIB_SEARCH := $(MACKERNEL_SDK)/Library/universal
  endif
else ifneq ($(wildcard ../MacKernelSDK/Library/universal/libkmod.a),)
  LIB_SEARCH := ../MacKernelSDK/Library/universal
endif

CXX := clang++
CXXFLAGS := -std=gnu++17 -arch x86_64 -mmacosx-version-min=13.0 -O2 \
            -Wall -Wextra -Werror -Wno-deprecated-declarations \
            -mkernel -DKERNEL -fno-builtin -fno-common -fno-rtti -fno-exceptions \
            -fapple-kext -nostdinc \
            -mno-sse -mno-mmx -mno-avx -mno-sse2 -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 \
            -Iinclude -Isrc -Isrc/Core -Isrc/IPU -Isrc/UVC \
            -isystem $(KERNEL_HDRS)

LDFLAGS := -arch x86_64 -mmacosx-version-min=13.0 -Wl,-kext -nostdlib \
           -L$(LIB_SEARCH) -lkmodc++ -lkmod -Wl,-undefined,dynamic_lookup

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
GEN_DIR := $(BUILD_DIR)/gen
KEXT_BUNDLE := $(BUILD_DIR)/UniCam.kext
KEXT_CONTENTS := $(KEXT_BUNDLE)/Contents
KEXT_MACOS := $(KEXT_CONTENTS)/MacOS
KEXT_BIN := $(KEXT_MACOS)/UniCam
FW_BIN := firmware/ipu4p/cpd_component_signed.bin
GEN_FW_CPP := $(GEN_DIR)/EmbeddedIpu4pFw.cpp
GEN_FW_OBJ := $(OBJ_DIR)/gen/EmbeddedIpu4pFw.o

SRCS := \
	src/kmod_main.cpp \
	src/Core/BoardHal/AcpiMethodPowerHal.cpp \
	src/Core/BoardHal/MmioGpioClockPowerHal.cpp \
	src/Core/BoardHal/Tps68470PowerHal.cpp \
	src/Core/I2C/VoodooI2CNubTransport.cpp \
	src/Core/Sensors/OV01A10Driver.cpp \
	src/Core/Sensors/OV02C10Driver.cpp \
	src/Core/Sensors/OV2740Driver.cpp \
	src/Core/Sensors/OV5675Driver.cpp \
	src/Core/Sensors/OV5693Driver.cpp \
	src/Core/Sensors/SensorRegistry.cpp \
	src/Core/UniCamCoreService.cpp \
	src/Core/UniCamCoreUserClient.cpp \
	src/IPU/Hal/Ipu4p/IpuDmaBuffer.cpp \
	src/IPU/Hal/Ipu4p/Ipu4pMmu.cpp \
	src/IPU/Hal/Ipu4p/Ipu4pCpd.cpp \
	src/IPU/Hal/Ipu4p/Ipu4pButtress.cpp \
	src/IPU/Hal/Ipu4p/Ipu4pSyscom.cpp \
	src/IPU/Hal/Ipu4p/Ipu4pCsiPhy.cpp \
	src/IPU/Hal/Ipu4p/Ipu4pIsysHal.cpp \
	src/IPU/Hal/Ipu4p/IpuEmbeddedFw.cpp \
	src/IPU/SoftISP/UniCamSoftISP.cpp \
	src/IPU/UniCamIPUService.cpp \
	src/IPU/UniCamIPUUserClient.cpp \
	src/UVC/UniCamVirtualUVC.cpp \
	src/UVC/UniCamBridgeService.cpp

OBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean verify

all: $(KEXT_BUNDLE)

$(FW_BIN):
	./scripts/extract_ipu_fw.sh --download

$(GEN_FW_CPP): $(FW_BIN) scripts/gen_embedded_fw.py
	@mkdir -p $(GEN_DIR)
	python3 scripts/gen_embedded_fw.py $(FW_BIN) $(GEN_FW_CPP)

$(GEN_FW_OBJ): $(GEN_FW_CPP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KEXT_BUNDLE): $(OBJS) $(GEN_FW_OBJ) Info.plist
	@mkdir -p $(KEXT_MACOS)
	$(CXX) $(LDFLAGS) $(OBJS) $(GEN_FW_OBJ) -o $(KEXT_BIN)
	cp Info.plist $(KEXT_CONTENTS)/Info.plist
	plutil -lint $(KEXT_CONTENTS)/Info.plist
	codesign --force --sign - $(KEXT_BUNDLE)
	@echo "[+] Built $(KEXT_BUNDLE)"

verify: $(KEXT_BUNDLE)
	@(cd firmware/ipu4p && shasum -a 256 -c SHA256SUMS.txt)
	@python3 -c "import glob, json; [json.load(open(p)) for p in sorted(glob.glob('profiles/sensors/*.json'))]; print('[+] Sensor profile JSONs OK (' + str(len(glob.glob('profiles/sensors/*.json'))) + ')')"
	plutil -lint $(KEXT_CONTENTS)/Info.plist
	otool -hv $(KEXT_BIN) | grep -q KEXTBUNDLE
	nm -g $(KEXT_BIN) | grep -q " _kmod_info$$"
	! otool -tV $(KEXT_BIN) | grep -E -q '%[xyz]mm[0-9]'
	@echo "[+] UniCam.kext Mach-O KEXTBUNDLE + _kmod_info + zero-SIMD verified"

clean:
	rm -rf $(BUILD_DIR)
