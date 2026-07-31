# DPU topology

Maps a `dpu_set_t` to its physical DIMMs, sockets, NUMA nodes, memory
channels, `/dev/dpu_rankN` devices, and `/dev/daxN.N` devices. It also detects
cross-socket allocations and DIMMs sharing a channel.

```sh
make -C lib/dpu_topology
lib/dpu_topology/build/dpu-topology --system                   # All ranks
lib/dpu_topology/build/dpu-topology --system --group-by-dimm   # All DIMMs
lib/dpu_topology/build/dpu-topology 256                        # Allocation
lib/dpu_topology/build/dpu-topology 256 --group-by-dimm        # By DIMM
```

```c
#include <dpu_topology.h>

dpu_set_topology_t topology;
if (dpu_topology_inspect(dpu_set, &topology) == 0) {
    dpu_topology_fprint(stdout, &topology);
    dpu_topology_free(&topology);
}
```

Link `build/libdpu_topology.a` with the UPMEM host libraries. Physical DIMMs
are identified by VPD serial number because the driver exposes no motherboard
slot label. The SDK rank ID is process-local, so Linux device matching uses
the current process's open rank descriptors. If multiple DPU sets make that
ambiguous, `physical_mapping_complete` is false while NUMA/socket data remains
available.
