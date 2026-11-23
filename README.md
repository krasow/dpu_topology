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
build/dpu-topology --faults                      # Static: driver + DIMM VPD, no allocation
build/dpu-topology --check                       # Allocate all DPUs and health-check them
build/dpu-topology --check 512 --mram-bytes 33554432 --timeout-ms 20000
```

Both commands exit with status 2 when they find faulty DPUs. Faults are
grouped by physical rank, with the DIMM, socket, NUMA node and channel:

```
DPU faults: 8 faulty DPUs on 1 rank, 1 DIMM (2560 DPUs checked)
  /dev/dpu_rank23: DIMM 3d622b62 rank 2/2, socket 1, NUMA 1, channel 7 -- 8 faulty DPUs
    DPU 1056 (23.4.0)        chip 4 dpu 0: MRAM readback mismatch (240/524288 words differ, first at heap+0x222270); wrong result (444/524288 words wrong, first at heap+0x22270; tasklet mask 0xdff7)
    ...
  Faulty DPU list: 23.4.0 23.4.1 23.4.2 23.4.3 23.4.4 23.4.5 23.4.6 23.4.7
```

DPU ids use the SDK's `rank.chip.dpu` form, so `DPU 1259 (26.5.3)` from an
application log means DPU index 1259 in its set, on `/dev/dpu_rank26`, chip
(control interface) 5, DPU 3. On hardware the SDK rank number is the driver's
`rank_id`, so it is stable across processes.

Detection sources:

| Source | Finds | Cost |
| --- | --- | --- |
| `--faults` / `dpu_faults_scan_system()` | chips masked by the driver, DPUs disabled or SRAM-repaired in the DIMM VPD | sysfs reads only |
| `dpu_faults_scan_set()` | DPUs of an allocation the SDK left disabled, with the reason | no hardware access |
| `--check` / `dpu_faults_health_check()` | MRAM write/readback errors, execution faults, hangs, wrong results from a 16-tasklet kernel | loads a program, overwrites MRAM |

The health check exercises 4 MiB of MRAM per DPU by default, which takes
about 7 s for 2560 DPUs. Some faults only appear under sustained traffic, so
use `--mram-bytes` (up to 60 MiB) for a deeper pass. A clean result does not
guarantee that an application-specific failure cannot occur, so applications
can record their own findings:

```c
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

The summary also lists DPUs the SDK disabled in the allocation. To also
health-check every allocation before the application loads its program:

```sh
DPU_TOPOLOGY_CHECK=1 LD_PRELOAD="$PWD/build/libdpu_topology_preload.so" ./application
```

`DPU_TOPOLOGY_CHECK_MRAM_BYTES` overrides the tested MRAM size.

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

Link `build/libdpu_topology.a` with the UPMEM host libraries plus `-ldl
-pthread`. The health-check DPU kernel is embedded in the archive, so no
separate `.dpu` file is needed at run time. Physical DIMMs
are identified by VPD serial number because the driver does not expose a
motherboard slot label. On hardware, each SDK rank id embeds the driver's
`rank_id`, which maps it exactly to `/dev/dpu_rankN`. Other backends fall back
to matching the current process's open rank descriptors. If multiple DPU sets
make that fallback ambiguous, `physical_mapping_complete` is false while NUMA
and socket information remains available.
