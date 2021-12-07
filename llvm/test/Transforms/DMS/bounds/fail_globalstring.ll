; RUN: clang -fdms=bounds -g -O0 %s -o %t && (if %t; then exit 1; else exit 0; fi)
; (require the compilation to succeed but the executable to return nonzero at
; runtime. the intention is to check that the executable fails a dynamic bounds
; check)

@str = private unnamed_addr constant [13 x i8] c"Hello world\0A\00", align 1

; OOB beyond a global string
define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	%ptrstorage = alloca i8*, align 8
	store volatile i8* getelementptr ([13 x i8], [13 x i8]* @str, i64 0, i64 0), i8** %ptrstorage, align 8
	%strptr = load volatile i8*, i8** %ptrstorage, align 8
	%oobptr = getelementptr i8, i8* %strptr, i64 23
	%loaded = load volatile i8, i8* %oobptr, align 1
	ret i32 0
}
