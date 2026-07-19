ARTIFACTS_DIR := .artifacts
.DEFAULT_GOAL := build

TUI_LIB_DIR := ../lib/tui/lib
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
MAIN_OBJECT := $(ARTIFACTS_DIR)/main.o
APP := $(ARTIFACTS_DIR)/observer
COMPILE_COMMANDS := $(ARTIFACTS_DIR)/compile_commands.json
PROJECT_COMPILE_COMMANDS := ../compile_commands.json

MODULES := $(TUI_MODULES)
MODULE_PCMS := $(TUI_MODULE_PCMS)
MODULE_OBJECTS := $(TUI_MODULE_OBJECTS)

DB_FRAGMENTS := \
	$(TUI_DB_FRAGMENTS) \
	$(ARTIFACTS_DIR)/main.json

.PHONY: build run clean
.DELETE_ON_ERROR:

build: $(APP) $(COMPILE_COMMANDS) $(PROJECT_COMPILE_COMMANDS)

run: $(APP)
	./$(APP)

clean:
	rm -rf $(ARTIFACTS_DIR)
	rm -f $(PROJECT_COMPILE_COMMANDS)

$(MAIN_OBJECT): $(MAIN_SOURCE) $(MODULE_PCMS)
	mkdir -p $(dir $@)
	$(TUI_CXX) $(CPPFLAGS) $(CXXFLAGS) -MJ $(ARTIFACTS_DIR)/main.json \
		-fprebuilt-module-path=$(ARTIFACTS_DIR) \
		-c $< -o $@

$(APP): $(MODULE_OBJECTS) $(MAIN_OBJECT)
	mkdir -p $(dir $@)
	$(TUI_CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(COMPILE_COMMANDS): $(DB_FRAGMENTS)
	mkdir -p $(dir $@)
	printf '[\n' > $@
	sed '$$s/,$$//' $^ >> $@
	printf '\n]\n' >> $@

$(PROJECT_COMPILE_COMMANDS): $(COMPILE_COMMANDS)
	ln -sfn observer/$(COMPILE_COMMANDS) $@
