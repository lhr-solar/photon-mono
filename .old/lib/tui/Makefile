ARTIFACTS_DIR := .artifacts
.DEFAULT_GOAL := all

TUI_LIB_DIR := lib
TUI_ARTIFACTS_DIR := $(ARTIFACTS_DIR)
TUI_CXX := clang++
TUI_CPPFLAGS :=
TUI_CXXFLAGS := -std=c++23

include $(TUI_LIB_DIR)/tui.mk

CPPFLAGS := $(TUI_CPPFLAGS)
CXXFLAGS := $(TUI_CXXFLAGS)
LDFLAGS :=
LDLIBS :=

MAIN_SOURCE := main.cpp

MODULES := $(TUI_MODULES)
MODULE_PCMS := $(TUI_MODULE_PCMS)
MODULE_OBJECTS := $(TUI_MODULE_OBJECTS)
MAIN_OBJECT := $(ARTIFACTS_DIR)/main.o
APP := $(ARTIFACTS_DIR)/app
COMPILE_COMMANDS := $(ARTIFACTS_DIR)/compile_commands.json

DB_FRAGMENTS := \
	$(TUI_DB_FRAGMENTS) \
	$(ARTIFACTS_DIR)/main.json

.PHONY: all build run clean
.DELETE_ON_ERROR:

all: build

build: $(APP) $(COMPILE_COMMANDS)

run: $(APP)
	./$(APP)

clean:
	rm -rf $(ARTIFACTS_DIR) .cache app main.o tui.o tui.pcm sphere.o sphere.pcm progressBar.o progressBar.pcm linePlot.o linePlot.pcm barPlot.o barPlot.pcm spinner.o spinner.pcm compile_commands.json

$(MAIN_OBJECT): $(MAIN_SOURCE) $(MODULE_PCMS)
	mkdir -p $(dir $@)
	$(TUI_CXX) $(CPPFLAGS) $(CXXFLAGS) -MJ $(ARTIFACTS_DIR)/main.json \
		$(foreach module,$(MODULES),-fmodule-file=$(module)=$(ARTIFACTS_DIR)/$(module).pcm) \
		-c $< -o $@

$(APP): $(MODULE_OBJECTS) $(MAIN_OBJECT)
	mkdir -p $(dir $@)
	$(TUI_CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(COMPILE_COMMANDS): $(DB_FRAGMENTS)
	mkdir -p $(dir $@)
	printf '[\n' > $@
	sed '$$s/,$$//' $^ >> $@
	printf '\n]\n' >> $@
