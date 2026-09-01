#include "../../include/thirdparty/shaft_llvm.h"
#include <stddef.h>

namespace lld::elf
{
    bool link(const char **args, size_t count);
} // namespace lld::elf

namespace lld::coff
{
    bool link(const char **args, size_t count);
} // namespace lld::coff

namespace lld::macho
{
    bool link(const char **args, size_t count);
} // namespace lld::macho

extern "C"
{
    bool lld_elf_link(const char **args, size_t count) { return lld::elf::link(args, count); }
    bool lld_coff_link(const char **args, size_t count) { return lld::coff::link(args, count); }
    bool lld_macho_link(const char **args, size_t count) { return lld::macho::link(args, count); }
}