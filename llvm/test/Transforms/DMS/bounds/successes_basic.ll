; RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
; (we just test that we can compile this with bounds checks and then run it and
; it exits successfully, no bounds-check violations or other crashes)
; (we do test with several different optimization levels)

define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	call i8 @staticboundscheck()
	call i8 @dynboundscheck()
	call i64 @storeload()
	call i64 @storeload_mod(i32 %argc)
	call i64 @storeload_becameclean(i32 %argc)
	call i64 @load_mod_deref()
	call i64 @load_mod_deref_nonconst(i32 %argc)
	call i64 @storeload_null()
	call i64 @access_global()
	call i64 @fwrite_to_stderr()
	ret i32 0
}

declare noalias i8* @malloc(i64) nounwind
declare void @free(i8*) nounwind

; do a dereference that requires a static bounds check
define i8 @staticboundscheck() noinline {
	%cleanptr = call i8* @malloc(i64 234)
	%dirtyptr = getelementptr i8, i8* %cleanptr, i64 67
	%loaded = load volatile i8, i8* %dirtyptr
	ret i8 %loaded
}

; do a dereference that requires a dynamic bounds check
define i8 @dynboundscheck() noinline {
	%mallocsize = call i64 @get_mallocsize(i64 117)
	%cleanptr = call i8* @malloc(i64 %mallocsize)
	%offset = call i64 @get_offset(i64 50)
	%dirtyptr = getelementptr i8, i8* %cleanptr, i64 %offset
	%loaded = load volatile i8, i8* %dirtyptr
	ret i8 %loaded
}
define i64 @get_mallocsize(i64 %a) noinline {
	%b = mul i64 %a, 2
	ret i64 %b
}
define i64 @get_offset(i64 %a) noinline {
	%b = add i64 %a, 17
	ret i64 %b
}

; storing and loading an ordinary pointer
define i64 @storeload() noinline {
	%ptrstorage = alloca i64*, align 4

	; here's the pointer
	%cleanptr = alloca i64, align 4
	store i64 72, i64* %cleanptr, align 4

	; store the pointer
	store volatile i64* %cleanptr, i64** %ptrstorage, align 4

	; load the pointer
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	; dereference the loaded pointer
	%loaded = load volatile i64, i64* %loadedptr, align 4
	ret i64 %loaded
}

; storing and loading a modified/dirty pointer
define i64 @storeload_mod(i32 %offset) noinline {
	%ptrstorage = alloca i64*, align 4

	; here's the pointer
	%rawarrayptr = alloca [64 x i64], align 4
	%arrayptr = bitcast [64 x i64]* %rawarrayptr to i64*
	%dirtyptr = getelementptr i64, i64* %arrayptr, i32 %offset

	; store the pointer
	store volatile i64* %dirtyptr, i64** %ptrstorage, align 4

	; load the pointer
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	; dereference the loaded pointer
	%loaded = load volatile i64, i64* %loadedptr, align 4
	ret i64 %loaded
}

; storing and loading a pointer which was modified/dirty and then became clean
define i64 @storeload_becameclean(i32 %offset) noinline {
	%ptrstorage = alloca i64*, align 4

	; here's the pointer
	%rawarrayptr = alloca [64 x i64], align 4
	%arrayptr = bitcast [64 x i64]* %rawarrayptr to i64*
	%dirtyptr = getelementptr i64, i64* %arrayptr, i32 %offset

	; dirty pointer is now clean
	store i64 62, i64* %dirtyptr, align 4

	; store the pointer
	store volatile i64* %dirtyptr, i64** %ptrstorage, align 4

	; load the pointer
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	; dereference the loaded pointer
	%loaded = load volatile i64, i64* %loadedptr, align 4
	ret i64 %loaded
}

; loading a pointer, modifying it by a constant, then dereferencing it
define i64 @load_mod_deref() noinline {
	%ptrstorage = alloca i64*, align 4

	; here's the pointer
	%rawarrayptr = alloca [64 x i64], align 4
	%arrayptr = bitcast [64 x i64]* %rawarrayptr to i64*

	; store the pointer
	store volatile i64* %arrayptr, i64** %ptrstorage, align 4

	; load the pointer
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	; modify the pointer by a constant
	%modptr = getelementptr i64, i64* %loadedptr, i32 51

	; dereference the modified pointer
	%loaded = load volatile i64, i64* %modptr, align 4
	ret i64 %loaded
}

; loading a pointer, modifying it by a nonconstant, then dereferencing it
define i64 @load_mod_deref_nonconst(i32 %offset) noinline {
	%ptrstorage = alloca i64*, align 4

	; here's the pointer
	%rawarrayptr = alloca [64 x i64], align 4
	%arrayptr = bitcast [64 x i64]* %rawarrayptr to i64*

	; store the pointer
	store volatile i64* %arrayptr, i64** %ptrstorage, align 4

	; load the pointer
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	; modify the pointer by a nonconstant
	%modptr = getelementptr i64, i64* %loadedptr, i32 %offset

	; dereference the modified pointer
	%loaded = load volatile i64, i64* %modptr, align 4
	ret i64 %loaded
}

; storing and loading a NULL pointer
define i64 @storeload_null() noinline {
	%ptrstorage = alloca i64*, align 4

	; store NULL
	store volatile i64* null, i64** %ptrstorage, align 4

	; load NULL
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	ret i64 0
}

; access various offsets in a global string
@str = private unnamed_addr constant [13 x i8] c"Hello world\0A\00", align 1
define i64 @access_global() noinline {
	%ptrstorage = alloca i8*, align 8
	store volatile i8* getelementptr ([13 x i8], [13 x i8]* @str, i64 0, i64 0), i8** %ptrstorage, align 8
	%strptr = load volatile i8*, i8** %ptrstorage, align 8
	%ptr1 = getelementptr i8, i8* %strptr, i64 0
	%loaded1 = load volatile i8, i8* %ptr1, align 1
	%ptr2 = getelementptr i8, i8* %strptr, i64 12
	%loaded2 = load volatile i8, i8* %ptr2, align 1
	ret i64 0
}

; call fwrite to stderr
%struct._IO_FILE = type { i32, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, %struct._IO_marker*, %struct._IO_FILE*, i32, i32, i64, i16, i8, [1 x i8], i8*, i64, %struct._IO_codecvt*, %struct._IO_wide_data*, %struct._IO_FILE*, i8*, i64, i32, [20 x i8] }
%struct._IO_marker = type opaque
%struct._IO_codecvt = type opaque
%struct._IO_wide_data = type opaque
@stderr = external dso_local local_unnamed_addr global %struct._IO_FILE*, align 8
@hi = private unnamed_addr constant [4 x i8] c"hi\0A\00", align 1
declare noundef i64 @fwrite(i8* nocapture noundef, i64 noundef, i64 noundef, %struct._IO_FILE* nocapture noundef) local_unnamed_addr
define i64 @fwrite_to_stderr() noinline {
	%1 = load %struct._IO_FILE*, %struct._IO_FILE** @stderr, align 8
	%ret = call i64 @fwrite(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @hi, i64 0, i64 0), i64 3, i64 1, %struct._IO_FILE* %1)
	ret i64 %ret
}
