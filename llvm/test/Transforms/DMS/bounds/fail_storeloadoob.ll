; RUN: clang -fdms=bounds -g -O0 %s -o %t && (if %t; then exit 1; else exit 0; fi)
; (require the compilation to succeed but the executable to return nonzero at
; runtime. the intention is to check that the executable fails a dynamic bounds
; check)

; storing and loading an OOB pointer before dereferencing it
define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	%ptrstorage = alloca i64*, align 4

	; here's the pointer
	%rawarrayptr = alloca [64 x i64], align 4
	%arrayptr = bitcast [64 x i64]* %rawarrayptr to i64*
	%oobptr = getelementptr i64, i64* %arrayptr, i32 99

	; store the pointer
	store volatile i64* %oobptr, i64** %ptrstorage, align 4

	; load the pointer
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	; dereference the loaded pointer
	%loaded = load volatile i64, i64* %loadedptr, align 4

	ret i32 0
}
