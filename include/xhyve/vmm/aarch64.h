#include <stdint.h>
#include <stdbool.h>
#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <xhyve/support/misc.h>

struct gic_page {
    // Distributor registers
    uint32_t *gicd_ctlr;
    uint32_t *gicd_typer;
    // Add other Distributor registers as needed

    // CPU Interface registers
    uint32_t *gicc_ctlr;
    uint32_t *gicc_pmr;
    // Add other CPU Interface registers as needed

    // Other fields for interrupt state, configuration, etc.
};

struct vmx {
    struct gic_page gic_page[VM_MAXCPU]; /* one GIC page per vcpu */
    struct vmxcap cap[VM_MAXCPU];
    struct vmxstate state[VM_MAXCPU];
    struct vm *vm;
};


