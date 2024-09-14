#include <stdint.h>
#include <stdbool.h>
#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <xhyve/support/misc.h>

/* -------------- APIC is GIC
If you are not initially concerned with holding the GIC registers and state, you can define an empty struct gic_page. However, if you plan to implement or simulate GIC behavior, you'll need to define this structure with relevant fields.

Basic Empty Definition
For the initial phase, you can define struct gic_page as empty:

c
Copy code
struct gic_page {
    // Initially empty if not managing GIC state
};
Future Considerations
When you are ready to implement GIC handling, you might include:

GIC Registers: Fields for GIC registers, such as the Distributor and CPU interface registers.
Interrupt Handling State: Information related to active interrupts, pending interrupts, and priorities.
*/

struct gic_page {
//#if 0
    // Distributor registers
    uint32_t *gicd_ctlr;
    uint32_t *gicd_typer;
    // Add other Distributor registers as needed

    // CPU Interface registers
    uint32_t *gicc_ctlr;
    uint32_t *gicc_pmr;
    // Add other CPU Interface registers as needed

    // Other fields for interrupt state, configuration, etc.
//#endif
};

struct vmx {
    struct gic_page gic_page[VM_MAXCPU]; /* one GIC page per vcpu */
    //struct vmxcap cap[VM_MAXCPU];
    //struct vmxstate state[VM_MAXCPU];
    struct vm *vm;
};


