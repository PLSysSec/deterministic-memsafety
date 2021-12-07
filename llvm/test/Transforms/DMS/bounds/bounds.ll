; RUN: clang -fdms=bounds -g %s -o %t && %t
; (we just test that we can compile this with bounds checks and then run it and
; it exits successfully, no bounds-check violations or other crashes)

define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	call i64 @storeload()
	call i64 @storeload_mod(i32 %argc)
	call i64 @storeload_becameclean(i32 %argc)
	call i64 @load_mod_deref()
	call i64 @load_mod_deref_nonconst(i32 %argc)
	call i64 @storeload_null()
	ret i32 0
}

; storing and loading an ordinary pointer
define i64 @storeload() {
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
define i64 @storeload_mod(i32 %offset) {
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
define i64 @storeload_becameclean(i32 %offset) {
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
define i64 @load_mod_deref() {
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
define i64 @load_mod_deref_nonconst(i32 %offset) {
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
define i64 @storeload_null() {
	%ptrstorage = alloca i64*, align 4

	; store NULL
	store volatile i64* null, i64** %ptrstorage, align 4

	; load NULL
	%loadedptr = load volatile i64*, i64** %ptrstorage, align 4

	ret i64 0
}
