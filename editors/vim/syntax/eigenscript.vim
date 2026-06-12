" Vim syntax file
" Language: EigenScript (.eigs)
" Install: copy editors/vim/* into ~/.vim/ (or ~/.config/nvim/)

if exists("b:current_syntax")
  finish
endif

syn keyword eigsControl if elif else loop while for in return try catch break continue match case import
syn keyword eigsDeclare define as local unobserved
syn keyword eigsOperatorWord is of and or not at
syn keyword eigsInterrogative what who when where why how prev state_at
syn keyword eigsPredicate converged stable improving oscillating diverging equilibrium
syn keyword eigsNull null

syn match eigsComment "#.*$"
syn match eigsNumber "\<\d\+\(\.\d\+\)\?\([eE][+-]\?\d\+\)\?\>"
syn region eigsString start=+"+ skip=+\\"+ end=+"+ contains=eigsEscape
syn region eigsFString start=+f"+ skip=+\\"+ end=+"+ contains=eigsEscape,eigsInterp
syn match eigsEscape "\\[ntr\"\\]" contained
syn region eigsInterp start="{" end="}" contained contains=eigsNumber,eigsOperatorWord
syn match eigsFuncDef "\<define\s\+\zs\w\+"
syn match eigsOperator "|>\||=\|=>\|==\|!=\|<=\|>=\|+=\|-=\|\*=\|/=\|%="

hi def link eigsControl Statement
hi def link eigsDeclare Keyword
hi def link eigsOperatorWord Operator
hi def link eigsInterrogative Special
hi def link eigsPredicate Constant
hi def link eigsNull Constant
hi def link eigsComment Comment
hi def link eigsNumber Number
hi def link eigsString String
hi def link eigsFString String
hi def link eigsEscape SpecialChar
hi def link eigsInterp Identifier
hi def link eigsFuncDef Function
hi def link eigsOperator Operator

let b:current_syntax = "eigenscript"
