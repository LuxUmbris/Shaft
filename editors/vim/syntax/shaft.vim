" Shaft syntax highlighting: imports, macro calls, and compiler metaprogramming directives.
if exists('b:current_syntax')
  finish
endif

syn keyword shaftKeyword def dec cdef cdec class struct enum namespace import export using global
syn keyword shaftKeyword if else match case default while for foreach break continue return tunnel reserve
syn keyword shaftType bool char State Thread u8 u16 u32 u64 usize i8 i16 i32 i64 f32 f64
syn match shaftImport /\<import\>/
syn match shaftMeta /^\s*@\%(config\%(\.\%(build\|package\)\.[A-Za-z_]\w*\)\?\|asm\|end\)\>/
syn match shaftMacro /\<[A-Za-z_]\w*!\ze\s*(/
syn match shaftComment /\/\/.*$/
syn region shaftString start=/"/ skip=/\\./ end=/"/

hi def link shaftKeyword Keyword
hi def link shaftType Type
hi def link shaftImport Include
hi def link shaftMeta PreProc
hi def link shaftMacro Macro
hi def link shaftComment Comment
hi def link shaftString String
let b:current_syntax = 'shaft'
