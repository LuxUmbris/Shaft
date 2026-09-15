" Shaft syntax highlighting: imports, macro calls, and compiler metaprogramming directives.
if exists('b:current_syntax')
  finish
endif

syn keyword shaftKeyword def dec cdef cdec naked class struct enum namespace import export using global
syn keyword shaftKeyword if else match case default while for foreach break continue return tunnel reserve
syn keyword shaftType bool char State Thread u8 u16 u32 u64 usize i8 i16 i32 i64 f32 f64
syn match shaftImport /\<import\>/
syn match shaftMeta /^\s*@\%(config\%(\.\%(build\|package\)\.[A-Za-z_]\w*\)\?\|asm\|end\)\>/
syn region shaftAssembly start=/^\s*@asm\%(.*\)$/ end=/^\s*@end\>/ contains=shaftAsmInstruction,shaftAsmRegister,shaftAsmVariable,shaftAsmNumber,shaftComment
syn match shaftAsmInstruction /^\s*\zs[A-Za-z_.][A-Za-z0-9_.]*/ contained
syn match shaftAsmRegister /%[A-Za-z][A-Za-z0-9]*/ contained
syn match shaftAsmVariable /\$[A-Za-z_]\w*/ contained
syn match shaftAsmNumber /\$\?\<\%(0x[0-9A-Fa-f]\+\|\d\+\)\>/ contained
syn match shaftMacro /\<[A-Za-z_]\w*!\ze\s*(/
syn match shaftComment /\/\/.*$/
syn region shaftString start=/"/ skip=/\\./ end=/"/

hi def link shaftKeyword Keyword
hi def link shaftType Type
hi def link shaftImport Include
hi def link shaftMeta PreProc
hi def link shaftAssembly Special
hi def link shaftAsmInstruction Keyword
hi def link shaftAsmRegister Type
hi def link shaftAsmVariable Identifier
hi def link shaftAsmNumber Number
hi def link shaftMacro Macro
hi def link shaftComment Comment
hi def link shaftString String
let b:current_syntax = 'shaft'
