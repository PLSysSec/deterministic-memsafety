; RUN: clang -fdms=bounds -g -O0 %s -o %t && (if %t; then exit 1; else exit 0; fi)
; (require the compilation to succeed but the executable to return nonzero at
; runtime. the intention is to check that the executable fails a dynamic bounds
; check)

; simple OOB beyond a stack-allocated array
define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	%arrayptr = alloca [64 x i8], align 4
	%ptr = bitcast [64 x i8]* %arrayptr to i8*
	%oobptr = getelementptr i8, i8* %ptr, i32 64
	%loaded = load volatile i8, i8* %oobptr, align 4
	ret i32 0
}
