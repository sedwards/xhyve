#ifndef vmm_aarch
#define vmm_aarch
/*
enum vm_reg_name gpr_map[16] = {
    VM_REG_GUEST_RAX,
    VM_REG_GUEST_RCX,
    VM_REG_GUEST_RDX,
    VM_REG_GUEST_RBX,
    VM_REG_GUEST_RSP,
    VM_REG_GUEST_RBP,
    VM_REG_GUEST_RSI,
    VM_REG_GUEST_RDI,
    VM_REG_GUEST_R8,
    VM_REG_GUEST_R9,
    VM_REG_GUEST_R10,
    VM_REG_GUEST_R11,
    VM_REG_GUEST_R12,
    VM_REG_GUEST_R13,
    VM_REG_GUEST_R14,
    VM_REG_GUEST_R15
};
*/
// Ensure enum vm_cpu_mode is fully defined
enum vm_cpu_mode {
    CPU_MODE_REAL,
    CPU_MODE_PROTECTED,
    CPU_MODE_COMPATIBILITY,
    CPU_MODE_64BIT
};



// Full definitions of structs and enums
typedef struct {
    uint8_t op_byte;   // Opcode byte
    uint8_t op_type;   // Operation type (ensure this matches the actual field in vie_op)
    // Add other fields as needed
} vie_op;


/*
// Ensure struct vie is fully defined
typedef struct {
    uint8_t op;
    uint8_t op_byte;   // Opcode byte
    uint8_t reg;
    uint64_t immediate;
    uint8_t modrm;  // Example fields; adjust to your actual struct definition
    int opsize;
    // ... other fields
}vie;
*/

// Ensure enum vm_reg_name is fully defined
enum vm_reg_name {
    VM_REG_GUEST_X0,
    VM_REG_GUEST_X1,
    // ... other ARM registers up to X30, SP, and PC
};

#endif

