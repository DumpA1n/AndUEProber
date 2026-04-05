    // Export symbols for both macOS (with underscore) and Linux/Android (without underscore)
    .global _get_regs
    .global _get_regs_end
    .global _set_regs
    .global _set_regs_end
    .global get_regs
    .global get_regs_end
    .global set_regs
    .global set_regs_end
    .text

_get_regs:
get_regs:
    STP             X30, XZR, [SP,#-0x20]
    STP             X28, X29, [SP,#-0x30]
    STP             X26, X27, [SP,#-0x40]
    STP             X24, X25, [SP,#-0x50]
    STP             X22, X23, [SP,#-0x60]
    STP             X20, X21, [SP,#-0x70]
    STP             X18, X19, [SP,#-0x80]
    STP             X16, X17, [SP,#-0x90]
    STP             X14, X15, [SP,#-0xA0]
    STP             X12, X13, [SP,#-0xB0]
    STP             X10, X11, [SP,#-0xC0]
    STP             X8, X9, [SP,#-0xD0]
    STP             X6, X7, [SP,#-0xE0]
    STP             X4, X5, [SP,#-0xF0]
    STP             X2, X3, [SP,#-0x100]
    STP             X0, X1, [SP,#-0x110]
    STP             Q30, Q31, [SP,#-0x130]
    STP             Q28, Q29, [SP,#-0x150]
    STP             Q26, Q27, [SP,#-0x170]
    STP             Q24, Q25, [SP,#-0x190]
    STP             Q22, Q23, [SP,#-0x1B0]
    STP             Q20, Q21, [SP,#-0x1D0]
    STP             Q18, Q19, [SP,#-0x1F0]
    STP             Q16, Q17, [SP,#-0x210]
    STP             Q14, Q15, [SP,#-0x230]
    STP             Q12, Q13, [SP,#-0x250]
    STP             Q10, Q11, [SP,#-0x270]
    STP             Q8, Q9, [SP,#-0x290]
    STP             Q6, Q7, [SP,#-0x2B0]
    STP             Q4, Q5, [SP,#-0x2D0]
    STP             Q2, Q3, [SP,#-0x2F0]
    STP             Q0, Q1, [SP,#-0x310]
    MRS             X1, NZCV
    STUR            X1, [SP,#-0x10]
    MOV             X1, SP
    STUR            X1, [SP,#-0x18]
    SUB             SP, SP, #0x310
    MOV             X0, SP

_get_regs_end:
get_regs_end:
    RET

_set_regs:
set_regs:
    ADD             SP, SP, #0x310
    LDUR            X1, [SP,#-0x10]
    LDP             X30, X29, [SP,#-0x20]
    LDP             X28, X29, [SP,#-0x30]
    LDP             X26, X27, [SP,#-0x40]
    LDP             X24, X25, [SP,#-0x50]
    LDP             X22, X23, [SP,#-0x60]
    LDP             X20, X21, [SP,#-0x70]
    LDP             X18, X19, [SP,#-0x80]
    LDP             X16, X17, [SP,#-0x90]
    LDP             X14, X15, [SP,#-0xA0]
    LDP             X12, X13, [SP,#-0xB0]
    LDP             X10, X11, [SP,#-0xC0]
    LDP             X8, X9, [SP,#-0xD0]
    LDP             X6, X7, [SP,#-0xE0]
    LDP             X4, X5, [SP,#-0xF0]
    LDP             X2, X3, [SP,#-0x100]
    LDP             Q30, Q31, [SP,#-0x130]
    LDP             Q28, Q29, [SP,#-0x150]
    LDP             Q26, Q27, [SP,#-0x170]
    LDP             Q24, Q25, [SP,#-0x190]
    LDP             Q22, Q23, [SP,#-0x1B0]
    LDP             Q20, Q21, [SP,#-0x1D0]
    LDP             Q18, Q19, [SP,#-0x1F0]
    LDP             Q16, Q17, [SP,#-0x210]
    LDP             Q14, Q15, [SP,#-0x230]
    LDP             Q12, Q13, [SP,#-0x250]
    LDP             Q10, Q11, [SP,#-0x270]
    LDP             Q8, Q9, [SP,#-0x290]
    LDP             Q6, Q7, [SP,#-0x2B0]
    LDP             Q4, Q5, [SP,#-0x2D0]
    LDP             Q2, Q3, [SP,#-0x2F0]
    LDP             Q0, Q1, [SP,#-0x310]
    MOV             X16, X0
    MSR             NZCV, X1
    LDP             X0, X1, [SP,#-0x110]
    CBZ             X16, label
    BR              X16
label:
    LDP             X16, X17, [SP,#-0x90]
    RET

_set_regs_end:
set_regs_end:
    RET
