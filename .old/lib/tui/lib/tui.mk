TUI_LIB_DIR ?= lib
TUI_ARTIFACTS_DIR ?= .artifacts
TUI_CXX ?= clang++
TUI_CPPFLAGS ?=
TUI_CXXFLAGS ?= -std=c++23

TUI_MODULES := sphere progressBar linePlot barPlot spinner tui
TUI_PUBLIC_MODULE := tui
TUI_MODULE_SOURCES := $(TUI_MODULES:%=$(TUI_LIB_DIR)/%.cppm)
TUI_MODULE_PCMS := $(TUI_MODULES:%=$(TUI_ARTIFACTS_DIR)/%.pcm)
TUI_MODULE_OBJECTS := $(TUI_MODULES:%=$(TUI_ARTIFACTS_DIR)/%.o)
TUI_DB_FRAGMENTS := \
	$(TUI_MODULES:%=$(TUI_ARTIFACTS_DIR)/%.pcm.json) \
	$(TUI_MODULES:%=$(TUI_ARTIFACTS_DIR)/%.json)

$(TUI_ARTIFACTS_DIR):
	mkdir -p $(TUI_ARTIFACTS_DIR)

$(TUI_ARTIFACTS_DIR)/%.pcm: $(TUI_LIB_DIR)/%.cppm
	mkdir -p $(dir $@)
	$(TUI_CXX) $(TUI_CPPFLAGS) $(TUI_CXXFLAGS) -MJ $(TUI_ARTIFACTS_DIR)/$*.pcm.json --precompile $< -o $@

$(TUI_ARTIFACTS_DIR)/tui.pcm: $(TUI_ARTIFACTS_DIR)/sphere.pcm $(TUI_ARTIFACTS_DIR)/progressBar.pcm \
	$(TUI_ARTIFACTS_DIR)/linePlot.pcm $(TUI_ARTIFACTS_DIR)/barPlot.pcm $(TUI_ARTIFACTS_DIR)/spinner.pcm
$(TUI_ARTIFACTS_DIR)/tui.pcm: TUI_CPPFLAGS += -fprebuilt-module-path=$(TUI_ARTIFACTS_DIR)
$(TUI_ARTIFACTS_DIR)/tui.o: TUI_CPPFLAGS += -fprebuilt-module-path=$(TUI_ARTIFACTS_DIR)

$(TUI_ARTIFACTS_DIR)/%.o: $(TUI_ARTIFACTS_DIR)/%.pcm
	mkdir -p $(dir $@)
	$(TUI_CXX) $(TUI_CPPFLAGS) $(TUI_CXXFLAGS) -MJ $(TUI_ARTIFACTS_DIR)/$*.json $< -c -o $@
