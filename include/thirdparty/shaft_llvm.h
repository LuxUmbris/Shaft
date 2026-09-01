#ifndef SHAFT_LLVM_H
#define SHAFT_LLVM_H

#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/Types.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Target init
    void LLVMInitializeX86Target();
    void LLVMInitializeX86TargetInfo();
    void LLVMInitializeX86TargetMC();
    void LLVMInitializeX86AsmParser();
    void LLVMInitializeX86AsmPrinter();

    void LLVMInitializeAArch64Target();
    void LLVMInitializeAArch64TargetInfo();
    void LLVMInitializeAArch64TargetMC();
    void LLVMInitializeAArch64AsmParser();
    void LLVMInitializeAArch64AsmPrinter();

    void LLVMInitializeRISCVTarget();
    void LLVMInitializeRISCVTargetInfo();
    void LLVMInitializeRISCVTargetMC();
    void LLVMInitializeRISCVAsmParser();
    void LLVMInitializeRISCVAsmPrinter();

    // LLD wrappers
    bool lld_elf_link(const char **args, size_t count);
    bool lld_coff_link(const char **args, size_t count);
    bool lld_macho_link(const char **args, size_t count);

#ifdef __cplusplus
}
#endif

#endif // SHAFT_LLVM_H
