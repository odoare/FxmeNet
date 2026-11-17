# Development shortcuts around the CMake build (Linux).
#
#   make            build and install the server and the VST3 plugin
#   make server     build fxmenet-server, install it in $(BINDIR)
#   make plugin     build FxmeNet.vst3, install it in $(VST3DIR)
#   make top        build fxmenet-top, install it in $(BINDIR)
#   make tests      build and run the protocol tests
#
# Variables: BUILD (build folder), JOBS, BINDIR, VST3DIR.

BUILD   ?= build
JOBS    ?= 2
BINDIR  ?= $(HOME)/bin
VST3DIR ?= $(HOME)/.vst3

CMAKE_BUILD = cmake --build $(BUILD) -j$(JOBS) --target

.PHONY: all install server plugin top tests configure

all install: server plugin

configure: $(BUILD)/CMakeCache.txt

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD)

server: configure
	$(CMAKE_BUILD) FxmeNetServer
	mkdir -p $(BINDIR)
	install -m 755 $(BUILD)/Server/FxmeNetServer_artefacts/fxmenet-server $(BINDIR)/fxmenet-server

plugin: configure
	$(CMAKE_BUILD) FxmeNetPlugin_VST3
	mkdir -p $(VST3DIR)
	rm -rf $(VST3DIR)/FxmeNet.vst3
	cp -R $(BUILD)/Plugin/FxmeNetPlugin_artefacts/VST3/FxmeNet.vst3 $(VST3DIR)/

top: configure
	$(CMAKE_BUILD) FxmeNetTop
	mkdir -p $(BINDIR)
	install -m 755 $$(find $(BUILD)/Tools/Top -name fxmenet-top -type f -perm -u+x | head -1) $(BINDIR)/fxmenet-top

tests: configure
	$(CMAKE_BUILD) FxmeNetProtocolTests
	$$(find $(BUILD) -name FxmeNetProtocolTests -type f -perm -u+x | head -1)
