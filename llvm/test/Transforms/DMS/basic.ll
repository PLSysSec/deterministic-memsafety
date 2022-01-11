; RUN: opt -passes=dms-static -disable-output < %s 2>&1 | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,dms-bounds -disable-output < %s 2>&1 > /dev/null

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_load
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @clean_load() {
  %ptr = alloca i32, align 4
  %res = load i32, i32* %ptr, align 4
  ret i32 %res
}

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_store
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define void @clean_store(i32 %arg) {
  %ptr = alloca i32, align 4
  store i32 %arg, i32* %ptr, align 4
  ret void
}

; This checks that bitcasting a clean pointer is still a clean pointer.
; CHECK-LABEL: bitcast_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i8 @bitcast_clean(i32 %arg) {
  %ptr = alloca i32, align 4
  %newptr = bitcast i32* %ptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that a pointer incremented by 16 bytes is considered blemished16.
; CHECK-LABEL: blemished16_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 1
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 2
define i32 @blemished16_load() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 4
  %res = load i32, i32* %newptr
  %newptr2 = getelementptr i32, i32* %castedptr, i32 4
  store i32 2, i32* %newptr2
  ret i32 %res
}

; This checks that a pointer incremented by 20 bytes is considered blemished32.
; CHECK-LABEL: blemished32_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 1
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 1
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 2
define i32 @blemished32_load() {
  %ptr = alloca [64 x i32]
  %castedptr = bitcast [64 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 5
  %res = load i32, i32* %newptr
  %newptr2 = getelementptr i32, i32* %castedptr, i32 5
  store i32 2, i32* %newptr2
  ret i32 %res
}

; This checks that a pointer incremented by 64 bytes is considered blemished64.
; CHECK-LABEL: blemished64_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 1
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 1
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 2
define i32 @blemished64_load() {
  %ptr = alloca [64 x i32]
  %castedptr = bitcast [64 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 16
  %res = load i32, i32* %newptr
  %newptr2 = getelementptr i32, i32* %castedptr, i32 16
  store i32 2, i32* %newptr2
  ret i32 %res
}

; This checks that a pointer incremented by too much is considered blemishedconst.
; CHECK-LABEL: blemishedconst_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 1
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 2
define i32 @blemishedconst_load() {
  %ptr = alloca [64 x i32]
  %castedptr = bitcast [64 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 50
  %res = load i32, i32* %newptr
  %newptr2 = getelementptr i32, i32* %castedptr, i32 50
  store i32 2, i32* %newptr2
  ret i32 %res
}

; This checks that a pointer incremented by a non-constant is considered dirty.
; CHECK-LABEL: dirty_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 0
define i32 @dirty_load(i32 %arg) {
  %ptr = alloca [64 x i32]
  %castedptr = bitcast [64 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 %arg
  %res = load i32, i32* %newptr
  %newptr2 = getelementptr i32, i32* %castedptr, i32 %arg
  store i32 2, i32* %newptr2
  ret i32 %res
}

; This checks that a pointer we've added 0 to is still clean.
; CHECK-LABEL: gep_still_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 0
define i32 @gep_still_clean() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 0
  %res = load i32, i32* %newptr
  %newptr2 = getelementptr i32, i32* %castedptr, i32 0
  store i32 2, i32* %newptr2
  ret i32 %res
}

; This checks that adding 0 to a blemished pointer is still a blemished pointer.
; CHECK-LABEL: gep_still_blemished
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 1
; CHECK: Nonzero constant pointer arithmetic on a blemished16 ptr: 0
define i32 @gep_still_blemished() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %blemptr = getelementptr i32, i32* %castedptr, i32 2
  %newptr = getelementptr i32, i32* %blemptr, i32 0
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that adding 1 and then 2 to a clean pointer is still just a
; blemished16 pointer.
; CHECK-LABEL: double_blemished16
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 1
; CHECK: Nonzero constant pointer arithmetic on a blemished16 ptr: 1
define i8 @double_blemished16() {
  %ptr = alloca [64 x i8]
  %castedptr = bitcast [64 x i8]* %ptr to i8*
  %blemptr = getelementptr i8, i8* %castedptr, i32 2
  %newptr = getelementptr i8, i8* %blemptr, i32 1
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that adding 10 and then 12 to a clean pointer is a blemished32
; pointer.
; CHECK-LABEL: double_blemished32
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 1
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 1
; CHECK: Nonzero constant pointer arithmetic on a blemished16 ptr: 1
define i8 @double_blemished32() {
  %ptr = alloca [64 x i8]
  %castedptr = bitcast [64 x i8]* %ptr to i8*
  %blemptr = getelementptr i8, i8* %castedptr, i32 10
  %newptr = getelementptr i8, i8* %blemptr, i32 12
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that adding 50 and then 22 to a clean pointer is a blemishedconst
; pointer.
; CHECK-LABEL: double_blemishedconst
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 1
; CHECK: Nonzero constant pointer arithmetic on a blemished16 ptr: 0
; CHECK: Nonzero constant pointer arithmetic on a blemished32 ptr: 0
; CHECK: Nonzero constant pointer arithmetic on a blemished64 ptr: 1
; CHECK: Nonzero constant pointer arithmetic on a blemishedconst ptr: 0
define i8 @double_blemishedconst() {
  %ptr = alloca [64 x i8]
  %castedptr = bitcast [64 x i8]* %ptr to i8*
  %blemptr = getelementptr i8, i8* %castedptr, i32 50
  %newptr = getelementptr i8, i8* %blemptr, i32 22
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that adding a non-constant to a blemished16 pointer is a dirty pointer.
; CHECK-LABEL: blemished_to_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on a clean ptr: 1
; CHECK: Nonzero constant pointer arithmetic on a blemished16 ptr: 0
define i8 @blemished_to_dirty(i32 %arg) {
  %ptr = alloca [64 x i8]
  %castedptr = bitcast [64 x i8]* %ptr to i8*
  %blemptr = getelementptr i8, i8* %castedptr, i32 3
  %newptr = getelementptr i8, i8* %blemptr, i32 %arg
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that bitcasting a dirty pointer is still a dirty pointer.
; CHECK-LABEL: bitcast_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i8 @bitcast_dirty(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %castedptr, i32 %arg
  %newptr = bitcast i32* %dirtyptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that loading from a dirty pointer makes it clean.
; CHECK-LABEL: load_makes_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @load_makes_clean(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 %arg
  %res = load i32, i32* %newptr
  %res2 = load i32, i32* %newptr
  ret i32 %res2
}

; This checks that storing to a dirty pointer makes it clean.
; CHECK-LABEL: store_makes_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @store_makes_clean(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 %arg
  store i32 %arg, i32* %newptr
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks our counting of storing clean/dirty pointers.
; CHECK-LABEL: store_clean_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 4
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 2
; CHECK-NEXT: Storing a blemished16 ptr to mem: 0
; CHECK-NEXT: Storing a blemished32 ptr to mem: 0
; CHECK-NEXT: Storing a blemished64 ptr to mem: 0
; CHECK-NEXT: Storing a blemishedconst ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 2
; CHECK-NEXT: Storing an unknown ptr to mem: 0
define void @store_clean_dirty(i32 %arg) {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 %arg
  %addr = alloca i32*
  store i32* %cleanptr, i32** %addr
  store i32* %dirtyptr, i32** %addr
  store i32* %cleanptr, i32** %addr
  store i32* %dirtyptr, i32** %addr
  ret void
}

; This checks our counting of passing clean/dirty args to functions.
; CHECK-LABEL: passing_args
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 0
; CHECK-NEXT: Storing a blemished16 ptr to mem: 0
; CHECK-NEXT: Storing a blemished32 ptr to mem: 0
; CHECK-NEXT: Storing a blemished64 ptr to mem: 0
; CHECK-NEXT: Storing a blemishedconst ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 0
; CHECK-NEXT: Storing an unknown ptr to mem: 0
; CHECK-NEXT: Passing a clean ptr to a func: 6
; CHECK-NEXT: Passing a blemished16 ptr to a func: 2
; CHECK-NEXT: Passing a blemished32 ptr to a func: 0
; CHECK-NEXT: Passing a blemished64 ptr to a func: 0
; CHECK-NEXT: Passing a blemishedconst ptr to a func: 0
; CHECK-NEXT: Passing a dirty ptr to a func: 2
; CHECK-NEXT: Passing an unknown ptr to a func: 0
declare i32* @takes_ptr(i32*)
declare i32* @takes_two_ptrs(i32*, i32*)
declare void @takes_lots_of_things(i32, i64, i32*, i64*, i1)
define void @passing_args(i32 %arg) {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %blemptr = getelementptr i32, i32* %cleanptr, i32 2
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 %arg
  %res1 = call i32* @takes_ptr(i32* %cleanptr)
  %res2 = call i32* @takes_ptr(i32* %blemptr)
  %res3 = call i32* @takes_ptr(i32* %cleanptr)
  %res4 = call i32* @takes_ptr(i32* %dirtyptr)
  %res5 = call i32* @takes_two_ptrs(i32* %cleanptr, i32* %blemptr)
  %res6 = call i32* @takes_two_ptrs(i32* %cleanptr, i32* %cleanptr)
  %clean64ptr = bitcast i32* %cleanptr to i64*
  call void @takes_lots_of_things(i32 76, i64 0, i32* %dirtyptr, i64* %clean64ptr, i1 1)
  ret void
}

; This checks our counting of returning clean/dirty ptrs from functions.
; CHECK-LABEL: return_clean
; CHECK: Returning a clean ptr from a func: 1
; CHECK-NEXT: Returning a blemished16 ptr from a func: 0
; CHECK-NEXT: Returning a blemished32 ptr from a func: 0
; CHECK-NEXT: Returning a blemished64 ptr from a func: 0
; CHECK-NEXT: Returning a blemishedconst ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 0
define i32* @return_clean() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  ret i32* %cleanptr
}
; CHECK-LABEL: return_blemished
; CHECK: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished16 ptr from a func: 1
; CHECK-NEXT: Returning a blemished32 ptr from a func: 0
; CHECK-NEXT: Returning a blemished64 ptr from a func: 0
; CHECK-NEXT: Returning a blemishedconst ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 0
define i32* @return_blemished() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %blemptr = getelementptr i32, i32* %cleanptr, i32 2
  ret i32* %blemptr
}
; CHECK-LABEL: return_dirty
; CHECK: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished16 ptr from a func: 0
; CHECK-NEXT: Returning a blemished32 ptr from a func: 0
; CHECK-NEXT: Returning a blemished64 ptr from a func: 0
; CHECK-NEXT: Returning a blemishedconst ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 1
define i32* @return_dirty(i32 %arg) {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 %arg
  ret i32* %dirtyptr
}
; CHECK-LABEL: return_void
; CHECK: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished16 ptr from a func: 0
; CHECK-NEXT: Returning a blemished32 ptr from a func: 0
; CHECK-NEXT: Returning a blemished64 ptr from a func: 0
; CHECK-NEXT: Returning a blemishedconst ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 0
define void @return_void() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 5
  ret void
}

; This tests the results of selecting various combinations of clean/dirty
; pointers.
; CHECK-LABEL: select_ptrs
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 2
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 1
; CHECK-NEXT: Stores with dirty addr: 2
; CHECK-NEXT: Stores with unknown addr: 0
define void @select_ptrs(i1 %arg1, i1 %arg2, i32* %unkptr1, i32* %unkptr2, i32 %arg) {
  %initialptr1 = alloca [64 x i32]
  %cleanptr1 = bitcast [64 x i32]* %initialptr1 to i32*
  %initialptr2 = alloca [64 x i32]
  %cleanptr2 = bitcast [64 x i32]* %initialptr2 to i32*
  %blemptr = getelementptr i32, i32* %cleanptr1, i32 2
  %blemconstptr = getelementptr i32, i32* %cleanptr1, i32 49
  %dirtyptr = getelementptr i32, i32* %cleanptr1, i32 %arg
  %ptr1 = select i1 %arg1, i32* %cleanptr1, i32* %cleanptr2  ; clean,clean = clean
  store i32 37, i32* %ptr1
  %ptr2 = select i1 %arg2, i32* %cleanptr1, i32* %dirtyptr  ; clean,dirty = dirty
  store i32 63, i32* %ptr2
  %ptr3 = select i1 %arg1, i32* %blemptr, i32* %cleanptr2  ; blem,clean = blem
  %loaded1 = load i32, i32* %ptr3
  %ptr4 = select i1 %arg2, i32* %blemptr, i32* %blemconstptr  ; blem,blemconst = blemconst
  store i32 107, i32* %ptr4
  %ptr5 = select i1 %arg2, i32* %dirtyptr, i32* %blemconstptr  ; dirty,blemconst = dirty
  store i32 107, i32* %ptr5
  %ptr6 = select i1 %arg2, i32* %blemptr, i32* %unkptr1  ; blem,unk = unk
  %loaded2 = load i32, i32* %ptr6
  %ptr7 = select i1 %arg1, i32* %dirtyptr, i32* %unkptr2  ; dirty,unk = dirty
  %loaded3 = load i32, i32* %ptr7
  %ptr8 = select i1 %arg2, i32* %unkptr1, i32* %unkptr2  ; unk,unk = unk
  %loaded4 = load i32, i32* %ptr8
  ret void
}

; This checks that inttoptr produces clean pointers
; CHECK-LABEL: inttoptr_dirty
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK: Producing a ptr from inttoptr: 1
define void @inttoptr_dirty(i64 %arg) {
  %ptr = inttoptr i64 %arg to i32*
  %loaded = load i32, i32* %ptr
  ret void
}

; This checks that function parameters are considered UNKNOWN pointers
; (which is our current assumption).
; CHECK-LABEL: func_param
; CHECK: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 1
define void @func_param(i32* %ptr) {
  store i32 3, i32* %ptr
  ret void
}

; This checks that pointers returned from calls are considered UNKNOWN
; (which is our current assumption).
; CHECK-LABEL: func_ret
; CHECK: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 2
declare i32* @asdfjkl()
declare i32* @fdsajkl()
define void @func_ret() {
  %ptr1 = call i32* @asdfjkl()
  %ptr2 = call nonnull align 8 dereferenceable(64) i32* @fdsajkl()
  store i32 3, i32* %ptr1
  store i32 4, i32* %ptr2
  ret void
}

; This checks that pointers returned from calls to functions named "malloc" are
; considered CLEAN.
; CHECK-LABEL: malloc_ret
; CHECK: Stores with clean addr: 1
; CHECK: Stores with unknown addr: 0
declare noalias noundef i8* @malloc(i64 noundef)
define void @malloc_ret() {
  %ptr1 = call noalias dereferenceable_or_null(4) i8* @malloc(i64 4)
  store volatile i8 3, i8* %ptr1
  ret void
}

; This checks that pointers loaded from memory are considered UNKNOWN
; (which is our current assumption).
; CHECK-LABEL: from_mem
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 1
define i32 @from_mem(i32 %arg) {
  %ptrptr = alloca [16 x i32*]
  %castedptrptr = bitcast [16 x i32*]* %ptrptr to i32**
  %loadedptr = load i32*, i32** %castedptrptr
  %res = load i32, i32* %loadedptr
  ret i32 %res
}

; This checks that pointers to global variables are considered CLEAN.
; CHECK-LABEL: globals_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
@global_const_str = private unnamed_addr constant [12 x i8] c"Hello world\00", align 1
define i8 @globals_clean() {
  %charptr = getelementptr [12 x i8], [12 x i8]* @global_const_str, i64 0, i64 0
  %loaded = load i8, i8* %charptr, align 1
  ret i8 %loaded
}

; This checks that NULL and undef are considered CLEAN.
; CHECK-LABEL: null_undef_clean
; CHECK-NEXT: Loads with clean addr: 2
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @null_undef_clean() {
  %loaded1 = load i8, i8* null
  %loaded2 = load i8, i8* undef
  %sum = add i8 %loaded1, %loaded2
  ret i8 %sum
}

; This checks that a nonzero GEP on an UNKNOWN pointer produces a dirty
; pointer.
; CHECK-LABEL: gep_on_unk
; CHECK: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Nonzero constant pointer arithmetic on an unknown ptr: 1
define void @gep_on_unk(i32* %arg) {
  %gepptr = getelementptr i32, i32* %arg, i32 1
  store i32 3, i32* %gepptr
  ret void
}

; This checks that constexpr GEP on a global variable produces a clean pointer.
; While we're at it, also bitcasts of global variable pointers.
; See issue #1.
; CHECK-LABEL: constexpr_gep
; CHECK: Passing a clean ptr to a func: 2
; CHECK-NEXT: Passing a blemished16 ptr to a func: 0
; CHECK-NEXT: Passing a blemished32 ptr to a func: 0
; CHECK-NEXT: Passing a blemished64 ptr to a func: 0
; CHECK-NEXT: Passing a blemishedconst ptr to a func: 0
; CHECK-NEXT: Passing a dirty ptr to a func: 0
; CHECK-NEXT: Passing an unknown ptr to a func: 0
@str = private unnamed_addr constant [12 x i8] c"Hello world\00", align 1
declare noundef i32 @puts(i8* nocapture noundef readonly)
define i32 @constexpr_gep() {
  %puts1 = call i32 @puts(i8* nonnull dereferenceable(1) getelementptr inbounds ([12 x i8], [12 x i8]* @str, i64 0, i64 0))
  %puts2 = call i32 @puts(i8* nonnull dereferenceable(1) bitcast ([12 x i8]* @str to i8*))
  ret i32 0
}
