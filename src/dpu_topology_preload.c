#define _GNU_SOURCE

#include "dpu_topology.h"

#include <dlfcn.h>
#include <pthread.h>
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
        fputc('\n', stdout);
        fflush(stdout);
    }
    return status;
}
