#include "../../include/thirdparty/shaft_llvm.h"
#include <lld/Common/Driver.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/raw_ostream.h>
#include <stddef.h>

LLD_HAS_DRIVER(elf)

extern "C"
{
    bool lld_elf_link(const char **args, size_t count)
    {
        return lld::elf::link(llvm::ArrayRef<const char *>(args, count), llvm::outs(), llvm::errs(), false, false);
    }
}