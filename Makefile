# trackir-mac: macOS port of the TrackIR 5 tracking path. Needs only the Xcode command line tools.
#
#   make            build build/trackir-mac
#   make test       build and run the unit/integration tests
#   make npclient   cross-compile the Wine NPClient.dll/NPClient64.dll (needs `brew install mingw-w64`)
#   make xplane     X-Plane 11/12 plugin (downloads the X-Plane SDK into build/ on first use)
#   make app        menu-bar app build/TrackIR-macOS.app

CXX      ?= clang++
CC       ?= clang
BUILD    := build
MACOS_MIN ?= 12.0
CPPFLAGS := -Isrc -MMD -MP -mmacosx-version-min=$(MACOS_MIN)
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS   := -std=c99 -O2 -Wall -Wextra
LDFLAGS  := -mmacosx-version-min=$(MACOS_MIN)
OBJCXXFLAGS := $(CXXFLAGS) -fobjc-arc
FRAMEWORKS := -framework Foundation -framework IOKit -framework IOUSBHost

CORE_SRC := src/common/np_shared.c \
            src/protocol/secure_codec.cpp src/protocol/frame.cpp src/protocol/camera_models.cpp \
            src/protocol/secure_camera.cpp src/protocol/classic_camera.cpp \
            src/vision/blobs.cpp src/vision/pose.cpp \
            src/output/profile.cpp src/output/filter.cpp src/output/np_bridge.cpp src/output/udp_sender.cpp \
            src/app/resources.cpp src/app/settings.cpp
ENGINE_SRC := src/app/engine.cpp src/usb/iousbhost_link.mm
APP_SRC  := src/app/main.cpp $(ENGINE_SRC)
TEST_SRC := tests/test_main.cpp

obj = $(patsubst %,$(BUILD)/%.o,$(basename $(1)))
CORE_OBJ := $(call obj,$(CORE_SRC))
APP_OBJ  := $(call obj,$(APP_SRC))
TEST_OBJ := $(call obj,$(TEST_SRC))

all: $(BUILD)/trackir-mac

$(BUILD)/trackir-mac: $(CORE_OBJ) $(APP_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(FRAMEWORKS)

$(BUILD)/run-tests: $(CORE_OBJ) $(TEST_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

test: $(BUILD)/run-tests
	./$(BUILD)/run-tests

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(OBJCXXFLAGS) -c $< -o $@

# Wine/CrossOver side: drop-in replacements for NPClient.dll and NPClient64.dll.
MINGW32 ?= i686-w64-mingw32-gcc
MINGW64 ?= x86_64-w64-mingw32-gcc
NPCLIENT_SRC := wine/npclient.c src/common/np_shared.c
npclient: $(BUILD)/wine/NPClient.dll $(BUILD)/wine/NPClient64.dll

$(BUILD)/wine/NPClient.dll: $(NPCLIENT_SRC) wine/NPClient.def
	@mkdir -p $(dir $@)
	$(MINGW32) -std=c99 -O2 -Wall -Isrc -shared -o $@ $(NPCLIENT_SRC) wine/NPClient.def -Wl,--enable-stdcall-fixup -static-libgcc

$(BUILD)/wine/NPClient64.dll: $(NPCLIENT_SRC) wine/NPClient64.def
	@mkdir -p $(dir $@)
	$(MINGW64) -std=c99 -O2 -Wall -Isrc -shared -o $@ $(NPCLIENT_SRC) wine/NPClient64.def -static-libgcc

# Menu-bar app: engine + Cocoa UI, with the CLI bundled for "Run Axis Check in Terminal".
MACAPP_SRC := src/macapp/AppDelegate.mm src/macapp/LiveView.mm src/macapp/SettingsWindow.mm
MACAPP_OBJ := $(call obj,$(MACAPP_SRC))
APP_BUNDLE := $(BUILD)/TrackIR-macOS.app
app: $(APP_BUNDLE)/Contents/MacOS/TrackIR-macOS

$(BUILD)/TrackIR-macOS: $(CORE_OBJ) $(call obj,$(ENGINE_SRC)) $(MACAPP_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(FRAMEWORKS) -framework Cocoa -framework Carbon -framework ServiceManagement -framework UniformTypeIdentifiers

# Assembled next to the old bundle and swapped in, so a running copy keeps its (still valid) executable.
$(APP_BUNDLE)/Contents/MacOS/TrackIR-macOS: $(BUILD)/TrackIR-macOS $(BUILD)/trackir-mac src/macapp/Info.plist
	rm -rf $(APP_BUNDLE).new && mkdir -p $(APP_BUNDLE).new/Contents/MacOS
	cp src/macapp/Info.plist $(APP_BUNDLE).new/Contents/Info.plist
	cp $(BUILD)/trackir-mac $(BUILD)/TrackIR-macOS $(APP_BUNDLE).new/Contents/MacOS/
	codesign -s - -f --deep $(APP_BUNDLE).new
	rm -rf $(APP_BUNDLE) && mv $(APP_BUNDLE).new $(APP_BUNDLE)

# X-Plane plugin. The SDK (MIT-style licence) is downloaded into build/, never committed.
XPSDK_URL ?= https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK430.zip
XPSDK     ?= $(BUILD)/sdk/SDK
XPLANE_PLUGIN := $(BUILD)/xplane/TrackIR-macOS/mac_x64/TrackIR-macOS.xpl
XPDEFS    := -DAPL=1 -DIBM=0 -DLIN=0 -DXPLM200 -DXPLM210 -DXPLM300 -DXPLM301
XPFLAGS   := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter $(XPDEFS) -I$(XPSDK)/CHeaders/XPLM -Isrc
xplane: $(XPLANE_PLUGIN)

$(XPSDK)/CHeaders/XPLM/XPLMDefs.h:
	@mkdir -p $(BUILD)/sdk
	curl -fsSL -o $(BUILD)/sdk/XPSDK.zip "$(XPSDK_URL)"
	cd $(BUILD)/sdk && unzip -q -o XPSDK.zip
	@touch $@

$(XPLANE_PLUGIN): xplane/trackir_xplane.cpp src/common/tir_bridge.h src/common/np_shared.h | $(XPSDK)/CHeaders/XPLM/XPLMDefs.h
	@mkdir -p $(dir $@)
	$(CXX) $(XPFLAGS) -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -fvisibility=hidden -dynamiclib \
	    -o $@ $< -F$(XPSDK)/Libraries/Mac -framework XPLM
	codesign -s - -f $@

# Loads the plugin into a fake XPLM.framework and drives it through a real bridge.
XPTEST := $(BUILD)/xplane-test
xplane-test: $(XPLANE_PLUGIN) $(XPTEST)/plugin-test
	DYLD_FRAMEWORK_PATH=$(XPTEST) $(XPTEST)/plugin-test $(XPLANE_PLUGIN)

$(XPTEST)/XPLM.framework/XPLM: tests/xplane/fake_xplm.cpp | $(XPSDK)/CHeaders/XPLM/XPLMDefs.h
	@mkdir -p $(dir $@)
	$(CXX) $(XPFLAGS) -mmacosx-version-min=$(MACOS_MIN) -dynamiclib -install_name @executable_path/../../../Resources/plugins/XPLM.framework/XPLM -o $@ $<

$(XPTEST)/plugin-test: tests/xplane/plugin_test.cpp $(BUILD)/src/output/np_bridge.o $(BUILD)/src/common/np_shared.o $(XPTEST)/XPLM.framework/XPLM
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/xplane/plugin_test.cpp $(BUILD)/src/output/np_bridge.o \
	    $(BUILD)/src/common/np_shared.o -F$(XPTEST) -framework XPLM

clean:
	rm -rf $(BUILD)

.PHONY: all test npclient xplane xplane-test app clean

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# Bit-exact checks against the vendor binaries, executed under the Unicorn CPU emulator.
#   python3 -m venv .venv && .venv/bin/pip install pefile unicorn
VENDOR   ?= ../extracted/msi/Program Files/TrackIR5
PYTHON   ?= .venv/bin/python
verify: $(BUILD)/trackir-mac $(BUILD)/verify/np_vectors $(BUILD)/verify/secure_harness
	@test -f "$(VENDOR)/TrackIR5.exe" || (echo "set VENDOR to the TrackIR5 install folder"; exit 1)
	$(BUILD)/trackir-mac extract-fpga "$(VENDOR)/TrackIR5.exe" $(BUILD)/verify/res > /dev/null
	$(PYTHON) -I tools/verify/emulate_npclient.py "$(VENDOR)/NPClient.dll" $(BUILD)/verify/np_vectors
	$(PYTHON) -I tools/verify/emulate_rev35.py "$(VENDOR)/TrackIR5.exe" $(BUILD)/verify/secure_harness $(BUILD)/verify/res/rev35_keys.bin

$(BUILD)/verify/np_vectors: tools/verify/np_vectors.c src/common/np_shared.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -Isrc $^ -o $@

$(BUILD)/verify/secure_harness: tools/verify/secure_harness.cpp src/protocol/secure_codec.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -Isrc $^ -o $@

.PHONY: verify
