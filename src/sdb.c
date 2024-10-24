#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <capstone/capstone.h>
#include <sys/user.h>

#define MAX_INSTRUCTION_SIZE 5

// We'll store the state of the program here
struct user_regs_struct oldregs;

// Open the capstone library to disassemble code
csh handle;
cs_insn *insn;
size_t count;

void disassemble(unsigned long addr, pid_t child) {
    unsigned long ins = ptrace(PTRACE_PEEKTEXT, child, addr, 0);
    count = cs_disasm(handle, (uint8_t*)&ins, MAX_INSTRUCTION_SIZE, addr, 0, &insn);
    if (count > 0) {
        size_t j;
        for (j = 0; j < count; j++) {
            printf("0x%"PRIx64":\t%s\t\t%s\n", insn[j].address, insn[j].mnemonic, insn[j].op_str);
        }
        cs_free(insn, count);
    } else {
        printf("ERROR: Failed to disassemble given code!\n");
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <program>\n", argv[0]);
        exit(1);
    }

    // Initialize capstone disassembler
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
        return -1;

    pid_t child;
    long orig_eax;
    child = fork();

    if(child == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        execl(argv[1], argv[1], NULL);
    }
    else {
        int status;
        struct user_regs_struct regs;
        unsigned ins;
        long breakpoint = 0x0;
        int isBreakpoint = 0;
        long orig;

        waitpid(child, &status, 0);  // Wait for child to stop on its first instruction
        ptrace(PTRACE_GETREGSET, child, NULL, &regs);  // Get registers

        printf("** program '%s' loaded. entry point 0x%llx\n", argv[1], regs.rip);
        disassemble(regs.rip, child);

        while(1) {
            char command[20];
            printf("(sdb) ");
            fgets(command, 20, stdin);
            command[strcspn(command, "\n")] = 0;

            if(strcmp(command, "cont") == 0) {
                if(isBreakpoint) {
                    regs.rip -= 1;
                    ptrace(PTRACE_SETREGSET, child, NULL, &regs);
                    ptrace(PTRACE_POKETEXT, child, breakpoint, orig);
                    ptrace(PTRACE_CONT, child, NULL, NULL);
                    waitpid(child, &status, 0);
                    if(WIFSTOPPED(status)) {
                        printf("** hit a breakpoint at 0x%lx\n", breakpoint);
                        regs.rip = breakpoint;
                        ptrace(PTRACE_SETREGSET,child, NULL, &regs);
                        ptrace(PTRACE_POKETEXT, child, breakpoint, (orig & 0xFFFFFFFFFFFFFF00) | 0xCC);
                        disassemble(regs.rip, child);
                    }
                    isBreakpoint = 0;
                } else {
                    ptrace(PTRACE_CONT, child, NULL, NULL);
                    waitpid(child, &status, 0);
                    if(WIFSTOPPED(status)) {
                        printf("** program exited with code %d\n", WSTOPSIG(status));
                        break;
                    }
                }
            }
            else if(strcmp(command, "si") == 0) {
                ptrace(PTRACE_SINGLESTEP, child, NULL, NULL);
                waitpid(child, &status, 0);
                ptrace(PTRACE_GETREGSET, child, NULL, &regs);
                disassemble(regs.rip, child);
            }
            else if(strncmp(command, "break", 5) == 0) {
                if(isBreakpoint) {
                    printf("** only one breakpoint supported\n");
                } else {
                    sscanf(command+6, "%lx", &breakpoint);
                    orig = ptrace(PTRACE_PEEKTEXT, child, breakpoint, 0);
                    ptrace(PTRACE_POKETEXT, child, breakpoint, (orig & 0xFFFFFFFFFFFFFF00) | 0xCC);
                    isBreakpoint = 1;
                }
            }
            else {
                printf("** unknown command\n");
            }
        }
    }

    cs_close(&handle);
    return 0;
}

