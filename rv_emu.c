#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "rv_emu.h"

#define DEBUG 0

uint32_t get_bitseq_c(uint32_t num, int start, int end);

void unsupported(char *s, uint32_t val) {
    printf("%s: %d\n", s, val);
    exit(-1);
}

void rv_init(struct rv_state *rsp, uint32_t *func,
             uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    int i;

    // Zero out registers
    for (i = 0; i < NREGS; i += 1) {
        rsp->regs[i] = 0;
    }

    // Zero out the stack
    for (i = 0; i < STACK_SIZE; i += 1) {
        rsp->stack[i] = 0;
    }

    // Initialize the Program Counter
    rsp->pc = (uint64_t) func;

    // Initialize the Link Register to a sentinel value
    rsp->regs[RA] = 0;

    // Initialize Stack Pointer to the logical bottom of the stack
    rsp->regs[SP] = (uint64_t) &rsp->stack[STACK_SIZE];

    // Initialize the first 4 arguments in emulated r0-r3
    rsp->regs[A0] = a0;
    rsp->regs[A1] = a1;
    rsp->regs[A2] = a2;
    rsp->regs[A3] = a3;

    memset(&rsp->analysis, 0, sizeof(struct rv_analysis_st));
    cache_init(&rsp->i_cache);  
}

static void print_pct(char *fmt, int numer, int denom) {
    double pct = 0.0;

    if (denom) {
        pct = (double) numer / (double) denom * 100.0;
    }
    printf(fmt, numer, pct);
}

void rv_print(struct rv_analysis_st *a) {
    int b_total = a->b_taken + a->b_not_taken;

    printf("=== Analysis\n");
    print_pct("Instructions Executed  = %d\n", a->i_count, a->i_count);
    print_pct("R-type + I-type        = %d (%.2f%%)\n", a->ir_count, a->i_count);
    print_pct("Loads                  = %d (%.2f%%)\n", a->ld_count, a->i_count);
    print_pct("Stores                 = %d (%.2f%%)\n", a->st_count, a->i_count);
    print_pct("Jumps/JAL/JALR         = %d (%.2f%%)\n", a->j_count, a->i_count);
    print_pct("Conditional branches   = %d (%.2f%%)\n", b_total, a->i_count);
    print_pct("  Branches taken       = %d (%.2f%%)\n", a->b_taken, b_total);
    print_pct("  Branches not taken   = %d (%.2f%%)\n", a->b_not_taken, b_total);
}

void emu_r_type(struct rv_state *rsp, uint32_t iw) {
    uint32_t rd = (iw >> 7) & 0b1111;
    uint32_t rs1 = (iw >> 15) & 0b11111;
    uint32_t rs2 = (iw >> 20) & 0b11111;
    uint32_t funct3 = (iw >> 12) & 0b111;
    uint32_t funct7 = get_bitseq_c(iw, 25, 31);

    switch(funct3) {
        case 0b000:
            if(funct7 == 0b0000000) {
                rsp->regs[rd] = rsp->regs[rs1] + rsp->regs[rs2];
            } else if(funct7 == 0b0100000) {
                rsp->regs[rd] = rsp->regs[rs1] - rsp->regs[rs2];
            } else if(funct7 == 0b0000001) {
                rsp->regs[rd] = rsp->regs[rs1] * rsp->regs[rs2];
            }
            break;
        case 0b001:
            if(funct7 == 0b0000000) {
                rsp->regs[rd] = rsp->regs[rs1] << rsp->regs[rs2];
            }
            break;
        case 0b101:
            if(funct7 == 0b0000000) {
                rsp->regs[rd] = rsp->regs[rs1] >> rsp->regs[rs2];
            } else if(funct7 == 0b0100000) {
                rsp->regs[rd] = (int64_t)rsp->regs[rs1] >> rsp->regs[rs2];
            }
            break;
        case 0b111:
            rsp->regs[rd] = rsp->regs[rs1] & rsp->regs[rs2];
            break;
        default:
            unsupported("R-type funct3", funct3);
    }
    rsp->pc += 4; // Next instruction
}

void emu_i_type(struct rv_state *rsp, uint32_t iw) {
    uint32_t rd = get_bitseq_c(iw, 7, 11);
    uint32_t rs1 = get_bitseq_c(iw, 15, 19);
    int64_t imm = (get_bitseq_c(iw, 20, 31) << 52) >> 52;
    uint32_t funct3 = get_bitseq_c(iw, 12, 14);

    switch(funct3) {
        case 0b000:
            rsp->regs[rd] = rsp->regs[rs1] + imm;
            break;
        case 0b101:
            rsp->regs[rd] = rsp->regs[rs1] > imm;
            break;
        default:
            unsupported("I-type funct3", funct3);
    }
    rsp->pc += 4; // Next instruction
}

int64_t get_b_offset(uint32_t iw) {
    int64_t output;
    uint32_t imm4_1 = get_bitseq_c(iw, 8, 11);
    uint32_t imm11 = get_bitseq_c(iw, 7, 7);
    uint32_t imm10_5 = get_bitseq_c(iw, 25, 30);
    uint32_t imm12 = get_bitseq_c(iw, 31, 31);

    output = (imm12 << 12) | (imm11 << 11) | (imm10_5 << 5) | (imm4_1 << 1);
    return (output << 51) >> 51;
}

void emu_b_type(struct rv_state *rsp, uint32_t iw) {
    uint32_t rs1 = get_bitseq_c(iw, 15, 19);
    uint32_t rs2 = get_bitseq_c(iw, 20, 24);
    int64_t offset = get_b_offset(iw);
    uint32_t funct3 = get_bitseq_c(iw, 12, 14);

    switch(funct3) {
        case 0b100:
            if((int64_t)rsp->regs[rs1] < (int64_t)rsp->regs[rs2]) {
                rsp->pc = rsp->pc + offset;  
            } else {
                rsp->pc += 4;
            }
            break;
        case 0b001:
            if((int64_t)rsp->regs[rs1] != (int64_t)rsp->regs[rs2]) {
                rsp->pc = rsp->pc + offset;  
            } else {
                rsp->pc += 4;
            }
            break;
        default:
            unsupported("B-type funct3", funct3);
            rsp->pc += 4; // Next instruction
    }
}

int64_t get_j_offset(uint32_t iw) {
    int64_t output;
    uint32_t imm19_12 = get_bitseq_c(iw, 12, 19);
    uint32_t imm11 = get_bitseq_c(iw, 20, 20);
    uint32_t imm10_1 = get_bitseq_c(iw, 21, 30);
    uint32_t imm20 = get_bitseq_c(iw, 31, 31);

    output = (imm20 << 20) | (imm19_12 << 12) | (imm11 << 11) | (imm10_1 << 1);
    return (output << 43) >> 43;
}

void emu_jal(struct rv_state *rsp, uint32_t iw) {
    uint32_t offset = get_j_offset(iw);
    rsp->pc = rsp->pc + offset; 
}

void emu_jalr(struct rv_state *rsp, uint32_t iw) {
    uint32_t rs1 = (iw >> 15) & 0b1111;  // Will be ra (aka x1)
    uint64_t val = rsp->regs[rs1];  // Value of regs[1]

    rsp->pc = val;  // PC = return address
}

void rv_one(struct rv_state *rsp) {

    // Get an instruction word from the current Program Counter    
    uint32_t iw = *(uint32_t*) rsp->pc;

    /* could also think of that ^^^ as:
        uint32_t *pc = (uint32_t*) rsp->pc;
        uint32_t iw = *pc;
    */

    //iw = *(uint32_t*) rsp->pc;
    // Use below to add cache
    iw = cache_lookup(&rsp->i_cache, (uint64_t) rsp->pc);
    
    uint32_t opcode = iw & 0b1111111;
    switch (opcode) {
        case 0b0110011:
            // R-type instructions have two register operands
            emu_r_type(rsp, iw);
            break;
        case 0b0010011:
            // I-type instructions have one register operand
            emu_i_type(rsp, iw);
            break;
        case 0b1100011:
            // B-type instructions have two register operand
            emu_b_type(rsp, iw);
            break;
        case 0b1101111:
            // JALR (aka RET) is a variant of I-type instructions
            emu_jal(rsp, iw);
            break;
        case 0b1100111:
            // JALR (aka RET) is a variant of I-type instructions
            emu_jalr(rsp, iw);
            break;
        default:
            unsupported("Unknown opcode: ", opcode);
            
    }
}

int rv_emulate(struct rv_state *rsp) {
    while (rsp->pc != 0) {
        rv_one(rsp);
    }
    
    return (int) rsp->regs[A0];
}
