" Shaft syntax highlighting for source, metaprogramming, and inline assembly.
if exists('b:current_syntax')
  finish
endif

syn case match

" Comments and quoted literals contain no source tokens.
syn match shaftComment /\/\/.*$/
syn region shaftString start=/"/ skip=/\\./ end=/"/ contains=shaftEscape
syn region shaftCharacter start=/'/ skip=/\\./ end=/'/ contains=shaftEscape
syn match shaftEscape /\\./ contained

" Ordinary source tokens.
syn keyword shaftKeyword align async await break case continue default else export for foreach global if import in index init inline match matches move mut naked raw ref return self sizeof start tunnel using valid while
syn keyword shaftType bool char State Thread u8 u16 u32 u64 usize i8 i16 i32 i64 f32 f64
syn keyword shaftBoolean true false
syn match shaftNumber /\<\%(0x[0-9A-Fa-f_]\+\|0b[01_]\+\|[0-9][0-9_]*\%(\.[0-9][0-9_]*\)\?\)\>/
syn match shaftOperator /::\|->\|<-\|<<=\|>>=\|<<\|>>\|<=\|>=\|==\|!=\|&&\|||\|[+\-*/%=<>!&|^]/
syn match shaftDelimiter /[(){}\[\],;.]/
syn match shaftVariable /\<[A-Za-z_][A-Za-z0-9_]*\>/

" Types are recognized at declarations and type-bearing source positions, even when lowercase.
syn match shaftType /\<[A-Z][A-Za-z0-9_]*\>/
syn keyword shaftKeyword namespace class struct enum nextgroup=shaftCustomType skipwhite
syn keyword shaftKeyword reserve nextgroup=shaftModifier,shaftCustomType skipwhite
syn keyword shaftModifier mut ref contained nextgroup=shaftCustomType skipwhite
syn keyword shaftKeyword def dec cdef cdec nextgroup=shaftFunctionModifier,shaftFunction skipwhite
syn keyword shaftFunctionModifier async naked contained nextgroup=shaftFunction skipwhite
syn match shaftFunction /[A-Za-z_][A-Za-z0-9_]*/ contained
syn match shaftCustomType /\*\?[A-Za-z_][A-Za-z0-9_]*/ contained
syn match shaftParameterOpen /(/ nextgroup=shaftCustomType skipwhite
syn match shaftReturnArrow /->/ nextgroup=shaftCustomType skipwhite
syn match shaftProperty /\.[A-Za-z_][A-Za-z0-9_]*\>/
syn match shaftCall /\<[A-Za-z_][A-Za-z0-9_]*\ze\s*(/
syn match shaftImport /\<import\>/

" Metaprogramming headers and macro calls.
syn match shaftConfig /^\s*@!\?config\.\%(build\|package\)\.[A-Za-z_][A-Za-z0-9_]*\>/
syn match shaftMeta /^\s*@\%(asm\|end\)\>/
syn match shaftError /^\s*@error\>/
syn match shaftMacro /\<[A-Za-z_]\w*!\ze\s*(/

" This rule follows operators so a comment wins over its opening slashes.
syn match shaftComment /\/\/.*$/

" Inline assembly uses its own token vocabulary.
syn region shaftAssembly start=/^\s*@asm\%(.*\)$/ end=/^\s*@end\>/ contains=shaftAsmInstruction,shaftAsmRegister,shaftAsmVariable,shaftAsmNumber,shaftComment
syn match shaftAsmInstruction /^\s*\zs[A-Za-z_.][A-Za-z0-9_.]*/ contained
syn match shaftAsmRegister /%[A-Za-z][A-Za-z0-9]*/ contained
syn match shaftAsmVariable /\$[A-Za-z_]\w*/ contained
syn match shaftAsmNumber /\$\?\<\%(0x[0-9A-Fa-f]\+\|\d\+\)\>/ contained

hi def link shaftKeyword Keyword
hi def link shaftType Type
hi def link shaftCustomType Type
hi def link shaftBoolean Boolean
hi def link shaftNumber Number
hi def link shaftString String
hi def link shaftCharacter Character
hi def link shaftEscape SpecialChar
hi def link shaftComment Comment
hi def link shaftOperator Operator
hi def link shaftDelimiter Delimiter
hi def link shaftVariable Identifier
hi def link shaftProperty Identifier
hi def link shaftFunction Function
hi def link shaftFunctionModifier Keyword
hi def link shaftCall Function
hi def link shaftModifier Keyword
hi def link shaftParameterOpen Delimiter
hi def link shaftReturnArrow Operator
hi def link shaftImport Include
hi def shaftConfig ctermfg=Blue guifg=Blue cterm=bold gui=bold
hi def link shaftMeta PreProc
hi def link shaftError Error
hi def link shaftMacro Macro
hi def link shaftAssembly Special
hi def link shaftAsmInstruction Keyword
hi def link shaftAsmRegister Type
hi def link shaftAsmVariable Identifier
hi def link shaftAsmNumber Number
let b:current_syntax = 'shaft'
