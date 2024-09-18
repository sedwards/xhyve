/*-
 * Copyright (c) 2014 Neel Natu <neel@freebsd.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/uio.h>

#include <xhyve/support/psl.h>
#include <xhyve/support/segments.h>
#include <xhyve/support/specialreg.h>
#include <xhyve/vmm/vmm_api.h>
#include <xhyve/xhyve.h>



#define	SEL_START(sel)	(((sel) & ~0x7))
#define	SEL_LIMIT(sel)	(((sel) | 0x7))
#define	TSS_BUSY(type)	(((type) & 0x2) != 0)

static uint64_t
GETREG(int vcpu, int reg)
{
	uint64_t val;
	int error;

	error = xh_vm_get_register(vcpu, reg, &val);
	assert(error == 0);
	return (val);
}

static void
SETREG(int vcpu, int reg, uint64_t val)
{
	int error;

	error = xh_vm_set_register(vcpu, reg, val);
	assert(error == 0);
}

#include <sys/uio.h> // For struct iovec

static int
push_errcode(int vcpu, struct vm_guest_paging *paging, int task_type,
        uint32_t errcode, int *faultptr)
{
    int bytes, error;
    uint64_t sp;
    uint32_t stacksel;

    *faultptr = 0;

    // ARM64 does not use XFLAGS, so this is removed
    // rflags = GETREG(vcpu, VM_REG_GUEST_XFLAGS); // Not applicable for ARM64

    stacksel = (uint16_t) GETREG(vcpu, VM_REG_GUEST_SP); // Simplified for ARM64

    // Determine size based on task type, simplified
    bytes = 4; // ARM64 typically uses 64-bit addressing, adjust as needed

    // Adjust stack pointer (ARM64 uses SP register)
    sp = GETREG(vcpu, VM_REG_GUEST_SP);
    sp -= bytes;

    // Error checking
    // ARM64 has different alignment and exception checking
    // Adapt or remove alignment and fault injection code

    // Setup and perform memory copy
    struct iovec iov[1]; // Declare iov array with appropriate size
    iov[0].iov_base = &errcode;
    iov[0].iov_len = bytes;

    error = xh_vm_copy_setup(vcpu, paging, sp, bytes, XHYVE_PROT_WRITE, iov, 1, faultptr);
    if (error || *faultptr)
        return (error);

    xh_vm_copyout(&errcode, iov, bytes);
    SETREG(vcpu, VM_REG_GUEST_SP, sp);
    return (0);
}

/*
 * Evaluate return value from helper functions and potentially return to
 * the VM run loop.
 */
#define	CHKERR(error,fault)						\
	do {								\
		assert((error == 0) || (error == EFAULT));		\
		if (error)						\
			return (VMEXIT_ABORT);				\
		else if (fault)						\
			return (VMEXIT_CONTINUE);			\
	} while (0)

int vmexit_task_switch(struct vm_exit *vmexit, int *pvcpu);

int
vmexit_task_switch(struct vm_exit *vmexit, int *pvcpu)
{
    struct vm_task_switch *task_switch;
    struct vm_guest_paging *paging;
    uint64_t new_sp;
    int error, fault;
    uint32_t eip;
    int vcpu;
    enum task_switch_reason reason;

    task_switch = &vmexit->u.task_switch;
    paging = &vmexit->u.task_switch.paging;
    vcpu = *pvcpu;

    reason = vmexit->u.task_switch.reason;

    // ARM64 does not use TSS, so we will focus on stack and context switching
    // Calculate the new stack pointer if necessary (simplified for ARM64)
    new_sp = GETREG(vcpu, VM_REG_GUEST_SP); // Example, adjust based on actual context

    // Handle stack adjustment if necessary, ARM64 does not use segment-based adjustments
    // Error handling adapted for ARM64 exception handling
    if (reason == TSR_IRET) {
        // Handle task switch triggered by IRET
        // Simplified, as ARM64 does not use TSS
    }

    // Update the stack pointer
    SETREG(vcpu, VM_REG_GUEST_SP, new_sp);

    // Update the instruction pointer (ARM64 equivalent to EIP)
    eip = (uint32_t) (vmexit->rip + ((uint64_t) vmexit->inst_length));
    error = xh_vm_set_register(vcpu, VM_REG_GUEST_PC, eip); // Set PC register
    if (error) {
        // Handle error
        return VMEXIT_ABORT;
    }

    // Handle virtual-NMI and other special cases
    if (task_switch->reason == TSR_IDT_GATE) {
        error = xh_vm_set_intinfo(vcpu, 0); // Clear interrupt info
        if (error) {
            // Handle error
            return VMEXIT_ABORT;
        }
    }

    return VMEXIT_CONTINUE;
}
