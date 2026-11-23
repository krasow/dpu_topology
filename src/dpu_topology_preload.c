#define _GNU_SOURCE

#include "dpu_topology.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef dpu_error_t (*dpu_alloc_fn)(uint32_t, const char *,
                                    struct dpu_set_t *);

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static dpu_alloc_fn real_dpu_alloc;

static void
resolve_dpu_alloc(void)
{
    void *symbol = dlsym(RTLD_NEXT, "dpu_alloc");
    memcpy(&real_dpu_alloc, &symbol, sizeof(real_dpu_alloc));
}

static int
env_enabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && *value != '\0' && strcmp(value, "0") != 0;
}

/*
 * Always lists DPUs the SDK disabled (cheap). With DPU_TOPOLOGY_CHECK=1 it
 * also runs the active health check before the application loads its
 * program; DPU_TOPOLOGY_CHECK_MRAM_BYTES overrides the tested MRAM size.
 */
static void
report_faults(struct dpu_set_t *set)
{
    dpu_health_options_t options = {0, 0};
    dpu_fault_report_t report;
    int check = env_enabled("DPU_TOPOLOGY_CHECK");
    int status;

    dpu_fault_report_init(&report);
    status = dpu_faults_scan_set(*set, &report);
    if (status == 0 && check) {
        const char *bytes = getenv("DPU_TOPOLOGY_CHECK_MRAM_BYTES");
        if (bytes != NULL)
            options.mram_bytes = (uint32_t)strtoul(bytes, NULL, 0);
        status = dpu_faults_health_check(*set, &options, &report);
    }

    if (status != 0 && status != -ETIMEDOUT)
        fprintf(stderr, "DPU fault scan failed (error %d)\n", status);
    if (check || report.nr_faults != 0)
        dpu_fault_report_fprint(stdout, &report);
    if (status == -ETIMEDOUT)
        fprintf(stderr, "DPU health check: some DPUs hung; this allocation "
                        "is unlikely to work\n");
    dpu_fault_report_free(&report);
}

dpu_error_t
dpu_alloc(uint32_t nr_dpus, const char *profile, struct dpu_set_t *set)
{
    dpu_error_t status;

    pthread_once(&resolve_once, resolve_dpu_alloc);
    if (real_dpu_alloc == NULL)
        return DPU_ERR_INTERNAL;

    status = real_dpu_alloc(nr_dpus, profile, set);
    if (status == DPU_OK) {
        fputc('\n', stdout);
        int topology_status = allocation_fprint_overview(stdout, set);
        if (topology_status < 0)
            fprintf(stderr, "DPU topology unavailable (error %d)\n",
                    topology_status);
        report_faults(set);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return status;
}
