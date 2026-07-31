# DPU topology

`libdpu_topology` relates an allocated `dpu_set_t` to the physical UPMEM
topology exposed by the SDK and Linux sysfs. It reports:

- whether an allocation spans NUMA nodes or CPU sockets;
- the physical UPMEM DIMMs used, identified by their VPD serial numbers;
- the memory channels used and whether multiple allocated DIMMs share one;
- the `/dev/dpu_rankN` and DAX device belonging to every allocated rank.

The Linux device node is `/dev/daxN.N` on the tested UPMEM system. The
corresponding sysfs subsystem directory is named `dpu_dax`.

## Build and inspect

```sh
make -C lib/dpu_topology
lib/dpu_topology/build/dpu-topology --system
lib/dpu_topology/build/dpu-topology --system --group-by-dimm
lib/dpu_topology/build/dpu-topology 256
lib/dpu_topology/build/dpu-topology 256 --group-by-dimm
```

`--system` prints the complete DAX-to-physical-DIMM mapping without allocating
DPUs. Supplying a DPU count inspects a real allocation and flags cross-socket
placement and memory-channel contention. `--group-by-dimm` combines ranks with
the same VPD serial number and shows the rank and DAX devices belonging to each
physical DIMM.

## Library API

```c
#include <dpu_topology.h>

dpu_set_topology_t topology;
int status = dpu_topology_inspect(dpu_set, &topology);
if (status == 0) {
    dpu_topology_fprint(stdout, &topology);
    if (topology.spans_sockets) {
        /* Mark this run as a cross-socket allocation. */
    }
    dpu_topology_free(&topology);
}
```

Link `build/libdpu_topology.a` along with the normal UPMEM host libraries.

The kernel driver does not expose a motherboard slot label. A physical DIMM
is therefore identified by its UPMEM VPD serial number; its two DPU ranks have
the same serial number and distinct `rank_index` values.

The public UPMEM SDK rank ID is process-local and is not the `dpu_rankN`
device index. On Linux, the library resolves that missing association from the
rank device descriptors held by the current process. Physical mapping is
complete when the process owns only the ranks represented by the inspected
set. `physical_mapping_complete` is false when that association is ambiguous,
while SDK NUMA/socket detection remains available.
# dpu_topology
