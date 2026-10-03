#include "../../include/thirdparty/shaft_llvm.h"
#include <lld/Common/Driver.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/raw_ostream.h>
#include <stddef.h>

LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(mingw)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(wasm)

namespace
{
    using LinkFunction = bool (*)(llvm::ArrayRef<const char *>, llvm::raw_ostream &, llvm::raw_ostream &, bool, bool);

    bool run(LinkFunction link, const char **args, size_t count)
    {
        return link(llvm::ArrayRef<const char *>(args, count), llvm::outs(), llvm::errs(), false, false);
    }
} // namespace

extern "C"
{
    bool lld_elf_link(const char **args, size_t count) { return run(lld::elf::link, args, count); }
    bool lld_macho_link(const char **args, size_t count) { return run(lld::macho::link, args, count); }
    bool lld_mingw_link(const char **args, size_t count) { return run(lld::mingw::link, args, count); }
    bool lld_coff_link(const char **args, size_t count) { return run(lld::coff::link, args, count); }
    bool lld_wasm_link(const char **args, size_t count) { return run(lld::wasm::link, args, count); }
}
