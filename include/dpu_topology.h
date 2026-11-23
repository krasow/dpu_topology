#ifndef DPU_TOPOLOGY_H
#define DPU_TOPOLOGY_H

#include <dpu.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPU_TOPOLOGY_DEVICE_LEN 128
#define DPU_TOPOLOGY_ID_LEN 64

typedef struct {
    uint32_t sysfs_index;
    int numa_node;
    int socket_id;
    int channel_id;
    int dimm_rank_index;
    int dimm_rank_count;
    char rank_device[DPU_TOPOLOGY_DEVICE_LEN];
    char dax_device[DPU_TOPOLOGY_DEVICE_LEN];
    char dimm_serial[DPU_TOPOLOGY_ID_LEN];
    char dimm_part_number[DPU_TOPOLOGY_ID_LEN];
} dpu_rank_topology_t;

typedef struct {
    dpu_rank_topology_t *ranks;
    size_t nr_ranks;
    uint32_t nr_dpus;
    size_t nr_numa_nodes;
    size_t nr_sockets;
    size_t nr_dimms;
    size_t nr_channels;
    int physical_mapping_complete;
    int spans_numa_nodes;
    int spans_sockets;
    int has_channel_contention;
} dpu_set_topology_t;

/*
 * Inspects the physical ranks backing an allocated DPU set.
 * Returns 0 on success and a negative errno value on failure.
 */
int dpu_topology_inspect(struct dpu_set_t set, dpu_set_topology_t *topology);

/* Releases memory owned by a topology returned by dpu_topology_inspect. */
void dpu_topology_free(dpu_set_topology_t *topology);

/*
 * Discovers all DPU rank devices visible in sysfs. The caller owns *ranks and
 * must release it with dpu_topology_free_ranks.
 */
int dpu_topology_discover_system(dpu_rank_topology_t **ranks,
                                 size_t *nr_ranks);
void dpu_topology_free_ranks(dpu_rank_topology_t *ranks);

/* Human-readable reports for benchmark logs and standalone diagnostics. */
void dpu_topology_fprint(FILE *stream, const dpu_set_topology_t *topology);
void dpu_topology_fprint_grouped(FILE *stream,
                                 const dpu_set_topology_t *topology);
int dpu_topology_fprint_system(FILE *stream);
int dpu_topology_fprint_system_grouped(FILE *stream);

/* Direct allocation queries. Negative return values are errno codes. */
int allocation_socket_count(const struct dpu_set_t *set);
int allocation_numa_node_count(const struct dpu_set_t *set);
int allocation_dimm_count(const struct dpu_set_t *set);
int allocation_channel_count(const struct dpu_set_t *set);
int allocation_spans_sockets(const struct dpu_set_t *set);
int allocation_has_channel_contention(const struct dpu_set_t *set);

/* Prints a compact allocation summary after one topology inspection. */
int allocation_fprint_overview(FILE *stream, const struct dpu_set_t *set);

/* ------------------------------------------------------------------------
 * DPU fault detection
 *
 * A fault report collects faulty DPUs from any combination of sources and
 * prints them grouped by physical rank (/dev/dpu_rankN) and DIMM.
 * ------------------------------------------------------------------------ */

#define DPU_FAULT_DETAIL_LEN 96
#define DPU_FAULT_NO_INDEX UINT32_MAX

typedef enum {
    /* Static: recorded by the driver or the DIMM VPD. */
    DPU_FAULT_CI_MASKED,    /* control interface (chip) masked by driver */
    DPU_FAULT_VPD_DISABLED, /* marked disabled in the DIMM VPD */
    DPU_FAULT_VPD_REPAIR,   /* has a VPD SRAM repair entry (SDK disables) */
    /* Allocation: the SDK excluded the DPU from the allocated set. */
    DPU_FAULT_SDK_DISABLED,
    /* Active health check. */
    DPU_FAULT_MEMORY,       /* host MRAM write/readback mismatch */
    DPU_FAULT_EXEC_FAULT,   /* DPU raised a fault while running */
    DPU_FAULT_TIMEOUT,      /* DPU did not finish in time */
    DPU_FAULT_WRONG_RESULT, /* DPU finished but computed wrong data */
    /* Added by the application through dpu_faults_add(). */
    DPU_FAULT_REPORTED,
} dpu_fault_kind_t;

typedef struct {
    dpu_fault_kind_t kind;
    /* Index in DPU_FOREACH order, or DPU_FAULT_NO_INDEX. */
    uint32_t set_index;
    /* SDK rank number, the "26" in DPU id 26.5.3, or -1. On hardware it
     * is the driver's rank_id, i.e. usually /dev/dpu_rank26. */
    int sdk_rank_id;
    int slice_id;  /* control interface / chip within the rank */
    int member_id; /* DPU within the chip */
    /* Physical location; sysfs_index is UINT32_MAX when unknown. */
    dpu_rank_topology_t rank;
    char detail[DPU_FAULT_DETAIL_LEN];
} dpu_fault_t;

typedef struct {
    dpu_fault_t *faults;
    size_t nr_faults;
    size_t capacity;
    uint32_t nr_dpus_checked;
} dpu_fault_report_t;

typedef struct {
    /* MRAM bytes exercised per DPU; rounded up to 16 KiB. Default 4 MiB,
     * max 60 MiB. Some faults only appear under sustained traffic. */
    uint32_t mram_bytes;
    /* Kernel timeout. Default 10000 ms. */
    uint32_t timeout_ms;
} dpu_health_options_t;

void dpu_fault_report_init(dpu_fault_report_t *report);
void dpu_fault_report_free(dpu_fault_report_t *report);

/* Number of distinct faulty DPUs (a DPU may carry several faults). */
size_t dpu_fault_report_nr_dpus(const dpu_fault_report_t *report);
const char *dpu_fault_kind_name(dpu_fault_kind_t kind);

/*
 * Static scan of every rank in sysfs: masked CIs, VPD-disabled DPUs and
 * VPD SRAM repairs. Needs no allocation and does not touch the hardware.
 */
int dpu_faults_scan_system(dpu_fault_report_t *report);

/* Lists DPUs of the allocated set's ranks that the SDK left disabled. */
int dpu_faults_scan_set(struct dpu_set_t set, dpu_fault_report_t *report);

/*
 * Active test of every enabled DPU in the set: MRAM write/readback, then a
 * 16-tasklet kernel whose output and checksums are verified on the host.
 * This loads a program and overwrites MRAM, so run it before loading the
 * application's own program. options may be NULL.
 *
 * Returns 0 when the check ran (faults, if any, are in the report),
 * -ETIMEDOUT when some DPUs hung (they are reported as DPU_FAULT_TIMEOUT
 * and the set should be abandoned rather than freed), or a negative errno.
 */
int dpu_faults_health_check(struct dpu_set_t set,
                            const dpu_health_options_t *options,
                            dpu_fault_report_t *report);

/*
 * Records a fault found by the application. dpu is a single-DPU set as
 * produced by DPU_FOREACH(set, dpu, set_index); pass DPU_FAULT_NO_INDEX if
 * the index is unknown. detail may be NULL.
 */
int dpu_faults_add(dpu_fault_report_t *report, struct dpu_set_t dpu,
                   uint32_t set_index, const char *detail);

/*
 * Prints "no DPU faults" or the faulty DPUs grouped by rank, with the
 * DIMM, socket, NUMA node and channel of each rank.
 */
void dpu_fault_report_fprint(FILE *stream, const dpu_fault_report_t *report);

#ifdef __cplusplus
}
#endif

#endif
