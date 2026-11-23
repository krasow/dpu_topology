CC ?= cc
AR ?= ar
DPU_CC ?= dpu-upmem-dpurte-clang

BUILD_DIR := build
INCLUDE_DIR := include
LIBRARY := $(BUILD_DIR)/libdpu_topology.a
PRELOAD_LIBRARY := $(BUILD_DIR)/libdpu_topology_preload.so
TOOL := $(BUILD_DIR)/dpu-topology
HEALTH_KERNEL := $(BUILD_DIR)/health_kernel.dpu
SOURCES := src/topology.c src/linux.c src/print.c src/query.c \
	src/faults.c src/health.c
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
HEADERS := include/dpu_topology.h include/dpu_faults.h src/internal.h dpu/health.h

DPU_CFLAGS := $(shell dpu-pkg-config --cflags dpu)
DPU_LIBS := $(shell dpu-pkg-config --libs dpu)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fPIC \
	-I$(INCLUDE_DIR) -Idpu $(DPU_CFLAGS)
LDLIBS := $(DPU_LIBS) -ldl -pthread

.PHONY: all clean

all: $(LIBRARY) $(PRELOAD_LIBRARY) $(TOOL)

$(BUILD_DIR):
	mkdir -p $@

$(HEALTH_KERNEL): dpu/health_kernel.c dpu/health.h | $(BUILD_DIR)
	UPMEM_NO_OS_WARNING=1 $(DPU_CC) -O2 -Wall -Wextra \
		-DNR_TASKLETS=16 $< -o $@

$(BUILD_DIR)/%.o: src/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# The health kernel is embedded with .incbin, so the library is standalone.
$(BUILD_DIR)/health.o: $(HEALTH_KERNEL)
$(BUILD_DIR)/health.o: CFLAGS += \
	-DDPU_TOPOLOGY_HEALTH_KERNEL='"$(abspath $(HEALTH_KERNEL))"'

$(LIBRARY): $(OBJECTS)
	$(AR) rcs $@ $^

$(PRELOAD_LIBRARY): src/preload.c $(LIBRARY)
	$(CC) $(CFLAGS) -shared $< $(LIBRARY) $(LDLIBS) -o $@

$(TOOL): tools/dpu-topology.c $(LIBRARY)
	$(CC) $(CFLAGS) $< $(LIBRARY) $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
