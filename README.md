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

## Fault detection

```sh
build/dpu-topology --faults        # Driver and DIMM VPD only, no allocation
build/dpu-topology --check         # Allocate all DPUs and run a health check
build/dpu-topology --check 512 --mram-bytes 33554432 --timeout-ms 20000
```

Both exit with status 2 if they find faulty DPUs, grouped by rank:

```
DPU faults: 8 faulty DPUs on 1 rank, 1 DIMM (2560 DPUs checked)
  /dev/dpu_rank23: DIMM 3d622b62 rank 2/2, socket 1, NUMA 1, channel 7 -- 8 faulty DPUs
    DPU 1056 (23.4.0)        chip 4 dpu 0: MRAM readback mismatch (...); wrong result (...)
    ...
  Faulty DPU list: 23.4.0 23.4.1 23.4.2 23.4.3 23.4.4 23.4.5 23.4.6 23.4.7
```

DPUs are named `rank.chip.dpu`, matching the SDK; the rank is the driver's
`rank_id` (`/dev/dpu_rankN`).

| Source | Finds | Cost |
| --- | --- | --- |
| `--faults` / `dpu_faults_scan_system()` | chips masked by the driver; DPUs disabled or SRAM-repaired in VPD | sysfs reads |
| `dpu_faults_scan_set()` | DPUs the SDK disabled in an allocation | none |
| `--check` / `dpu_faults_health_check()` | MRAM errors, crashes, hangs, wrong results | overwrites MRAM |

`--check` tests 4 MiB of MRAM per DPU by default (about 7 s for 2560 DPUs);
use `--mram-bytes` (max 60 MiB) for a deeper pass. A clean result does not
rule out pattern-specific faults, so applications can add their own:

```c
#include <dpu_faults.h>

dpu_fault_report_t report;
dpu_fault_report_init(&report);
dpu_faults_scan_set(set, &report);                   /* optional */
DPU_FOREACH(set, dpu, index)
    if (!result_ok(index))
        dpu_faults_add(&report, dpu, index, "isolated test failed");
dpu_fault_report_fprint(stderr, &report);
dpu_fault_report_free(&report);
```

## Automatic reporting

Preload the interposer to print a topology summary after each successful
`dpu_alloc()` call:

```sh
LD_PRELOAD="$PWD/build/libdpu_topology_preload.so" ./application
```

The summary includes DPUs the SDK disabled. Set `DPU_TOPOLOGY_CHECK=1` to also
health-check each allocation before the application loads its program:

```sh
DPU_TOPOLOGY_CHECK=1 LD_PRELOAD="$PWD/build/libdpu_topology_preload.so" ./application
```

`DPU_TOPOLOGY_CHECK_MRAM_BYTES` sets the MRAM size tested.

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

Link `build/libdpu_topology.a` with the UPMEM host libraries, `-ldl` and
`-pthread`. The health-check kernel is embedded, so no `.dpu` file is needed.

DIMMs are identified by VPD serial number, since the driver exposes no slot
label. On hardware, SDK rank ids map directly to `/dev/dpu_rankN`; other
backends match the process's open rank descriptors, and if that is ambiguous
`physical_mapping_complete` is false (NUMA and socket info is still set).
