/*-
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2015 xhyve developers
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <Hypervisor/hv_vcpu.h>
#include <Hypervisor/hv_base.h>
#include <Hypervisor/hv_vm_types.h>
#include <xhyve/support/misc.h>
#include <xhyve/support/atomic.h>
#include <xhyve/support/psl.h>
#include <xhyve/support/specialreg.h>
#include <xhyve/vmm/vmm.h>
#include <xhyve/vmm/vmm_instruction_emul.h>
#include <xhyve/vmm/vmm_lapic.h>
#include <xhyve/vmm/vmm_host.h>
#include <xhyve/vmm/vmm_ktr.h>
#include <xhyve/vmm/vmm_stat.h>
#include <xhyve/vmm/io/vatpic.h>
#include <xhyve/vmm/io/vlapic.h>
#include <xhyve/vmm/io/vlapic_priv.h>
#include <xhyve/vmm/aarch64.h>
#include <xhyve/vmm/aarch64/vmcs.h>
#include <xhyve/vmm/aarch64/vmx_msr.h>
#include <xhyve/dtrace.h>

//#define PROCBASED_CTLS_WINDOW_SETTING \
//	(PROCBASED_INT_WINDOW_EXITING | \
//	 PROCBASED_NMI_WINDOW_EXITING)
#define PROCBASED_CTLS_ONE_SETTING \
	(PROCBASED_SECONDARY_CONTROLS | \
	 PROCBASED_MWAIT_EXITING | \
	 PROCBASED_MONITOR_EXITING | \
	 PROCBASED_IO_EXITING | \
	 PROCBASED_MSR_BITMAPS | \
	 //PROCBASED_CTLS_WINDOW_SETTING | \
	 PROCBASED_CR8_LOAD_EXITING | \
	 PROCBASED_CR8_STORE_EXITING | \
	 PROCBASED_HLT_EXITING | \
	 PROCBASED_TSC_OFFSET)
#define PROCBASED_CTLS_ZERO_SETTING \
	(PROCBASED_CR3_LOAD_EXITING | \
	 PROCBASED_CR3_STORE_EXITING | \
	 PROCBASED_IO_BITMAPS | \
	 PROCBASED_RDTSC_EXITING | \
	 PROCBASED_USE_TPR_SHADOW | \
	 PROCBASED_MOV_DR_EXITING | \
	 PROCBASED_MTF | \
	 PROCBASED_INVLPG_EXITING | \
	 PROCBASED_PAUSE_EXITING)
#define PROCBASED_CTLS2_ONE_SETTING \
	(PROCBASED2_ENABLE_EPT | \
	 PROCBASED2_UNRESTRICTED_GUEST | \
	 PROCBASED2_ENABLE_VPID | \
	 PROCBASED2_ENABLE_RDTSCP)
#define PROCBASED_CTLS2_ZERO_SETTING \
	(PROCBASED2_VIRTUALIZE_APIC_ACCESSES | \
	 PROCBASED2_DESC_TABLE_EXITING | \
	 PROCBASED2_WBINVD_EXITING | \
	 PROCBASED2_PAUSE_LOOP_EXITING /* FIXME */ | \
	 PROCBASED2_RDRAND_EXITING | \
	 PROCBASED2_ENABLE_INVPCID /* FIXME */ | \
	 PROCBASED2_RDSEED_EXITING | \
	 PROCBASED2_VMCS_SHADOW | \
	 PROCBASED2_XSAVES)
#define PINBASED_CTLS_ONE_SETTING \
	(PINBASED_EXTINT_EXITING | \
	 PINBASED_NMI_EXITING | \
	 PINBASED_VIRTUAL_NMI)
#define PINBASED_CTLS_ZERO_SETTING \
	(PINBASED_PREMPTION_TIMER)
#define VM_ENTRY_CTLS_ONE_SETTING \
	(VM_ENTRY_LOAD_EFER)
#define VM_ENTRY_CTLS_ZERO_SETTING \
	(VM_ENTRY_INTO_SMM | \
	 VM_ENTRY_DEACTIVATE_DUAL_MONITOR | \
	 VM_ENTRY_GUEST_LMA)
#define  VM_EXIT_CTLS_ONE_SETTING \
	(VM_EXIT_HOST_LMA | \
	 VM_EXIT_LOAD_EFER)
#define VM_EXIT_CTLS_ZERO_SETTING \
	(VM_EXIT_SAVE_PREEMPTION_TIMER)
#define NMI_BLOCKING \
	(VMCS_INTERRUPTIBILITY_NMI_BLOCKING | \
	 VMCS_INTERRUPTIBILITY_MOVSS_BLOCKING)
#define HWINTR_BLOCKING \
	(VMCS_INTERRUPTIBILITY_STI_BLOCKING | \
	 VMCS_INTERRUPTIBILITY_MOVSS_BLOCKING)

#define	HANDLED		1
#define	UNHANDLED	0
#define HV_VCPU_DEFAULT 1

static uint32_t pinbased_ctls, procbased_ctls, procbased_ctls2;
static uint32_t exit_ctls, entry_ctls;
static uint64_t cr0_ones_mask, cr0_zeros_mask;
static uint64_t cr4_ones_mask, cr4_zeros_mask;

/*
 * Optional capabilities
 */

static int cap_halt_exit;
static int cap_pause_exit;
// static int cap_unrestricted_guest;
static int cap_monitor_trap;
// static int cap_invpcid;
// static int pirvec = -1;
// static struct unrhdr *vpid_unr;
// static u_int vpid_alloc_failed;

/*
 * Use the last page below 4GB as the APIC access address. This address is
 * occupied by the boot firmware so it is guaranteed that it will not conflict
 * with a page in system memory.
 */
// #define	APIC_ACCESS_ADDRESS	0xFFFFF000

static int vmx_getdesc(void *arg, int vcpu, enum vm_reg_name reg, struct seg_desc *desc);
static int vmx_setdesc(void *arg, int vcpu, enum vm_reg_name reg, struct seg_desc *desc);
static int vmx_getreg(void *arg, int vcpu, enum vm_reg_name reg, uint64_t *retval);
static int vmx_setreg(void *arg, int vcpu, enum vm_reg_name reg, uint64_t val);

/* FIXME - Should be read from hv.h, investigate */
typedef uint32_t hv_vcpuid_t;


//static __inline uint64_t
//reg_read(int vcpuid, hv_reg_t reg) {
//	uint64_t val;

//	hv_vcpu_read_register(((hv_vcpuid_t) vcpuid), reg, &val);
//	return val;
//}

static __inline uint64_t
reg_read(int vcpuid, hv_reg_t reg) {
    uint64_t value;
    
    /* ChatGPT suggested checking for failure here */
    if (hv_vcpu_get_reg((hv_vcpuid_t)vcpuid, reg, &value) != 0) {
        // Handle error, such as logging or return an error code
        printf("reg_read failure\n");
        return 0;
    }
    return value;
}

static __inline void
reg_write(int vcpuid, hv_reg_t reg, uint64_t val) {
	hv_vcpu_write_register(((hv_vcpuid_t) vcpuid), reg, val);
}

static void hvdump(int vcpu) {
    // Reading system registers with hv_sys_reg_t type
    printf("HV_SYS_REG_TTBR0_EL1:          0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_TTBR0_EL1));
    printf("HV_SYS_REG_TTBR1_EL1:          0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_TTBR1_EL1));
    printf("HV_SYS_REG_TCR_EL1:            0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_TCR_EL1));
    printf("HV_SYS_REG_ESR_EL1:            0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_ESR_EL1));
    printf("HV_SYS_REG_SPSR_EL1:           0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_SPSR_EL1));
    printf("HV_SYS_REG_ELR_EL1:            0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_ELR_EL1));
    printf("HV_SYS_REG_SP_EL0:             0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_SP_EL0));
    printf("HV_SYS_REG_SP_EL1:             0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_SP_EL1));
    printf("HV_SYS_REG_VBAR_EL1:           0x%016llx\n", 
           reg_read(vcpu, (hv_reg_t)HV_SYS_REG_VBAR_EL1));
   // printf("HV_SYS_REG_ACTLR_EL1:          0x%016llx\n", 
     //      reg_read(vcpu, (hv_reg_t)HV_SYS_REG_ACTLR_EL1));
    
    // Reading general-purpose registers with hv_reg_t type
    printf("x0:  0x%016llx  x1:  0x%016llx  x2:  0x%016llx  x3:  0x%016llx\n",
           reg_read(vcpu, HV_REG_X0), reg_read(vcpu, HV_REG_X1),
           reg_read(vcpu, HV_REG_X2), reg_read(vcpu, HV_REG_X3));
    printf("x4:  0x%016llx  x5:  0x%016llx  x6:  0x%016llx  x7:  0x%016llx\n",
           reg_read(vcpu, HV_REG_X4), reg_read(vcpu, HV_REG_X5),
           reg_read(vcpu, HV_REG_X6), reg_read(vcpu, HV_REG_X7));
    printf("x8:  0x%016llx  x9:  0x%016llx  x10: 0x%016llx  x11: 0x%016llx\n",
           reg_read(vcpu, HV_REG_X8), reg_read(vcpu, HV_REG_X9),
           reg_read(vcpu, HV_REG_X10), reg_read(vcpu, HV_REG_X11));
    printf("x12: 0x%016llx  x13: 0x%016llx  x14: 0x%016llx  x15: 0x%016llx\n",
           reg_read(vcpu, HV_REG_X12), reg_read(vcpu, HV_REG_X13),
           reg_read(vcpu, HV_REG_X14), reg_read(vcpu, HV_REG_X15));
    printf("x16: 0x%016llx  x17: 0x%016llx  x18: 0x%016llx  x19: 0x%016llx\n",
           reg_read(vcpu, HV_REG_X16), reg_read(vcpu, HV_REG_X17),
           reg_read(vcpu, HV_REG_X18), reg_read(vcpu, HV_REG_X19));
    printf("x20: 0x%016llx  x21: 0x%016llx  x22: 0x%016llx  x23: 0x%016llx\n",
           reg_read(vcpu, HV_REG_X20), reg_read(vcpu, HV_REG_X21),
           reg_read(vcpu, HV_REG_X22), reg_read(vcpu, HV_REG_X23));
    printf("x24: 0x%016llx  x25: 0x%016llx  x26: 0x%016llx  x27: 0x%016llx\n",
           reg_read(vcpu, HV_REG_X24), reg_read(vcpu, HV_REG_X25),
           reg_read(vcpu, HV_REG_X26), reg_read(vcpu, HV_REG_X27));
    //printf("x28: 0x%016llx  fp:  0x%016llx  lr:  0x%016llx  sp_el0:  0x%016llx\n",
      //     reg_read(vcpu, HV_REG_X28), reg_read(vcpu, HV_REG_FP));
}


#ifdef XHYVE_CONFIG_TRACE
static const char *
exit_reason_to_str(int reason)
{
	static char reasonbuf[32];

	switch (reason) {
	case EXIT_REASON_EXCEPTION:
		return "exception";
	case EXIT_REASON_EXT_INTR:
		return "extint";
	case EXIT_REASON_TRIPLE_FAULT:
		return "triplefault";
	case EXIT_REASON_INIT:
		return "init";
	case EXIT_REASON_SIPI:
		return "sipi";
	case EXIT_REASON_IO_SMI:
		return "iosmi";
	case EXIT_REASON_SMI:
		return "smi";
	case EXIT_REASON_INTR_WINDOW:
		return "intrwindow";
	case EXIT_REASON_NMI_WINDOW:
		return "nmiwindow";
	case EXIT_REASON_TASK_SWITCH:
		return "taskswitch";
	case EXIT_REASON_CPUID:
		return "cpuid";
	case EXIT_REASON_GETSEC:
		return "getsec";
	case EXIT_REASON_HLT:
		return "hlt";
	case EXIT_REASON_INVD:
		return "invd";
	case EXIT_REASON_INVLPG:
		return "invlpg";
	case EXIT_REASON_RDPMC:
		return "rdpmc";
	case EXIT_REASON_RDTSC:
		return "rdtsc";
	case EXIT_REASON_RSM:
		return "rsm";
	case EXIT_REASON_VMCALL:
		return "vmcall";
	case EXIT_REASON_VMCLEAR:
		return "vmclear";
	case EXIT_REASON_VMLAUNCH:
		return "vmlaunch";
	case EXIT_REASON_VMPTRLD:
		return "vmptrld";
	case EXIT_REASON_VMPTRST:
		return "vmptrst";
	case EXIT_REASON_VMREAD:
		return "vmread";
	case EXIT_REASON_VMRESUME:
		return "vmresume";
	case EXIT_REASON_VMWRITE:
		return "vmwrite";
	case EXIT_REASON_VMXOFF:
		return "vmxoff";
	case EXIT_REASON_VMXON:
		return "vmxon";
	case EXIT_REASON_CR_ACCESS:
		return "craccess";
	case EXIT_REASON_DR_ACCESS:
		return "draccess";
	case EXIT_REASON_INOUT:
		return "inout";
	case EXIT_REASON_RDMSR:
		return "rdmsr";
	case EXIT_REASON_WRMSR:
		return "wrmsr";
	case EXIT_REASON_INVAL_VMCS:
		return "invalvmcs";
	case EXIT_REASON_INVAL_MSR:
		return "invalmsr";
	case EXIT_REASON_MWAIT:
		return "mwait";
	case EXIT_REASON_MTF:
		return "mtf";
	case EXIT_REASON_MONITOR:
		return "monitor";
	case EXIT_REASON_PAUSE:
		return "pause";
	case EXIT_REASON_MCE_DURING_ENTRY:
		return "mce-during-entry";
	case EXIT_REASON_TPR:
		return "tpr";
	case EXIT_REASON_APIC_ACCESS:
		return "apic-access";
	case EXIT_REASON_GDTR_IDTR:
		return "gdtridtr";
	case EXIT_REASON_LDTR_TR:
		return "ldtrtr";
	case EXIT_REASON_EPT_FAULT:
		return "eptfault";
	case EXIT_REASON_EPT_MISCONFIG:
		return "eptmisconfig";
	case EXIT_REASON_INVEPT:
		return "invept";
	case EXIT_REASON_RDTSCP:
		return "rdtscp";
	case EXIT_REASON_VMX_PREEMPT:
		return "vmxpreempt";
	case EXIT_REASON_INVVPID:
		return "invvpid";
	case EXIT_REASON_WBINVD:
		return "wbinvd";
	case EXIT_REASON_XSETBV:
		return "xsetbv";
	case EXIT_REASON_APIC_WRITE:
		return "apic-write";
	default:
		snprintf(reasonbuf, sizeof(reasonbuf), "%d", reason);
		return (reasonbuf);
	}
}
#endif	/* XHYVE_CONFIG_TRACE */

u_long
vmx_fix_cr0(u_long cr0)
{
	return ((cr0 | cr0_ones_mask) & ~cr0_zeros_mask);
}

u_long
vmx_fix_cr4(u_long cr4)
{
	return ((cr4 | cr4_ones_mask) & ~cr4_zeros_mask);
}

static int
vmx_cleanup(void)
{
	return (0);
}

#include <Hypervisor/Hypervisor.h>

// Function prototype for creating a VM
//int hv_vm_create(hv_vm *vm);

//hv_return_t
//hv_vm_create(hv_vm_config_t _Nullable config);


#include <stdio.h>

#include <sys/sysctl.h>
#include <stdio.h>
#include <stdint.h>

// Function to compute the number of virtual CPUs
int compute_cpu_count() {
    int total_available_cpus;
    size_t size = sizeof(total_available_cpus);
    int result = sysctlbyname("hw.ncpu", &total_available_cpus, &size, NULL, 0);
    if (result != 0) {
        perror("sysctlbyname(hw.ncpu) failed");
        return -1; // Indicate an error
    }

    int virtual_cpu_count = total_available_cpus <= 1 ? 1 : total_available_cpus - 1;
    // Example min/max values, adjust according to actual constraints
    const int MIN_CPU_COUNT = 1;
    const int MAX_CPU_COUNT = 32;
    virtual_cpu_count = virtual_cpu_count < MIN_CPU_COUNT ? MIN_CPU_COUNT : virtual_cpu_count;
    virtual_cpu_count = virtual_cpu_count > MAX_CPU_COUNT ? MAX_CPU_COUNT : virtual_cpu_count;

    return virtual_cpu_count;
}


#include <stdint.h>

// Function to compute memory size
uint64_t compute_memory_size() {
    // Example: Set to 4 GB
    uint64_t memory_size = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    
    // Example min/max values, adjust according to actual constraints
    const uint64_t MIN_MEMORY_SIZE = 1ULL * 1024ULL * 1024ULL * 1024ULL; // 1 GB
    const uint64_t MAX_MEMORY_SIZE = 64ULL * 1024ULL * 1024ULL * 1024ULL; // 64 GB

    memory_size = memory_size < MIN_MEMORY_SIZE ? MIN_MEMORY_SIZE : memory_size;
    memory_size = memory_size > MAX_MEMORY_SIZE ? MAX_MEMORY_SIZE : memory_size;

    return memory_size;
}

#if 0
// Define the types for VM context and reference
typedef struct hv_vm *hv_vm_ref; // Pointer to VM context
// Define a structure to hold VM context
struct hv_vm {
    hv_vm_ref vm_ref; // Hypothetical reference to the VM context
};

// Function to create a VM context
// Parameters:
// - vm_ref: Pointer to a variable where the VM reference will be stored
// Returns: 0 on success, non-zero on failure
int hv_create_vm_context(hv_vm_ref *vm_ref) {
    if (vm_ref == NULL) {
        return -1; // Return error if the provided reference is NULL
    }

    // Create a VM context using the Hypervisor framework
    hv_vm vm = NULL;
    int result = hv_vm_create(&vm); // Create VM context

    if (result != HV_SUCCESS) {
        return result; // Return error code if VM creation fails
    }

    *vm_ref = vm; // Store the created VM context in the provided reference
    return 0; // Return success
}

// Function prototype for creating a VM
//int hv_vm_create(hv_vm *vm);
hv_return_t
hv_vm_create(hv_vm_config_t _Nullable config)
{
    // Check if the provided pointer is valid
    if (vm == NULL) {
        return -1; // Invalid argument
    }

    // Hypothetical API call to create a VM context
    // In practice, this would involve calling Hypervisor.framework APIs
    int result = hv_create_vm_context(&vm->vm_ref); // Example function

    // Check for errors in VM creation
    if (result != HV_SUCCESS) {
        return result; // Return the error code from the API
    }

    // Additional setup and configuration could go here

    return HV_SUCCESS; // Success
}

// Hypothetical API to create a VM context (for illustration purposes)
int hv_create_vm_context(hv_vm_ref *vm_ref)
{
    // Simulate successful VM creation
    // In practice, this function would interact with the Hypervisor.framework
    *vm_ref = 1; // Example VM reference ID
    return HV_SUCCESS;
}

// Example usage
int main(void)
{
    hv_vm my_vm;
    int error = hv_vm_create(&my_vm);

    if (error != HV_SUCCESS) {
        printf("Error creating VM: %d\n", error);
        return error;
    }

    printf("VM created successfully!\n");
    return 0;
}

// Hypothetical structures for VM and vCPU context
typedef struct {
    // VM context structure
    void *vm_ref; // Reference to the VM context
} hv_vm;

typedef struct {
    // vCPU context structure
    int id;
    void *vcpu_ref; // Reference to the vCPU context
} hv_vcpu;

// Hypothetical function to create a vCPU

hv_return_t
hv_vcpu_create(hv_vcpu_t *vcpu, hv_vcpu_exit_t * _Nullable * _Nonnull exit,
    hv_vcpu_config_t _Nullable config);

    if (!vm || !vcpu) {
        return -1; // Invalid arguments
    }

    // Hypothetical API call to create a vCPU context
    // In practice, this would involve calling Hypervisor.framework APIs
    int result = create_vcpu_context(vm->vm_ref, vcpu_id, &vcpu->vcpu_ref);
    if (result != 0) {
        return -1; // Error creating vCPU context
    }

    vcpu->id = vcpu_id;
    return 0; // Success
}
#endif

// Hypothetical function to simulate creating a vCPU context
int create_vcpu_context(void *vm_ref, int vcpu_id, void **vcpu_ref) {
    // Simulate creation of vCPU context
    *vcpu_ref = malloc(sizeof(void*)); // Allocate memory for vCPU context
    if (!*vcpu_ref) {
        return -1; // Memory allocation failure
    }
    // Initialize vCPU context here if needed
    return 0; // Success
}

//typedef struct hv_vm *hv_vm_ref; // Pointer to VM context
// Define a structure to hold VM context
//struct hv_vm {
//    hv_vm_ref vm_ref; // Hypothetical reference to the VM context
//};   

static int
vmx_init(void)
{
/*
    hv_vm my_vm;
    hv_vcpu my_vcpu;
    int vcpu_id = 0; // Example vCPU ID

    // Initialize VM context (omitted for brevity)
    // ...

    // Create a vCPU
    if (hv_vcpu_create(&my_vm, vcpu_id, &my_vcpu) == 0) {
        printf("vCPU %d created successfully.\n", vcpu_id);
    } else {
        printf("Failed to create vCPU %d.\n", vcpu_id);
    }

	int error = hv_vm_create(HV_VM_DEFAULT);
	switch (error) {
		case HV_SUCCESS:
			break;
		case HV_NO_DEVICE:
			printf("vmx_init: processor not supported by "
			       "Hypervisor.framework\n");
			return (error);
		case HV_BAD_ARGUMENT:
			xhyve_abort("hv_vm_create HV_BAD_ARGUMENT\n");
		case HV_BUSY:
			xhyve_abort("hv_vm_create HV_BUSY\n");
		case HV_NO_RESOURCES:
			xhyve_abort("hv_vm_create HV_NO_RESOURCES\n");
		case HV_UNSUPPORTED:
			xhyve_abort("hv_vm_create HV_UNSUPPORTED\n");
		case HV_ERROR:
			xhyve_abort("hv_vm_create HV_ERROR (unspecified error)\n");
		default:
			xhyve_abort("hv_vm_create unknown error %#010x\n", error);
	}
*/
	return (0);
}



static void *
vmx_vm_init(struct vm *vm)
{
	struct vmx *vmx;

	vmx = malloc(sizeof(struct vmx));
	assert(vmx);
	bzero(vmx, sizeof(struct vmx));
	vmx->vm = vm;

	return (vmx);
}

static int
vmx_vcpu_init(void *arg, int vcpuid) {
        int error = 0;
#if 0
	uint32_t exc_bitmap;
	struct vmx *vmx;
	hv_vcpuid_t hvid;

	vmx = (struct vmx *) arg;

	if (hv_vcpu_create(&hvid, HV_VCPU_DEFAULT)) {
		xhyve_abort("hv_vcpu_create failed\n");
	}

	if (hvid != ((hv_vcpuid_t) vcpuid)) {
		/* FIXME */
		xhyve_abort("vcpu id mismatch\n");
	}

	if (hv_vcpu_enable_native_msr(hvid, MSR_GSBASE, 1) ||
		hv_vcpu_enable_native_msr(hvid, MSR_FSBASE, 1) ||
		hv_vcpu_enable_native_msr(hvid, MSR_SYSENTER_CS_MSR, 1) ||
		hv_vcpu_enable_native_msr(hvid, MSR_SYSENTER_ESP_MSR, 1) ||
		hv_vcpu_enable_native_msr(hvid, MSR_SYSENTER_EIP_MSR, 1) ||
		hv_vcpu_enable_native_msr(hvid, MSR_TSC, 1) ||
		hv_vcpu_enable_native_msr(hvid, MSR_IA32_TSC_AUX, 1))
	{
		xhyve_abort("vmx_vcpu_init: error setting guest msr access\n");
	}

	vmx_msr_guest_init(vmx, vcpuid);
	/* Check support for primary processor-based VM-execution controls */
	procbased_ctls = (uint32_t) vmcs_read(vcpuid, HV_VMX_CAP_PROCBASED);
	error = vmx_set_ctlreg(HV_VMX_CAP_PROCBASED,
			       PROCBASED_CTLS_ONE_SETTING,
			       PROCBASED_CTLS_ZERO_SETTING, &procbased_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired primary "
		       "processor-based controls\n");
		return (error);
	}

	/* Clear the processor-based ctl bits that are set on demand */
	procbased_ctls &= ~PROCBASED_CTLS_WINDOW_SETTING;

	/* Check support for secondary processor-based VM-execution controls */
	procbased_ctls2 = (uint32_t) vmcs_read(vcpuid, HV_VMX_CAP_PROCBASED2);
	error = vmx_set_ctlreg(HV_VMX_CAP_PROCBASED2,
			       PROCBASED_CTLS2_ONE_SETTING,
			       PROCBASED_CTLS2_ZERO_SETTING, &procbased_ctls2);
	if (error) {
		printf("vmx_init: processor does not support desired secondary "
		       "processor-based controls\n");
		return (error);
	}

	/* Check support for pin-based VM-execution controls */
	pinbased_ctls = (uint32_t) vmcs_read(vcpuid, HV_VMX_CAP_PINBASED);
	error = vmx_set_ctlreg(HV_VMX_CAP_PINBASED,
			       PINBASED_CTLS_ONE_SETTING,
			       PINBASED_CTLS_ZERO_SETTING, &pinbased_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired "
		       "pin-based controls\n");
		return (error);
	}

	/* Check support for VM-exit controls */
	exit_ctls = (uint32_t) vmcs_read(vcpuid, HV_VMX_CAP_EXIT);
	error = vmx_set_ctlreg(HV_VMX_CAP_EXIT,
			       VM_EXIT_CTLS_ONE_SETTING,
			       VM_EXIT_CTLS_ZERO_SETTING,
			       &exit_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired "
		    "exit controls\n");
		return (error);
	}

	/* Check support for VM-entry controls */
	entry_ctls = (uint32_t) vmcs_read(vcpuid, HV_VMX_CAP_ENTRY);
	error = vmx_set_ctlreg(HV_VMX_CAP_ENTRY,
	    VM_ENTRY_CTLS_ONE_SETTING, VM_ENTRY_CTLS_ZERO_SETTING,
	    &entry_ctls);
#endif
	if (error) {
		printf("vmx_init: processor does not support desired "
		    "entry controls\n");
		return (error);
	}


	return (0);
}

static int
vmx_handle_cpuid(struct vm *vm, int vcpuid)
{
    uint64_t midr, mpidr, revidr;
    int error = 0;

    // Read ARM system registers for CPU identification
    midr = reg_read(vcpuid, (hv_reg_t)HV_SYS_REG_MIDR_EL1);    // Read MIDR_EL1 for CPU features
    mpidr = reg_read(vcpuid, (hv_reg_t)HV_SYS_REG_MPIDR_EL1);  // Read MPIDR_EL1 for multiprocessor info
    revidr = reg_read(vcpuid, (hv_reg_t)HV_SYS_REG_MIDR_EL1); // Read REVIDR_EL1 for revision info

    // Emulate CPUID equivalent behavior if needed
    error = arm_emulate_cpuid(vm, vcpuid, &midr, &mpidr, &revidr);
    if (error != 0) {
        return error;
    }

    // Write back the results to registers if needed
    reg_write(vcpuid, (hv_reg_t)HV_SYS_REG_MIDR_EL1, midr);
    reg_write(vcpuid, (hv_reg_t)HV_SYS_REG_MPIDR_EL1, mpidr);
    reg_write(vcpuid, (hv_reg_t)HV_SYS_REG_MIDR_EL1, revidr);

    return 0;  // Successful CPUID handling
}

static __inline void
vmx_run_trace(struct vmx *vmx, int vcpu)
{
	(void) vmx;
	(void) vcpu;
}

static __inline void
vmx_exit_trace(struct vmx *vmx, int vcpu, uint64_t rip, uint32_t exit_reason,
	       int handled)
{
	(void) vmx;
	(void) vcpu;
	(void) rip;
	(void) exit_reason;
	(void) handled;
}

static void
vmx_inject_interrupts(struct vmx *vmx, int vcpu, struct vlapic *vlapic,
    uint64_t guestrip)
{
#if 0
	int vector, need_nmi_exiting, extint_pending;
	uint64_t rflags, entryinfo;
	uint32_t gi, info;

	if (vmx->state[vcpu].nextrip != guestrip) {
		gi = (uint32_t) vmcs_read(vcpu, VMCS_GUEST_INTERRUPTIBILITY);
		if (gi & HWINTR_BLOCKING) {
			VCPU_CTR2(vmx->vm, vcpu, "Guest interrupt blocking "
			    "cleared due to rip change: %#llx/%#llx",
			    vmx->state[vcpu].nextrip, guestrip);
			gi &= ~HWINTR_BLOCKING;
			vmcs_write(vcpu, VMCS_GUEST_INTERRUPTIBILITY, gi);
		}
	}

	if (vm_entry_intinfo(vmx->vm, vcpu, &entryinfo)) {
		KASSERT((entryinfo & VMCS_INTR_VALID) != 0, ("%s: entry "
		    "intinfo is not valid: %#llx", __func__, entryinfo));

		info = (uint32_t) vmcs_read(vcpu, VMCS_ENTRY_INTR_INFO);
		KASSERT((info & VMCS_INTR_VALID) == 0, ("%s: cannot inject "
		     "pending exception: %#llx/%#x", __func__, entryinfo, info));

		info = (uint32_t) entryinfo;
		vector = info & 0xff;
		if (vector == IDT_BP || vector == IDT_OF) {
			/*
			 * VT-x requires #BP and #OF to be injected as software
			 * exceptions.
			 */
			info &= ~VMCS_INTR_T_MASK;
			info |= VMCS_INTR_T_SWEXCEPTION;
		}

		if (info & VMCS_INTR_DEL_ERRCODE)
			vmcs_write(vcpu, VMCS_ENTRY_EXCEPTION_ERROR, entryinfo >> 32);

		vmcs_write(vcpu, VMCS_ENTRY_INTR_INFO, info);
	}

	if (vm_nmi_pending(vmx->vm, vcpu)) {
		/*
		 * If there are no conditions blocking NMI injection then
		 * inject it directly here otherwise enable "NMI window
		 * exiting" to inject it as soon as we can.
		 *
		 * We also check for STI_BLOCKING because some implementations
		 * don't allow NMI injection in this case. If we are running
		 * on a processor that doesn't have this restriction it will
		 * immediately exit and the NMI will be injected in the
		 * "NMI window exiting" handler.
		 */
		need_nmi_exiting = 1;
		gi = (uint32_t) vmcs_read(vcpu, VMCS_GUEST_INTERRUPTIBILITY);
		if ((gi & (HWINTR_BLOCKING | NMI_BLOCKING)) == 0) {
			info = (uint32_t) vmcs_read(vcpu, VMCS_ENTRY_INTR_INFO);
			if ((info & VMCS_INTR_VALID) == 0) {
				vmx_inject_nmi(vmx, vcpu);
				need_nmi_exiting = 0;
			} else {
				VCPU_CTR1(vmx->vm, vcpu, "Cannot inject NMI "
				    "due to VM-entry intr info %#x", info);
			}
		} else {
			VCPU_CTR1(vmx->vm, vcpu, "Cannot inject NMI due to "
			    "Guest Interruptibility-state %#x", gi);
		}

		if (need_nmi_exiting)
			vmx_set_nmi_window_exiting(vmx, vcpu);
	}

	extint_pending = vm_extint_pending(vmx->vm, vcpu);

	/*
	 * If interrupt-window exiting is already in effect then don't bother
	 * checking for pending interrupts. This is just an optimization and
	 * not needed for correctness.
	 */
	if ((vmx->cap[vcpu].proc_ctls & PROCBASED_INT_WINDOW_EXITING) != 0) {
		VCPU_CTR0(vmx->vm, vcpu, "Skip interrupt injection due to "
		    "pending int_window_exiting");
		return;
	}

	if (!extint_pending) {
		/* Ask the local apic for a vector to inject */
		if (!vlapic_pending_intr(vlapic, &vector))
			return;

		/*
		 * From the Intel SDM, Volume 3, Section "Maskable
		 * Hardware Interrupts":
		 * - maskable interrupt vectors [16,255] can be delivered
		 *   through the local APIC.
		*/
		KASSERT(vector >= 16 && vector <= 255,
		    ("invalid vector %d from local APIC", vector));
	} else {
		/* Ask the legacy pic for a vector to inject */
		vatpic_pending_intr(vmx->vm, &vector);

		/*
		 * From the Intel SDM, Volume 3, Section "Maskable
		 * Hardware Interrupts":
		 * - maskable interrupt vectors [0,255] can be delivered
		 *   through the INTR pin.
		 */
		KASSERT(vector >= 0 && vector <= 255,
		    ("invalid vector %d from INTR", vector));
	}

	/* Check RFLAGS.IF and the interruptibility state of the guest */
	rflags = vmcs_read(vcpu, VMCS_GUEST_RFLAGS);
	if ((rflags & PSL_I) == 0) {
		VCPU_CTR2(vmx->vm, vcpu, "Cannot inject vector %d due to "
		    "rflags %#llx", vector, rflags);
		goto cantinject;
	}

	gi = (uint32_t) vmcs_read(vcpu, VMCS_GUEST_INTERRUPTIBILITY);
	if (gi & HWINTR_BLOCKING) {
		VCPU_CTR2(vmx->vm, vcpu, "Cannot inject vector %d due to "
		    "Guest Interruptibility-state %#x", vector, gi);
		goto cantinject;
	}

	info = (uint32_t) vmcs_read(vcpu, VMCS_ENTRY_INTR_INFO);
	if (info & VMCS_INTR_VALID) {
		/*
		 * This is expected and could happen for multiple reasons:
		 * - A vectoring VM-entry was aborted due to astpending
		 * - A VM-exit happened during event injection.
		 * - An exception was injected above.
		 * - An NMI was injected above or after "NMI window exiting"
		 */
		VCPU_CTR2(vmx->vm, vcpu, "Cannot inject vector %d due to "
		    "VM-entry intr info %#x", vector, info);
		goto cantinject;
	}

	/* Inject the interrupt */
	info = VMCS_INTR_T_HWINTR | VMCS_INTR_VALID;
	info |= (uint32_t) vector;
	vmcs_write(vcpu, VMCS_ENTRY_INTR_INFO, info);

	if (!extint_pending) {
		/* Update the Local APIC ISR */
		vlapic_intr_accepted(vlapic, vector);
	} else {
		vm_extint_clear(vmx->vm, vcpu);
		vatpic_intr_accepted(vmx->vm, vector);

		/*
		 * After we accepted the current ExtINT the PIC may
		 * have posted another one.  If that is the case, set
		 * the Interrupt Window Exiting execution control so
		 * we can inject that one too.
		 *
		 * Also, interrupt window exiting allows us to inject any
		 * pending APIC vector that was preempted by the ExtINT
		 * as soon as possible. This applies both for the software
		 * emulated vlapic and the hardware assisted virtual APIC.
		 */
		vmx_set_int_window_exiting(vmx, vcpu);
	}

	HYPERKIT_VMX_INJECT_VIRQ(vcpu, vector);
	VCPU_CTR1(vmx->vm, vcpu, "Injecting hwintr at vector %d", vector);

	return;

cantinject:
	/*
	 * Set the Interrupt Window Exiting execution control so we can inject
	 * the interrupt as soon as blocking condition goes away.
	 */
	vmx_set_int_window_exiting(vmx, vcpu);
#endif
	return;
}

#if 0
/*
 * If the Virtual NMIs execution control is '1' then the logical processor
 * tracks virtual-NMI blocking in the Guest Interruptibility-state field of
 * the VMCS. An IRET instruction in VMX non-root operation will remove any
 * virtual-NMI blocking.
 *
 * This unblocking occurs even if the IRET causes a fault. In this case the
 * hypervisor needs to restore virtual-NMI blocking before resuming the guest.
 */
static void
vmx_restore_nmi_blocking(struct vmx *vmx, int vcpuid)
{
	uint32_t gi;

	VCPU_CTR0(vmx->vm, vcpuid, "Restore Virtual-NMI blocking");
	gi = (uint32_t) vmcs_read(vcpuid, VMCS_GUEST_INTERRUPTIBILITY);
	gi |= VMCS_INTERRUPTIBILITY_NMI_BLOCKING;
	vmcs_write(vcpuid, VMCS_GUEST_INTERRUPTIBILITY, gi);
}

static void
vmx_clear_nmi_blocking(struct vmx *vmx, int vcpuid)
{
	uint32_t gi;

	VCPU_CTR0(vmx->vm, vcpuid, "Clear Virtual-NMI blocking");
	gi = (uint32_t) vmcs_read(vcpuid, VMCS_GUEST_INTERRUPTIBILITY);
	gi &= ~VMCS_INTERRUPTIBILITY_NMI_BLOCKING;
	vmcs_write(vcpuid, VMCS_GUEST_INTERRUPTIBILITY, gi);
}

static void
vmx_assert_nmi_blocking(int vcpuid)
{
	uint32_t gi;

	gi = (uint32_t) vmcs_read(vcpuid, VMCS_GUEST_INTERRUPTIBILITY);
	KASSERT(gi & VMCS_INTERRUPTIBILITY_NMI_BLOCKING,
	    ("NMI blocking is not in effect %#x", gi));
}

static int
vmx_emulate_xsetbv(struct vmx *vmx, int vcpu)
{
	uint64_t xcrval;
	const struct xsave_limits *limits;

	limits = vmm_get_xsave_limits();

	/*
	 * Note that the processor raises a GP# fault on its own if
	 * xsetbv is executed for CPL != 0, so we do not have to
	 * emulate that fault here.
	 */

	/* Only xcr0 is supported. */
	if (reg_read(vcpu, HV_X86_RCX) != 0) {
		vm_inject_gp(vmx->vm, vcpu);
		return (HANDLED);
	}

	/* We only handle xcr0 if both the host and guest have XSAVE enabled. */
	if (!limits->xsave_enabled ||
		!(vmcs_read(vcpu, VMCS_GUEST_CR4) & CR4_XSAVE))
	{
		vm_inject_ud(vmx->vm, vcpu);
		return (HANDLED);
	}

	xcrval = reg_read(vcpu, HV_X86_RDX) << 32
		| (reg_read(vcpu, HV_X86_RAX) & 0xffffffff);

	if ((xcrval & ~limits->xcr0_allowed) != 0) {
		vm_inject_gp(vmx->vm, vcpu);
		return (HANDLED);
	}

	if (!(xcrval & XFEATURE_ENABLED_X87)) {
		vm_inject_gp(vmx->vm, vcpu);
		return (HANDLED);
	}

	/* AVX (YMM_Hi128) requires SSE. */
	if (xcrval & XFEATURE_ENABLED_AVX &&
	    (xcrval & XFEATURE_AVX) != XFEATURE_AVX) {
		vm_inject_gp(vmx->vm, vcpu);
		return (HANDLED);
	}

	/*
	 * AVX512 requires base AVX (YMM_Hi128) as well as OpMask,
	 * ZMM_Hi256, and Hi16_ZMM.
	 */
	if (xcrval & XFEATURE_AVX512 &&
	    (xcrval & (XFEATURE_AVX512 | XFEATURE_AVX)) !=
	    (XFEATURE_AVX512 | XFEATURE_AVX)) {
		vm_inject_gp(vmx->vm, vcpu);
		return (HANDLED);
	}

	/*
	 * Intel MPX requires both bound register state flags to be
	 * set.
	 */
	if (((xcrval & XFEATURE_ENABLED_BNDREGS) != 0) !=
	    ((xcrval & XFEATURE_ENABLED_BNDCSR) != 0)) {
		vm_inject_gp(vmx->vm, vcpu);
		return (HANDLED);
	}

	reg_write(vcpu, HV_X86_XCR0, xcrval);
	return (HANDLED);
}

static uint64_t
vmx_get_guest_reg(int vcpu, int ident)
{
	switch (ident) {
	case 0:
		return (reg_read(vcpu, HV_X86_RAX));
	case 1:
		return (reg_read(vcpu, HV_X86_RCX));
	case 2:
		return (reg_read(vcpu, HV_X86_RDX));
	case 3:
		return (reg_read(vcpu, HV_X86_RBX));
	case 4:
		return (vmcs_read(vcpu, VMCS_GUEST_RSP));
	case 5:
		return (reg_read(vcpu, HV_X86_RBP));
	case 6:
		return (reg_read(vcpu, HV_X86_RSI));
	case 7:
		return (reg_read(vcpu, HV_X86_RDI));
	case 8:
		return (reg_read(vcpu, HV_X86_R8));
	case 9:
		return (reg_read(vcpu, HV_X86_R9));
	case 10:
		return (reg_read(vcpu, HV_X86_R10));
	case 11:
		return (reg_read(vcpu, HV_X86_R11));
	case 12:
		return (reg_read(vcpu, HV_X86_R12));
	case 13:
		return (reg_read(vcpu, HV_X86_R13));
	case 14:
		return (reg_read(vcpu, HV_X86_R14));
	case 15:
		return (reg_read(vcpu, HV_X86_R15));
	default:
		xhyve_abort("invalid vmx register %d", ident);
	}
}

static void
vmx_set_guest_reg(int vcpu, int ident, uint64_t regval)
{
	switch (ident) {
	case 0:
		reg_write(vcpu, HV_X86_RAX, regval);
		break;
	case 1:
		reg_write(vcpu, HV_X86_RCX, regval);
		break;
	case 2:
		reg_write(vcpu, HV_X86_RDX, regval);
		break;
	case 3:
		reg_write(vcpu, HV_X86_RBX, regval);
		break;
	case 4:
		vmcs_write(vcpu, VMCS_GUEST_RSP, regval);
		break;
	case 5:
		reg_write(vcpu, HV_X86_RBP, regval);
		break;
	case 6:
		reg_write(vcpu, HV_X86_RSI, regval);
		break;
	case 7:
		reg_write(vcpu, HV_X86_RDI, regval);
		break;
	case 8:
		reg_write(vcpu, HV_X86_R8, regval);
		break;
	case 9:
		reg_write(vcpu, HV_X86_R9, regval);
		break;
	case 10:
		reg_write(vcpu, HV_X86_R10, regval);
		break;
	case 11:
		reg_write(vcpu, HV_X86_R11, regval);
		break;
	case 12:
		reg_write(vcpu, HV_X86_R12, regval);
		break;
	case 13:
		reg_write(vcpu, HV_X86_R13, regval);
		break;
	case 14:
		reg_write(vcpu, HV_X86_R14, regval);
		break;
	case 15:
		reg_write(vcpu, HV_X86_R15, regval);
		break;
	default:
		xhyve_abort("invalid vmx register %d", ident);
	}
}

static int
vmx_emulate_cr0_access(UNUSED struct vm *vm, int vcpu, uint64_t exitqual)
{
	uint64_t crval, efer, entryctls, regval;
	// *pt;

	/* We only handle mov to %cr0 at this time */
	if ((exitqual & 0xf0) != 0x00)
		return (UNHANDLED);

	regval = vmx_get_guest_reg(vcpu, (exitqual >> 8) & 0xf);

	vmcs_write(vcpu, VMCS_CR0_SHADOW, regval);

	crval = regval | cr0_ones_mask;
	crval &= ~cr0_zeros_mask;
	// printf("cr0: v:0x%016llx 1:0x%08llx 0:0x%08llx v:0x%016llx\n",
	// 	regval, cr0_ones_mask, cr0_zeros_mask, crval);
	vmcs_write(vcpu, VMCS_GUEST_CR0, crval);

	efer = vmcs_read(vcpu, VMCS_GUEST_IA32_EFER);

	if (regval & CR0_PG) {
		/*
		 * If CR0.PG is 1 and EFER.LME is 1 then EFER.LMA and
		 * the "IA-32e mode guest" bit in VM-entry control must be
		 * equal.
		 */
		if (efer & EFER_LME) {
			efer |= EFER_LMA;
			vmcs_write(vcpu, VMCS_GUEST_IA32_EFER, efer);
			entryctls = vmcs_read(vcpu, VMCS_ENTRY_CTLS);
			entryctls |= VM_ENTRY_GUEST_LMA;
			vmcs_write(vcpu, VMCS_ENTRY_CTLS, entryctls);
		}

		// if (vmcs_read(vcpu, VMCS_GUEST_CR4) & CR4_PAE) {
		// 	if (!(pt = (uint64_t *) vm_gpa2hva(vm,
		// 		vmcs_read(vcpu, VMCS_GUEST_CR3), sizeof(uint64_t) * 4)))
		// 	{
		// 		xhyve_abort("invalid cr3\n");
		// 	}

		// 	vmcs_write(vcpu, VMCS_GUEST_PDPTE0, pt[0]);
		// 	vmcs_write(vcpu, VMCS_GUEST_PDPTE1, pt[1]);
		// 	vmcs_write(vcpu, VMCS_GUEST_PDPTE2, pt[2]);
		// 	vmcs_write(vcpu, VMCS_GUEST_PDPTE3, pt[3]);
		// }
	} else {
		/*
		 * If CR0.PG is 0 and EFER.LMA is 1, this is a
		 * switch out of IA32e mode so emulate that.
		 */
		if (efer & EFER_LMA) {
			efer &= ~(uint64_t)EFER_LMA;
			vmcs_write(vcpu, VMCS_GUEST_IA32_EFER, efer);
			entryctls = vmcs_read(vcpu, VMCS_ENTRY_CTLS);
			entryctls &= ~VM_ENTRY_GUEST_LMA;
			vmcs_write(vcpu, VMCS_ENTRY_CTLS, entryctls);
		}
	}

	return (HANDLED);
}

static int
vmx_emulate_cr4_access(int vcpu, uint64_t exitqual)
{
	uint64_t crval, regval;

	/* We only handle mov to %cr4 at this time */
	if ((exitqual & 0xf0) != 0x00)
		return (UNHANDLED);

	regval = vmx_get_guest_reg(vcpu, (exitqual >> 8) & 0xf);

	vmcs_write(vcpu, VMCS_CR4_SHADOW, regval);

	crval = regval | cr4_ones_mask;
	crval &= ~cr4_zeros_mask;
	vmcs_write(vcpu, VMCS_GUEST_CR4, crval);

	return (HANDLED);
}

static int
vmx_emulate_cr8_access(struct vmx *vmx, int vcpu, uint64_t exitqual)
{
	struct vlapic *vlapic;
	uint64_t cr8;
	int regnum;

	/* We only handle mov %cr8 to/from a register at this time. */
	if ((exitqual & 0xe0) != 0x00) {
		return (UNHANDLED);
	}

	vlapic = vm_lapic(vmx->vm, vcpu);
	regnum = (exitqual >> 8) & 0xf;
	if (exitqual & 0x10) {
		cr8 = vlapic_get_cr8(vlapic);
		vmx_set_guest_reg(vcpu, regnum, cr8);
	} else {
		cr8 = vmx_get_guest_reg(vcpu, regnum);
		vlapic_set_cr8(vlapic, cr8);
	}

	return (HANDLED);
}

/*
 * From section "Guest Register State" in the Intel SDM: CPL = SS.DPL
 */
static int
vmx_cpl(int vcpu)
{
	uint32_t ssar;

	ssar = (uint32_t) vmcs_read(vcpu, VMCS_GUEST_SS_ACCESS_RIGHTS);
	return ((ssar >> 5) & 0x3);
}
#endif

static enum vm_cpu_mode
vmx_cpu_mode(int vcpu)
{
#if 0
	uint32_t csar;

	if (vmcs_read(vcpu, VMCS_GUEST_IA32_EFER) & EFER_LMA) {
		csar = (uint32_t) vmcs_read(vcpu, VMCS_GUEST_CS_ACCESS_RIGHTS);
		if (csar & 0x2000)
			return (CPU_MODE_64BIT);	/* CS.L = 1 */
		else
			return (CPU_MODE_COMPATIBILITY);
	} else if (vmcs_read(vcpu, VMCS_GUEST_CR0) & CR0_PE) {
		return (CPU_MODE_PROTECTED);
	} else {
		return (CPU_MODE_REAL);
	}
#endif
	return 0;
}

static enum vm_paging_mode
vmx_paging_mode(int vcpu)
{
#if 0
	if (!(vmcs_read(vcpu, VMCS_GUEST_CR0) & CR0_PG))
		return (PAGING_MODE_FLAT);
	if (!(vmcs_read(vcpu, VMCS_GUEST_CR4) & CR4_PAE))
		return (PAGING_MODE_32);
	if (vmcs_read(vcpu, VMCS_GUEST_IA32_EFER) & EFER_LME)
		return (PAGING_MODE_64);
	else
		return (PAGING_MODE_PAE);
#endif
	return 0;
}

static uint64_t
inout_str_index(struct vmx *vmx, int vcpuid, int in)
{
	uint64_t val;
	int error;
	enum vm_reg_name reg;

	reg = in ? VM_REG_GUEST_RDI : VM_REG_GUEST_RSI;
	error = vmx_getreg(vmx, vcpuid, reg, &val);
	KASSERT(error == 0, ("%s: vmx_getreg error %d", __func__, error));
	return (val);
}

static uint64_t
inout_str_count(struct vmx *vmx, int vcpuid, int rep)
{
	uint64_t val;
	int error;

	if (rep) {
		error = vmx_getreg(vmx, vcpuid, VM_REG_GUEST_RCX, &val);
		KASSERT(!error, ("%s: vmx_getreg error %d", __func__, error));
	} else {
		val = 1;
	}
	return (val);
}

static int
inout_str_addrsize(uint32_t inst_info)
{
	uint32_t size;

	size = (inst_info >> 7) & 0x7;
	switch (size) {
	case 0:
		return (2);	/* 16 bit */
	case 1:
		return (4);	/* 32 bit */
	case 2:
		return (8);	/* 64 bit */
	default:
		xhyve_abort("%s: invalid size encoding %d", __func__, size);
	}
}

#if 0
static void
inout_str_seginfo(struct vmx *vmx, int vcpuid, uint32_t inst_info, int in,
    struct vm_inout_str *vis)
{
	int error, s;

	if (in) {
		vis->seg_name = VM_REG_GUEST_ES;
	} else {
		s = (inst_info >> 15) & 0x7;
		vis->seg_name = vm_segment_name(s);
	}

	error = vmx_getdesc(vmx, vcpuid, vis->seg_name, &vis->seg_desc);
	KASSERT(error == 0, ("%s: vmx_getdesc error %d", __func__, error));
}

static void
vmx_paging_info(struct vm_guest_paging *paging, int vcpu)
{
	paging->cr3 = vmcs_guest_cr3(vcpu);
	paging->cpl = vmx_cpl(vcpu);
	paging->cpu_mode = vmx_cpu_mode(vcpu);
	paging->paging_mode = vmx_paging_mode(vcpu);
}

static void
vmexit_inst_emul(struct vm_exit *vmexit, uint64_t gpa, uint64_t gla, int vcpu)
{
	struct vm_guest_paging *paging;
	uint32_t csar;

	paging = &vmexit->u.inst_emul.paging;

	vmexit->exitcode = VM_EXITCODE_INST_EMUL;
	vmexit->u.inst_emul.gpa = gpa;
	vmexit->u.inst_emul.gla = gla;
	vmx_paging_info(paging, vcpu);
	switch (paging->cpu_mode) {
	case CPU_MODE_REAL:
		vmexit->u.inst_emul.cs_base = vmcs_read(vcpu, VMCS_GUEST_CS_BASE);
		vmexit->u.inst_emul.cs_d = 0;
		break;
	case CPU_MODE_PROTECTED:
	case CPU_MODE_COMPATIBILITY:
		vmexit->u.inst_emul.cs_base = vmcs_read(vcpu, VMCS_GUEST_CS_BASE);
		csar = (uint32_t) vmcs_read(vcpu, VMCS_GUEST_CS_ACCESS_RIGHTS);
		vmexit->u.inst_emul.cs_d = SEG_DESC_DEF32(csar);
		break;
	case CPU_MODE_64BIT:
		vmexit->u.inst_emul.cs_base = 0;
		vmexit->u.inst_emul.cs_d = 0;
		break;
	}
	vie_init(&vmexit->u.inst_emul.vie, NULL, 0);
}

/* This just has to be wrong.... */
#define EPT_VIOLATION_DATA_WRITE 1

static int
ept_fault_type(uint64_t ept_qual)
{
	int fault_type;

	if (ept_qual & EPT_VIOLATION_DATA_WRITE)
		fault_type = XHYVE_PROT_WRITE;
	else if (ept_qual & EPT_VIOLATION_INST_FETCH)
		fault_type = XHYVE_PROT_EXECUTE;
	else
		fault_type= XHYVE_PROT_READ;

	return (fault_type);
}

static bool
ept_emulation_fault(uint64_t ept_qual)
{
	int read, write;

	/* EPT fault on an instruction fetch doesn't make sense here */
	if (ept_qual & EPT_VIOLATION_INST_FETCH)
		return (FALSE);

	/* EPT fault must be a read fault or a write fault */
	read = ept_qual & EPT_VIOLATION_DATA_READ ? 1 : 0;
	write = ept_qual & EPT_VIOLATION_DATA_WRITE ? 1 : 0;
	if ((read | write) == 0)
		return (FALSE);

	/*
	 * The EPT violation must have been caused by accessing a
	 * guest-physical address that is a translation of a guest-linear
	 * address.
	 */
	if ((ept_qual & EPT_VIOLATION_GLA_VALID) == 0 ||
	    (ept_qual & EPT_VIOLATION_XLAT_VALID) == 0) {
		return (FALSE);
	}

	return (TRUE);
}

static __inline int
apic_access_virtualization(struct vmx *vmx, int vcpuid)
{
	uint32_t proc_ctls2;

	proc_ctls2 = vmx->cap[vcpuid].proc_ctls2;
	return ((proc_ctls2 & PROCBASED2_VIRTUALIZE_APIC_ACCESSES) ? 1 : 0);
}

static __inline int
x2apic_virtualization(struct vmx *vmx, int vcpuid)
{
	uint32_t proc_ctls2;

	proc_ctls2 = vmx->cap[vcpuid].proc_ctls2;
	return ((proc_ctls2 & PROCBASED2_VIRTUALIZE_X2APIC_MODE) ? 1 : 0);
}

static int
vmx_handle_apic_write(struct vmx *vmx, int vcpuid, struct vlapic *vlapic,
    uint64_t qual)
{
	int error, handled, offset;
	uint32_t *apic_regs, vector;
	bool retu;

	handled = HANDLED;
	offset = APIC_WRITE_OFFSET(qual);

	if (!apic_access_virtualization(vmx, vcpuid)) {
		/*
		 * In general there should not be any APIC write VM-exits
		 * unless APIC-access virtualization is enabled.
		 *
		 * However self-IPI virtualization can legitimately trigger
		 * an APIC-write VM-exit so treat it specially.
		 */
		if (x2apic_virtualization(vmx, vcpuid) &&
		    offset == APIC_OFFSET_SELF_IPI) {
			apic_regs = (uint32_t *)(vlapic->apic_page);
			vector = apic_regs[APIC_OFFSET_SELF_IPI / 4];
			vlapic_self_ipi_handler(vlapic, vector);
			return (HANDLED);
		} else
			return (UNHANDLED);
	}

	switch (offset) {
	case APIC_OFFSET_ID:
		vlapic_id_write_handler(vlapic);
		break;
	case APIC_OFFSET_LDR:
		vlapic_ldr_write_handler(vlapic);
		break;
	case APIC_OFFSET_DFR:
		vlapic_dfr_write_handler(vlapic);
		break;
	case APIC_OFFSET_SVR:
		vlapic_svr_write_handler(vlapic);
		break;
	case APIC_OFFSET_ESR:
		vlapic_esr_write_handler(vlapic);
		break;
	case APIC_OFFSET_ICR_LOW:
		retu = false;
		error = vlapic_icrlo_write_handler(vlapic, &retu);
		if (error != 0 || retu)
			handled = UNHANDLED;
		break;
	case APIC_OFFSET_CMCI_LVT:
	case APIC_OFFSET_TIMER_LVT:
	case APIC_OFFSET_THERM_LVT:
	case APIC_OFFSET_PERF_LVT:
	case APIC_OFFSET_LINT0_LVT:
	case APIC_OFFSET_LINT1_LVT:
	case APIC_OFFSET_ERROR_LVT:
		vlapic_lvt_write_handler(vlapic, ((uint32_t) offset));
		break;
	case APIC_OFFSET_TIMER_ICR:
		vlapic_icrtmr_write_handler(vlapic);
		break;
	case APIC_OFFSET_TIMER_DCR:
		vlapic_dcr_write_handler(vlapic);
		break;
	default:
		handled = UNHANDLED;
		break;
	}
	return (handled);
}

static bool
apic_access_fault(struct vmx *vmx, int vcpuid, uint64_t gpa)
{

	if (apic_access_virtualization(vmx, vcpuid) &&
	    (gpa >= DEFAULT_APIC_BASE && gpa < DEFAULT_APIC_BASE + XHYVE_PAGE_SIZE))
		return (true);
	else
		return (false);
}

static int
vmx_handle_apic_access(struct vmx *vmx, int vcpuid, struct vm_exit *vmexit)
{
	uint64_t qual;
	int access_type, offset, allowed;

	if (!apic_access_virtualization(vmx, vcpuid))
		return (UNHANDLED);

	qual = vmexit->u.vmx.exit_qualification;
	access_type = APIC_ACCESS_TYPE(qual);
	offset = APIC_ACCESS_OFFSET(qual);

	allowed = 0;
	if (access_type == 0) {
		/*
		 * Read data access to the following registers is expected.
		 */
		switch (offset) {
		case APIC_OFFSET_APR:
		case APIC_OFFSET_PPR:
		case APIC_OFFSET_RRR:
		case APIC_OFFSET_CMCI_LVT:
		case APIC_OFFSET_TIMER_CCR:
			allowed = 1;
			break;
		default:
			break;
		}
	} else if (access_type == 1) {
		/*
		 * Write data access to the following registers is expected.
		 */
		switch (offset) {
		case APIC_OFFSET_VER:
		case APIC_OFFSET_APR:
		case APIC_OFFSET_PPR:
		case APIC_OFFSET_RRR:
		case APIC_OFFSET_ISR0:
		case APIC_OFFSET_ISR1:
		case APIC_OFFSET_ISR2:
		case APIC_OFFSET_ISR3:
		case APIC_OFFSET_ISR4:
		case APIC_OFFSET_ISR5:
		case APIC_OFFSET_ISR6:
		case APIC_OFFSET_ISR7:
		case APIC_OFFSET_TMR0:
		case APIC_OFFSET_TMR1:
		case APIC_OFFSET_TMR2:
		case APIC_OFFSET_TMR3:
		case APIC_OFFSET_TMR4:
		case APIC_OFFSET_TMR5:
		case APIC_OFFSET_TMR6:
		case APIC_OFFSET_TMR7:
		case APIC_OFFSET_IRR0:
		case APIC_OFFSET_IRR1:
		case APIC_OFFSET_IRR2:
		case APIC_OFFSET_IRR3:
		case APIC_OFFSET_IRR4:
		case APIC_OFFSET_IRR5:
		case APIC_OFFSET_IRR6:
		case APIC_OFFSET_IRR7:
		case APIC_OFFSET_CMCI_LVT:
		case APIC_OFFSET_TIMER_CCR:
			allowed = 1;
			break;
		default:
			break;
		}
	}

	if (allowed) {
		vmexit_inst_emul(vmexit, DEFAULT_APIC_BASE + ((uint32_t) offset),
		    VIE_INVALID_GLA, vcpuid);
	}

	/*
	 * Regardless of whether the APIC-access is allowed this handler
	 * always returns UNHANDLED:
	 * - if the access is allowed then it is handled by emulating the
	 *   instruction that caused the VM-exit (outside the critical section)
	 * - if the access is not allowed then it will be converted to an
	 *   exitcode of VM_EXITCODE_VMX and will be dealt with in userland.
	 */
	return (UNHANDLED);
}

static enum task_switch_reason
vmx_task_switch_reason(uint64_t qual)
{
	int reason;

	reason = (qual >> 30) & 0x3;
	switch (reason) {
	case 0:
		return (TSR_CALL);
	case 1:
		return (TSR_IRET);
	case 2:
		return (TSR_JMP);
	case 3:
		return (TSR_IDT_GATE);
	default:
		xhyve_abort("%s: invalid reason %d", __func__, reason);
	}
}

static int
emulate_wrmsr(struct vmx *vmx, int vcpuid, u_int num, uint64_t val, bool *retu)
{
	int error;

	HYPERKIT_VMX_WRITE_MSR(vcpuid, num, val);
	if (lapic_msr(num))
		error = lapic_wrmsr(vmx->vm, vcpuid, num, val, retu);
	else
		error = vmx_wrmsr(vmx, vcpuid, num, val);

	return (error);
}

static int
emulate_rdmsr(struct vmx *vmx, int vcpuid, u_int num, bool *retu)
{
	uint64_t result;
	uint32_t eax, edx;
	int error;

	if (lapic_msr(num))
		error = lapic_rdmsr(vmx->vm, vcpuid, num, &result, retu);
	else
		error = vmx_rdmsr(vmx, vcpuid, num, &result);

	if (error == 0) {
		eax = (uint32_t) result;
		reg_write(vcpuid, HV_X86_RAX, eax);
		edx = (uint32_t) (result >> 32);
		reg_write(vcpuid, HV_X86_RDX, edx);
	} else
		result = 0;
	HYPERKIT_VMX_READ_MSR(vcpuid, num, result);

	return (error);
}


static int
vmx_exit_process(struct vmx *vmx, int vcpu, struct vm_exit *vmexit)
{
	int error, errcode, errcode_valid, handled, in;
	struct vlapic *vlapic;
	struct vm_inout_str *vis;
	struct vm_task_switch *ts;
	uint32_t eax, ecx, edx, idtvec_info, idtvec_err, intr_info, inst_info;
	uint32_t intr_type, intr_vec, reason;
	uint64_t exitintinfo, qual, gpa;
	bool retu;

	CTASSERT((PINBASED_CTLS_ONE_SETTING & PINBASED_VIRTUAL_NMI) != 0);
	CTASSERT((PINBASED_CTLS_ONE_SETTING & PINBASED_NMI_EXITING) != 0);

	handled = UNHANDLED;

	qual = vmexit->u.vmx.exit_qualification;
	reason = vmexit->u.vmx.exit_reason;
	vmexit->exitcode = VM_EXITCODE_BOGUS;

	vmm_stat_incr(vmx->vm, vcpu, VMEXIT_COUNT, 1);
	HYPERKIT_VMX_EXIT(vcpu, reason);

	/*
	 * VM exits that can be triggered during event delivery need to
	 * be handled specially by re-injecting the event if the IDT
	 * vectoring information field's valid bit is set.
	 *
	 * See "Information for VM Exits During Event Delivery" in Intel SDM
	 * for details.
	 */
	idtvec_info = (uint32_t) vmcs_idt_vectoring_info(vcpu);
	if (idtvec_info & VMCS_IDT_VEC_VALID) {
		idtvec_info &= ~(1u << 12); /* clear undefined bit */
		exitintinfo = idtvec_info;
		if (idtvec_info & VMCS_IDT_VEC_ERRCODE_VALID) {
			idtvec_err = (uint32_t) vmcs_idt_vectoring_err(vcpu);
			exitintinfo |= (uint64_t)idtvec_err << 32;
		}
		error = vm_exit_intinfo(vmx->vm, vcpu, exitintinfo);
		KASSERT(error == 0, ("%s: vm_set_intinfo error %d",
		    __func__, error));

		/*
		 * If 'virtual NMIs' are being used and the VM-exit
		 * happened while injecting an NMI during the previous
		 * VM-entry, then clear "blocking by NMI" in the
		 * Guest Interruptibility-State so the NMI can be
		 * reinjected on the subsequent VM-entry.
		 *
		 * However, if the NMI was being delivered through a task
		 * gate, then the new task must start execution with NMIs
		 * blocked so don't clear NMI blocking in this case.
		 */
		intr_type = idtvec_info & VMCS_INTR_T_MASK;
		if (intr_type == VMCS_INTR_T_NMI) {
			if (reason != EXIT_REASON_TASK_SWITCH)
				vmx_clear_nmi_blocking(vmx, vcpu);
			else
				vmx_assert_nmi_blocking(vcpu);
		}

		/*
		 * Update VM-entry instruction length if the event being
		 * delivered was a software interrupt or software exception.
		 */
		if (intr_type == VMCS_INTR_T_SWINTR ||
		    intr_type == VMCS_INTR_T_PRIV_SWEXCEPTION ||
		    intr_type == VMCS_INTR_T_SWEXCEPTION) {
			vmcs_write(vcpu, VMCS_ENTRY_INST_LENGTH,
				((uint64_t) vmexit->inst_length));
		}
	}

	switch (reason) {
	case EXIT_REASON_TASK_SWITCH:
		ts = &vmexit->u.task_switch;
		ts->tsssel = qual & 0xffff;
		ts->reason = vmx_task_switch_reason(qual);
		ts->ext = 0;
		ts->errcode_valid = 0;
		vmx_paging_info(&ts->paging, vcpu);
		/*
		 * If the task switch was due to a CALL, JMP, IRET, software
		 * interrupt (INT n) or software exception (INT3, INTO),
		 * then the saved %rip references the instruction that caused
		 * the task switch. The instruction length field in the VMCS
		 * is valid in this case.
		 *
		 * In all other cases (e.g., NMI, hardware exception) the
		 * saved %rip is one that would have been saved in the old TSS
		 * had the task switch completed normally so the instruction
		 * length field is not needed in this case and is explicitly
		 * set to 0.
		 */
		if (ts->reason == TSR_IDT_GATE) {
			KASSERT(idtvec_info & VMCS_IDT_VEC_VALID,
			    ("invalid idtvec_info %#x for IDT task switch",
			    idtvec_info));
			intr_type = idtvec_info & VMCS_INTR_T_MASK;
			if (intr_type != VMCS_INTR_T_SWINTR &&
			    intr_type != VMCS_INTR_T_SWEXCEPTION &&
			    intr_type != VMCS_INTR_T_PRIV_SWEXCEPTION) {
				/* Task switch triggered by external event */
				ts->ext = 1;
				vmexit->inst_length = 0;
				if (idtvec_info & VMCS_IDT_VEC_ERRCODE_VALID) {
					ts->errcode_valid = 1;
					ts->errcode = (uint32_t) vmcs_idt_vectoring_err(vcpu);
				}
			}
		}
		vmexit->exitcode = VM_EXITCODE_TASK_SWITCH;
		VCPU_CTR4(vmx->vm, vcpu, "task switch reason %d, tss 0x%04x, "
		    "%s errcode 0x%016llx", ts->reason, ts->tsssel,
		    ts->ext ? "external" : "internal",
		    ((uint64_t)ts->errcode << 32) | ((uint64_t) ts->errcode_valid));
		break;
	case EXIT_REASON_CR_ACCESS:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_CR_ACCESS, 1);
		switch (qual & 0xf) {
		case 0:
			handled = vmx_emulate_cr0_access(vmx->vm, vcpu, qual);
			break;
		case 4:
			handled = vmx_emulate_cr4_access(vcpu, qual);
			break;
		case 8:
			handled = vmx_emulate_cr8_access(vmx, vcpu, qual);
			break;
		}
		break;
	case EXIT_REASON_RDMSR:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_RDMSR, 1);
		retu = false;
		ecx = (uint32_t) reg_read(vcpu, HV_X86_RCX);
		VCPU_CTR1(vmx->vm, vcpu, "rdmsr 0x%08x", ecx);
		error = emulate_rdmsr(vmx, vcpu, ecx, &retu);
		if (error) {
			vmexit->exitcode = VM_EXITCODE_RDMSR;
			vmexit->u.msr.code = ecx;
		} else if (!retu) {
			handled = HANDLED;
		} else {
			/* Return to userspace with a valid exitcode */
			KASSERT(vmexit->exitcode != VM_EXITCODE_BOGUS,
			    ("emulate_rdmsr retu with bogus exitcode"));
		}
		break;
	case EXIT_REASON_WRMSR:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_WRMSR, 1);
		retu = false;
		eax = (uint32_t) reg_read(vcpu, HV_X86_RAX);
		ecx = (uint32_t) reg_read(vcpu, HV_X86_RCX);
		edx = (uint32_t) reg_read(vcpu, HV_X86_RDX);
		VCPU_CTR2(vmx->vm, vcpu, "wrmsr 0x%08x value 0x%016llx",
		    ecx, (uint64_t)edx << 32 | eax);
		error = emulate_wrmsr(vmx, vcpu, ecx,
		    (uint64_t)edx << 32 | eax, &retu);
		if (error) {
			vmexit->exitcode = VM_EXITCODE_WRMSR;
			vmexit->u.msr.code = ecx;
			vmexit->u.msr.wval = (uint64_t)edx << 32 | eax;
		} else if (!retu) {
			handled = HANDLED;
		} else {
			/* Return to userspace with a valid exitcode */
			KASSERT(vmexit->exitcode != VM_EXITCODE_BOGUS,
			    ("emulate_wrmsr retu with bogus exitcode"));
		}
		break;
	case EXIT_REASON_HLT:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_HLT, 1);
		vmexit->exitcode = VM_EXITCODE_HLT;
		vmexit->u.hlt.rflags = vmcs_read(vcpu, VMCS_GUEST_RFLAGS);
		break;
	case EXIT_REASON_MTF:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_MTRAP, 1);
		vmexit->exitcode = VM_EXITCODE_MTRAP;
		vmexit->inst_length = 0;
		break;
	case EXIT_REASON_PAUSE:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_PAUSE, 1);
		vmexit->exitcode = VM_EXITCODE_PAUSE;
		break;
	case EXIT_REASON_INTR_WINDOW:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_INTR_WINDOW, 1);
		vmx_clear_int_window_exiting(vmx, vcpu);
		return (1);
	case EXIT_REASON_EXT_INTR:
		/*
		 * External interrupts serve only to cause VM exits and allow
		 * the host interrupt handler to run.
		 *
		 * If this external interrupt triggers a virtual interrupt
		 * to a VM, then that state will be recorded by the
		 * host interrupt handler in the VM's softc. We will inject
		 * this virtual interrupt during the subsequent VM enter.
		 */
		intr_info = (uint32_t) vmcs_read(vcpu, VMCS_EXIT_INTR_INFO);

		/*
		 * XXX: Ignore this exit if VMCS_INTR_VALID is not set.
		 * This appears to be a bug in VMware Fusion?
		 */
		if (!(intr_info & VMCS_INTR_VALID))
			return (1);
		KASSERT((intr_info & VMCS_INTR_VALID) != 0 &&
		    (intr_info & VMCS_INTR_T_MASK) == VMCS_INTR_T_HWINTR,
		    ("VM exit interruption info invalid: %#x", intr_info));

		/*
		 * This is special. We want to treat this as an 'handled'
		 * VM-exit but not increment the instruction pointer.
		 */
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_EXTINT, 1);
		return (1);
	case EXIT_REASON_NMI_WINDOW:
		/* Exit to allow the pending virtual NMI to be injected */
		if (vm_nmi_pending(vmx->vm, vcpu))
			vmx_inject_nmi(vmx, vcpu);
		vmx_clear_nmi_window_exiting(vmx, vcpu);
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_NMI_WINDOW, 1);
		return (1);
	case EXIT_REASON_INOUT:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_INOUT, 1);
		vmexit->exitcode = VM_EXITCODE_INOUT;
		vmexit->u.inout.bytes = (qual & 0x7) + 1;
		vmexit->u.inout.in = in = (qual & 0x8) ? 1 : 0;
		vmexit->u.inout.string = (qual & 0x10) ? 1 : 0;
		vmexit->u.inout.rep = (qual & 0x20) ? 1 : 0;
		vmexit->u.inout.port = (uint16_t)(qual >> 16);
		vmexit->u.inout.eax = (uint32_t) reg_read(vcpu, HV_X86_RAX);
		// if ((vmexit->u.inout.port == 0x0020) ||
		// 	(vmexit->u.inout.port == 0x0021) ||
		// 	(vmexit->u.inout.port == 0x00a0) ||
		// 	(vmexit->u.inout.port == 0x00a1))
		// {
		// 	printf("EXIT_REASON_INOUT port 0x%03x in %d\n",
		// 		vmexit->u.inout.port, vmexit->u.inout.in);
		// }
		if (vmexit->u.inout.string) {
			inst_info = (uint32_t) vmcs_read(vcpu, VMCS_EXIT_INSTRUCTION_INFO);
			vmexit->exitcode = VM_EXITCODE_INOUT_STR;
			vis = &vmexit->u.inout_str;
			vmx_paging_info(&vis->paging, vcpu);
			vis->rflags = vmcs_read(vcpu, VMCS_GUEST_RFLAGS);
			vis->cr0 = vmcs_read(vcpu, VMCS_GUEST_CR0);
			vis->index = inout_str_index(vmx, vcpu, in);
			vis->count = inout_str_count(vmx, vcpu, vis->inout.rep);
			vis->addrsize = inout_str_addrsize(inst_info);
			inout_str_seginfo(vmx, vcpu, inst_info, in, vis);
		}
		break;
	case EXIT_REASON_CPUID:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_CPUID, 1);
		handled = vmx_handle_cpuid(vmx->vm, vcpu);
		break;
	case EXIT_REASON_EXCEPTION:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_EXCEPTION, 1);
		intr_info = (uint32_t) vmcs_read(vcpu, VMCS_EXIT_INTR_INFO);
		KASSERT((intr_info & VMCS_INTR_VALID) != 0,
		    ("VM exit interruption info invalid: %#x", intr_info));

		intr_vec = intr_info & 0xff;
		intr_type = intr_info & VMCS_INTR_T_MASK;

		/*
		 * If Virtual NMIs control is 1 and the VM-exit is due to a
		 * fault encountered during the execution of IRET then we must
		 * restore the state of "virtual-NMI blocking" before resuming
		 * the guest.
		 *
		 * See "Resuming Guest Software after Handling an Exception".
		 * See "Information for VM Exits Due to Vectored Events".
		 */
		if ((idtvec_info & VMCS_IDT_VEC_VALID) == 0 &&
		    (intr_vec != IDT_DF) &&
		    (intr_info & EXIT_QUAL_NMIUDTI) != 0)
			vmx_restore_nmi_blocking(vmx, vcpu);

		/*
		 * The NMI has already been handled in vmx_exit_handle_nmi().
		 */
		if (intr_type == VMCS_INTR_T_NMI)
			return (1);

		if (intr_vec == IDT_PF) {
			reg_write(vcpu, HV_X86_CR2, qual);
		}

		/*
		 * Software exceptions exhibit trap-like behavior. This in
		 * turn requires populating the VM-entry instruction length
		 * so that the %rip in the trap frame is past the INT3/INTO
		 * instruction.
		 */
		if (intr_type == VMCS_INTR_T_SWEXCEPTION)
			vmcs_write(vcpu, VMCS_ENTRY_INST_LENGTH,
				((uint64_t) vmexit->inst_length));

		/* Reflect all other exceptions back into the guest */
		errcode_valid = errcode = 0;
		if (intr_info & VMCS_INTR_DEL_ERRCODE) {
			errcode_valid = 1;
			errcode = (int) vmcs_read(vcpu, VMCS_EXIT_INTR_ERRCODE);
		}
		VCPU_CTR2(vmx->vm, vcpu, "Reflecting exception %d/%#x into "
		    "the guest", intr_vec, errcode);
		error = vm_inject_exception(vmx->vm, vcpu, ((int) intr_vec),
		    errcode_valid, ((uint32_t) errcode), 0);
		KASSERT(error == 0, ("%s: vm_inject_exception error %d",
		    __func__, error));
		return (1);

	case EXIT_REASON_EPT_FAULT:
		/*
		 * If 'gpa' lies within the address space allocated to
		 * memory then this must be a nested page fault otherwise
		 * this must be an instruction that accesses MMIO space.
		 */
		gpa = vmcs_gpa(vcpu);
		HYPERKIT_VMX_EPT_FAULT(vcpu, gpa, qual);
#if 0
		if (vm_mem_allocated(vmx->vm, gpa) ||
		    bootrom_contains_gpa(gpa) ||
		    apic_access_fault(vmx, vcpu, gpa)) {
			vmexit->exitcode = VM_EXITCODE_PAGING;
			vmexit->inst_length = 0;
			vmexit->u.paging.gpa = gpa;
			vmexit->u.paging.fault_type = ept_fault_type(qual);
			vmm_stat_incr(vmx->vm, vcpu, VMEXIT_NESTED_FAULT, 1);
		} else if (ept_emulation_fault(qual)) {
			vmexit_inst_emul(vmexit, gpa, vmcs_gla(vcpu), vcpu);
			vmm_stat_incr(vmx->vm, vcpu, VMEXIT_INST_EMUL, 1);
		}
#endif
		/*
		 * If Virtual NMIs control is 1 and the VM-exit is due to an
		 * EPT fault during the execution of IRET then we must restore
		 * the state of "virtual-NMI blocking" before resuming.
		 *
		 * See description of "NMI unblocking due to IRET" in
		 * "Exit Qualification for EPT Violations".
		 */
		if ((idtvec_info & VMCS_IDT_VEC_VALID) == 0 &&
		    (qual & EXIT_QUAL_NMIUDTI) != 0)
			vmx_restore_nmi_blocking(vmx, vcpu);
		break;
	case EXIT_REASON_VIRTUALIZED_EOI:
		vmexit->exitcode = VM_EXITCODE_IOAPIC_EOI;
		vmexit->u.ioapic_eoi.vector = qual & 0xFF;
		vmexit->inst_length = 0;	/* trap-like */
		break;
	case EXIT_REASON_APIC_ACCESS:
		handled = vmx_handle_apic_access(vmx, vcpu, vmexit);
		break;
	case EXIT_REASON_APIC_WRITE:
		/*
		 * APIC-write VM exit is trap-like so the %rip is already
		 * pointing to the next instruction.
		 */
		vmexit->inst_length = 0;
		vlapic = vm_lapic(vmx->vm, vcpu);
		handled = vmx_handle_apic_write(vmx, vcpu, vlapic, qual);
		break;
	case EXIT_REASON_XSETBV:
		handled = vmx_emulate_xsetbv(vmx, vcpu);
		break;
	case EXIT_REASON_MONITOR:
		vmexit->exitcode = VM_EXITCODE_MONITOR;
		break;
	case EXIT_REASON_MWAIT:
		vmexit->exitcode = VM_EXITCODE_MWAIT;
		break;
	default:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_UNKNOWN, 1);
		break;
	}

	if (handled) {
		/*
		 * It is possible that control is returned to userland
		 * even though we were able to handle the VM exit in the
		 * kernel.
		 *
		 * In such a case we want to make sure that the userland
		 * restarts guest execution at the instruction *after*
		 * the one we just processed. Therefore we update the
		 * guest rip in the VMCS and in 'vmexit'.
		 */
		vmexit->rip += (uint64_t) vmexit->inst_length;
		vmexit->inst_length = 0;
		vmcs_write(vcpu, VMCS_GUEST_RIP, vmexit->rip);
	} else {
		if (vmexit->exitcode == VM_EXITCODE_BOGUS) {
			/*
			 * If this VM exit was not claimed by anybody then
			 * treat it as a generic VMX exit.
			 */
			vmexit->exitcode = VM_EXITCODE_VMX;
			vmexit->u.vmx.status = VM_SUCCESS;
			vmexit->u.vmx.inst_type = 0;
			vmexit->u.vmx.inst_error = 0;
		} else {
			/*
			 * The exitcode and collateral have been populated.
			 * The VM exit will be processed further in userland.
			 */
		}
	}
	return (handled);
}

static int
vmx_run(void *arg, int vcpu, register_t rip, void *rendezvous_cookie,
	void *suspend_cookie)
{
	int handled;
	struct vmx *vmx;
	struct vm *vm;
	struct vm_exit *vmexit;
	struct vlapic *vlapic;
	uint32_t exit_reason;
	hv_return_t hvr;

	vmx = arg;
	vm = vmx->vm;
	vlapic = vm_lapic(vm, vcpu);
	vmexit = vm_exitinfo(vm, vcpu);

	vmcs_write(vcpu, VMCS_GUEST_RIP, ((uint64_t) rip));

	do {
		KASSERT(vmcs_guest_rip(vcpu) == ((uint64_t) rip),
			("%s: vmcs guest rip mismatch %#llx/%#llx",
				__func__, vmcs_guest_rip(vcpu), ((uint64_t) rip)));

		handled = UNHANDLED;

		vmx_inject_interrupts(vmx, vcpu, vlapic, ((uint64_t) rip));

		/*
		 * Check for vcpu suspension after injecting events because
		 * vmx_inject_interrupts() can suspend the vcpu due to a
		 * triple fault.
		 */
		if (vcpu_suspended(suspend_cookie)) {
			vm_exit_suspended(vmx->vm, vcpu, ((uint64_t) rip));
			break;
		}

		if (vcpu_rendezvous_pending(rendezvous_cookie)) {
			vm_exit_rendezvous(vmx->vm, vcpu, ((uint64_t) rip));
			break;
		}

		vmx_run_trace(vmx, vcpu);
		hvr = hv_vcpu_run((hv_vcpuid_t) vcpu);
		/* Collect some information for VM exit processing */
		rip = (register_t) vmcs_guest_rip(vcpu);
		vmexit->rip = (uint64_t) rip;
		vmexit->inst_length = (int) vmexit_instruction_length(vcpu);
		vmexit->u.vmx.exit_reason = exit_reason = vmcs_exit_reason(vcpu);
		vmexit->u.vmx.exit_qualification = vmcs_exit_qualification(vcpu);
		/* Update 'nextrip' */
		vmx->state[vcpu].nextrip = (uint64_t) rip;
		if (hvr == HV_SUCCESS) {
			handled = vmx_exit_process(vmx, vcpu, vmexit);
		} else {
			hvdump(vcpu);
			xhyve_abort("vmentry error\n");
		}
		vmx_exit_trace(vmx, vcpu, ((uint64_t) rip), exit_reason, handled);
		rip = (register_t) vmexit->rip;

		vm_check_for_unpause(vm, vcpu);

	} while (handled);

	/*
	 * If a VM exit has been handled then the exitcode must be BOGUS
	 * If a VM exit is not handled then the exitcode must not be BOGUS
	 */
	if ((handled && vmexit->exitcode != VM_EXITCODE_BOGUS) ||
	    (!handled && vmexit->exitcode == VM_EXITCODE_BOGUS)) {
		xhyve_abort("Mismatch between handled (%d) and exitcode (%d)",
		      handled, vmexit->exitcode);
	}

	if (!handled)
		vmm_stat_incr(vm, vcpu, VMEXIT_USERSPACE, 1);

	VCPU_CTR1(vm, vcpu, "returning from vmx_run: exitcode %d",
	    vmexit->exitcode);

	return (0);
}
#endif

static void
vmx_vm_cleanup(void *arg)
{
	struct vmx *vmx = arg;

	free(vmx);

	return;
}

static void
vmx_vcpu_dump(void *arg UNUSED, int vcpu)
{
	hvdump(vcpu);
}

static void
vmx_vcpu_cleanup(void *arg, int vcpuid) {
	if (arg || vcpuid) xhyve_abort("vmx_vcpu_cleanup\n");
}

#if 0
static int
vmx_get_intr_shadow(int vcpu, uint64_t *retval)
{
	uint64_t gi;
	int error;

	error = vmcs_getreg(vcpu, VMCS_IDENT(VMCS_GUEST_INTERRUPTIBILITY), &gi);
	*retval = (gi & HWINTR_BLOCKING) ? 1 : 0;
	return (error);
}

static int
vmx_modify_intr_shadow(struct vmx *vmx, int vcpu, uint64_t val)
{
	uint64_t gi;
	int error, ident;

	/*
	 * Forcing the vcpu into an interrupt shadow is not supported.
	 */
	if (val) {
		error = EINVAL;
		goto done;
	}

	ident = VMCS_IDENT(VMCS_GUEST_INTERRUPTIBILITY);
	error = vmcs_getreg(vcpu, ident, &gi);
	if (error == 0) {
		gi &= ~HWINTR_BLOCKING;
		error = vmcs_setreg(vcpu, ident, gi);
	}
done:
	VCPU_CTR2(vmx->vm, vcpu, "Setting intr_shadow to %#llx %s", val,
	    error ? "failed" : "succeeded");
	return (error);
}

static int
vmx_shadow_reg(enum vm_reg_name reg)
{
	int shreg;

	shreg = -1;

	switch (reg) {
	case VM_REG_GUEST_CR0:
		shreg = VMCS_CR0_SHADOW;
                break;
        case VM_REG_GUEST_CR4:
		shreg = VMCS_CR4_SHADOW;
		break;
	default:
		break;
	}

	return (shreg);
}
#endif
/*
Comparable Registers to x86
x86 RAX, RBX, RCX, RDX: Equivalent to x0, x1, x2, x3 in AArch64.
x86 R8-R15: Equivalent to x8-x15 in AArch64.
x86 RSI, RDI, RBP: Equivalent to x19, x20, and x29 (Frame Pointer) in AArch64.
Program Counter (PC): AArch64 uses PC but it is not directly accessible in user-space code; instead, the program counter is implied in the instruction flow.
Stack Pointer (SP): x86-64 uses RSP; AArch64 uses SP.
System Registers
AArch64 also has various system registers for managing control and status, similar to the control registers (CR0-CR4) in x86:

AArch64 Control Registers: SCTLR_EL1, TTBR0_EL1, TTBR1_EL1, etc.
Exception Link Registers: ELR_EL1, ELR_EL2, etc.
Processor State Registers: DAIF, NZCV, FPCR, FPSR.
*/

/*
static const hv_x86_reg_t hvregs[] = {
	HV_X86_RAX,
	HV_X86_RBX,
        ...
	HV_X86_REGISTERS_MAX,
	HV_X86_CR2,
	HV_X86_REGISTERS_MAX,
};
*/

#if 0
/* Don't use this */
static const aarch64_reg_t hvregs[] = {
   HV_REG_X0,
    HV_REG_X1,
    HV_REG_X2,
    HV_REG_X3,
    HV_REG_X4,
    HV_REG_X5,
    HV_REG_X6,
    HV_REG_X7,
    HV_REG_X8,
    HV_REG_X9,
    HV_REG_X10,
    HV_REG_X11,
    HV_REG_X12,
    HV_REG_X13,
    HV_REG_X14,
    HV_REG_X15,
    HV_REG_X16,
    HV_REG_X17,
    HV_REG_X18,
    HV_REG_X19,
    HV_REG_X20,
    HV_REG_X21,
    HV_REG_X22,
    HV_REG_X23,
    HV_REG_X24,
    HV_REG_X25,
    HV_REG_X26,
    HV_REG_X27,
    HV_REG_X28,
    HV_REG_FP,  // Frame Pointer (x29)
    HV_REG_LR,  // Link Register (x30)
    HV_REG_SP,  // Stack Pointer
    HV_REG_PC,  // Program Counter

    // System Registers
    HV_REG_CPSR,  // Current Program Status Register
};

typedef enum {
    // General-Purpose Registers (GPRs)
    HV_REG_X0,
    HV_REG_X1,
    HV_REG_X2,
    HV_REG_X3,
    HV_REG_X4,
    HV_REG_X5,
    HV_REG_X6,
    HV_REG_X7,
    HV_REG_X8,
    HV_REG_X9,
    HV_REG_X10,
    HV_REG_X11,
    HV_REG_X12,
    HV_REG_X13,
    HV_REG_X14,
    HV_REG_X15,
    HV_REG_X16,
    HV_REG_X17,
    HV_REG_X18,
    HV_REG_X19,
    HV_REG_X20,
    HV_REG_X21,
    HV_REG_X22,
    HV_REG_X23,
    HV_REG_X24,
    HV_REG_X25,
    HV_REG_X26,
    HV_REG_X27,
    HV_REG_X28,
    HV_REG_FP,  // Frame Pointer (x29)
    HV_REG_LR,  // Link Register (x30)
    HV_REG_SP,  // Stack Pointer
    HV_REG_PC,  // Program Counter

    // System Registers
    HV_REG_CPSR,  // Current Program Status Register
} hv_reg_t;
#endif

static int
vmx_getreg(UNUSED void *arg, int vcpu, enum vm_reg_name reg, uint64_t *retval)
{
#if 0
	hv_reg_t hvreg;

	hvreg = hvreg[reg];
	*retval = reg_read(vcpu, hvreg);

	return (vmcs_getreg(vcpu, (int) reg, retval));
#endif
	return 0;
}

static int
vmx_setreg(void *arg, int vcpu, enum vm_reg_name reg, uint64_t val)
{
#if 0
	int error, shadow;
	uint64_t ctls;
	hv_reg_t hvreg;
	struct vmx *vmx = arg;

	hvreg = hvreg[reg];
	
	reg_write(vcpu, hvreg, val);

	error = vmcs_setreg(vcpu, (int) reg, val);

      	return (error);
#endif
	return 0;
}

//static int
//vmx_getdesc(UNUSED void *arg, int vcpu, enum vm_reg_name reg, struct seg_desc *desc)
//{
//	return (vmcs_getdesc(vcpu, (int) reg, desc));
//}

static int
vmx_setdesc(UNUSED void *arg, int vcpu, enum vm_reg_name reg, struct seg_desc *desc)
{
	return (vmcs_setdesc(vcpu, (int) reg, desc));
}

#if 0
static int
vmx_getcap(void *arg, int vcpu, enum vm_cap_type type, int *retval)
{
	struct vmx *vmx = arg;
	int vcap;
	int ret;

	ret = ENOENT;

	vcap = vmx->cap[vcpu].set;

	switch (type) {
	case VM_CAP_HALT_EXIT:
		if (cap_halt_exit)
			ret = 0;
		break;
	case VM_CAP_PAUSE_EXIT:
		if (cap_pause_exit)
			ret = 0;
		break;
	case VM_CAP_MTRAP_EXIT:
		if (cap_monitor_trap)
			ret = 0;
		break;
	default:
		break;
	}

	if (ret == 0)
		*retval = (vcap & (1 << type)) ? 1 : 0;

	return (ret);
}

static int
vmx_setcap(void *arg, int vcpu, enum vm_cap_type type, int val)
{
#if 0
	struct vmx *vmx = arg;
	uint32_t baseval;
	uint32_t *pptr;
	uint32_t reg;
	uint32_t flag;
	int retval;

	retval = ENOENT;
	pptr = NULL;
	baseval = 0;
	reg = 0;
	flag = 0;

	switch (type) {
	case VM_CAP_HALT_EXIT:
		if (cap_halt_exit) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_HLT_EXITING;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_MTRAP_EXIT:
		if (cap_monitor_trap) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_MTF;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_PAUSE_EXIT:
		if (cap_pause_exit) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_PAUSE_EXITING;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	default:
		xhyve_abort("vmx_setcap\n");
	}

	if (retval == 0) {
		if (val) {
			baseval |= flag;
		} else {
			baseval &= ~flag;
		}

		vmcs_write(vcpu, reg, baseval);

		/*
		 * Update optional stored flags, and record
		 * setting
		 */
		if (pptr != NULL) {
			*pptr = baseval;
		}

		if (val) {
			vmx->cap[vcpu].set |= (1 << type);
		} else {
			vmx->cap[vcpu].set &= ~(1 << type);
		}

	}
#endif
	return 0;
}
#endif
struct vlapic_vtx {
	struct vlapic vlapic;
	struct pir_desc *pir_desc;
	struct vmx *vmx;
};

// #define	VMX_CTR_PIR(vm, vcpuid, pir_desc, notify, vector, level, msg)	\
// do {									\
// 	VCPU_CTR2(vm, vcpuid, msg " assert %s-triggered vector %d",	\
// 	    level ? "level" : "edge", vector);				\
// 	VCPU_CTR1(vm, vcpuid, msg " pir0 0x%016lx", pir_desc->pir[0]);	\
// 	VCPU_CTR1(vm, vcpuid, msg " pir1 0x%016lx", pir_desc->pir[1]);	\
// 	VCPU_CTR1(vm, vcpuid, msg " pir2 0x%016lx", pir_desc->pir[2]);	\
// 	VCPU_CTR1(vm, vcpuid, msg " pir3 0x%016lx", pir_desc->pir[3]);	\
// 	VCPU_CTR1(vm, vcpuid, msg " notify: %s", notify ? "yes" : "no");\
// } while (0)

// /*
//  * vlapic->ops handlers that utilize the APICv hardware assist described in
//  * Chapter 29 of the Intel SDM.
//  */
// static int
// vmx_set_intr_ready(struct vlapic *vlapic, int vector, bool level)
// {
// 	struct vlapic_vtx *vlapic_vtx;
// 	struct pir_desc *pir_desc;
// 	uint64_t mask;
// 	int idx, notify;

// 	vlapic_vtx = (struct vlapic_vtx *)vlapic;
// 	pir_desc = vlapic_vtx->pir_desc;

// 	/*
// 	 * Keep track of interrupt requests in the PIR descriptor. This is
// 	 * because the virtual APIC page pointed to by the VMCS cannot be
// 	 * modified if the vcpu is running.
// 	 */
// 	idx = vector / 64;
// 	mask = 1UL << (vector % 64);
// 	atomic_set_long(&pir_desc->pir[idx], mask);
// 	notify = atomic_cmpset_long(&pir_desc->pending, 0, 1);

// 	VMX_CTR_PIR(vlapic->vm, vlapic->vcpuid, pir_desc, notify, vector,
// 	    level, "vmx_set_intr_ready");
// 	return (notify);
// }

static int
vmx_run(void *arg, int vcpu, register_t rip, void *rendezvous_cookie,
        void *suspend_cookie)
{
return 0;
}

#if 0
static struct vlapic *
vmx_vlapic_init(void *arg, int vcpuid)
{
	struct vmx *vmx;
	struct vlapic *vlapic;
	struct vlapic_vtx *vlapic_vtx;

	vmx = arg;

	vlapic = malloc(sizeof(struct vlapic_vtx));
	assert(vlapic);
	bzero(vlapic, sizeof(struct vlapic));
	vlapic->vm = vmx->vm;
	vlapic->vcpuid = vcpuid;
	vlapic->apic_page = (struct LAPIC *)&vmx->apic_page[vcpuid];

	vlapic_vtx = (struct vlapic_vtx *)vlapic;
	vlapic_vtx->vmx = vmx;

	vlapic_init(vlapic);

	return (vlapic);
}
#endif

static void
vmx_vlapic_cleanup(UNUSED void *arg, struct vlapic *vlapic)
{
	vlapic_cleanup(vlapic);
	free(vlapic);
}

static void
vmx_vcpu_interrupt(int vcpu) {
	hv_vcpuid_t hvvcpu;

	hvvcpu = (hv_vcpuid_t) vcpu;

	hv_vcpu_interrupt(&hvvcpu, 1);
}

struct vmm_ops vmm_ops_aarch64 = {
	vmx_init,
	vmx_cleanup,
	vmx_vm_init,
	vmx_vcpu_init,
	vmx_vcpu_dump,
	vmx_run,
	vmx_vm_cleanup,
	vmx_vcpu_cleanup,
	//vmx_getreg,
	//vmx_setreg,
	//vmx_getdesc,
	//vmx_setdesc,
	//vmx_getcap,
	//vmx_setcap,
	//vmx_vlapic_init,
	//vmx_vlapic_cleanup,
	//vmx_vcpu_interrupt
};

