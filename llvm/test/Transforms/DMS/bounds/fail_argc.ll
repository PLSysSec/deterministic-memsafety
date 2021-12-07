; RUN: clang -fdms=bounds -g -O0 %s -o %t && (if %t; then exit 1; else exit 0; fi)
; (require the compilation to succeed but the executable to return nonzero at
; runtime. the intention is to check that the executable fails a dynamic bounds
; check)

; OOB by accessing a too-high element of argv
define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	%ptr_to_argv2 = getelementptr i8*, i8** %argv, i64 2
	%argv2 = load volatile i8*, i8** %ptr_to_argv2
	%loaded = load volatile i8, i8* %argv2
	ret i32 0
}
