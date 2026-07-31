CC ?= cc
AR ?= ar

BUILD_DIR := build
INCLUDE_DIR := include
LIBRARY := $(BUILD_DIR)/libdpu_topology.a
PRELOAD_LIBRARY := $(BUILD_DIR)/libdpu_topology_preload.so
TOOL := $(BUILD_DIR)/dpu-topology
SOURCES := src/dpu_topology.c src/dpu_topology_linux.c \
	src/dpu_topology_print.c src/dpu_topology_query.c
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

DPU_CFLAGS := $(shell dpu-pkg-config --cflags dpu)
DPU_LIBS := $(shell dpu-pkg-config --libs dpu)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fPIC \
	-I$(INCLUDE_DIR) $(DPU_CFLAGS)

.PHONY: all clean

all: $(LIBRARY) $(PRELOAD_LIBRARY) $(TOOL)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c include/dpu_topology.h src/dpu_topology_internal.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBRARY): $(OBJECTS)
	$(AR) rcs $@ $^

$(PRELOAD_LIBRARY): src/dpu_topology_preload.c $(LIBRARY)
	$(CC) $(CFLAGS) -shared $< $(LIBRARY) $(DPU_LIBS) -ldl -pthread -o $@

$(TOOL): tools/dpu-topology.c $(LIBRARY)
	$(CC) $(CFLAGS) $< $(LIBRARY) $(DPU_LIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
