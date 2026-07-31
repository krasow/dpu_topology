# DPU topology

Reports the physical topology of UPMEM DPU allocations, including DIMMs,
sockets, NUMA nodes, memory channels, `/dev/dpu_rankN` devices, and
`/dev/daxN.N` devices. It also identifies cross-socket allocations and DIMMs
that share a memory channel.

## Build

```sh
make
```

## Command line

```sh
build/dpu-topology --system                   # List all ranks
build/dpu-topology --system --group-by-dimm   # Group all ranks by DIMM
build/dpu-topology 256                        # Inspect a 256-DPU allocation
build/dpu-topology 256 --group-by-dimm        # Group the allocation by DIMM
```

## Automatic reporting

Preload the interposer to print a topology summary after each successful
`dpu_alloc()` call:

```sh
LD_PRELOAD="$PWD/build/libdpu_topology_preload.so" ./application
```

## C API

```c
#include <dpu_topology.h>

dpu_set_topology_t topology;
if (dpu_topology_inspect(dpu_set, &topology) == 0) {
    dpu_topology_fprint(stdout, &topology);
    dpu_topology_free(&topology);
}

int sockets = allocation_socket_count(&dpu_set);
allocation_fprint_overview(stdout, &dpu_set);
```

Available queries include `allocation_{socket,numa_node,dimm,channel}_count()`,
`allocation_spans_sockets()`, and `allocation_has_channel_contention()`.

Link `build/libdpu_topology.a` with the UPMEM host libraries. Physical DIMMs
are identified by VPD serial number because the driver does not expose a
motherboard slot label. SDK rank IDs are process-local, so Linux device
matching uses the current process's open rank descriptors. If multiple DPU
sets make the mapping ambiguous, `physical_mapping_complete` is false while
NUMA and socket information remains available.
