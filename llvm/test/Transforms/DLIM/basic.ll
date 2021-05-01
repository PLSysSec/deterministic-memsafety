; RUN: opt -passes=dlim -disable-output < %s 2>&1 | FileCheck %s

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_load
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
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
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with blemished addr: 0
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
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i8 @bitcast_clean(i32 %arg) {
  %ptr = alloca i32, align 4
  %newptr = bitcast i32* %ptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that a pointer incremented by only a little is considered blemished.
; CHECK-LABEL: blemished_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @blemished_load() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that a pointer incremented by too much is considered dirty.
; CHECK-LABEL: dirty_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @dirty_load() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 3
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that a pointer we've added 0 to is still clean.
; CHECK-LABEL: gep_still_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @gep_still_clean() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 0
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that adding 0 to a blemished pointer is still a blemished pointer.
; CHECK-LABEL: gep_still_blemished
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @gep_still_blemished() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %blemptr = getelementptr i32, i32* %castedptr, i32 2
  %newptr = getelementptr i32, i32* %blemptr, i32 0
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that adding 1 to a blemished pointer is a dirty pointer.
; (Someday maybe we could consider this blemished instead of dirty.)
; CHECK-LABEL: double_blemished
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i8 @double_blemished() {
  %ptr = alloca [64 x i8]
  %castedptr = bitcast [64 x i8]* %ptr to i8*
  %blemptr = getelementptr i8, i8* %castedptr, i32 2
  %newptr = getelementptr i8, i8* %blemptr, i32 1
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that bitcasting a dirty pointer is still a dirty pointer.
; CHECK-LABEL: bitcast_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i8 @bitcast_dirty(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %castedptr, i32 3
  %newptr = bitcast i32* %dirtyptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that loading from a dirty pointer makes it clean.
; CHECK-LABEL: load_makes_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @load_makes_clean() {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 3
  %res = load i32, i32* %newptr
  %res2 = load i32, i32* %newptr
  ret i32 %res2
}

; This checks that storing to a dirty pointer makes it clean.
; CHECK-LABEL: store_makes_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @store_makes_clean(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 3
  store i32 %arg, i32* %newptr
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks our counting of storing clean/dirty pointers.
; CHECK-LABEL: store_clean_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 4
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 2
; CHECK-NEXT: Storing a blemished ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 2
; CHECK-NEXT: Storing an unknown ptr to mem: 0
define void @store_clean_dirty() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 3
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
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 0
; CHECK-NEXT: Storing a blemished ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 0
; CHECK-NEXT: Storing an unknown ptr to mem: 0
; CHECK-NEXT: Passing a clean ptr to a func: 6
; CHECK-NEXT: Passing a blemished ptr to a func: 2
; CHECK-NEXT: Passing a dirty ptr to a func: 2
; CHECK-NEXT: Passing an unknown ptr to a func: 0
declare i32* @takes_ptr(i32*)
declare i32* @takes_two_ptrs(i32*, i32*)
declare void @takes_lots_of_things(i32, i64, i32*, i64*, i1)
define void @passing_args() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %blemptr = getelementptr i32, i32* %cleanptr, i32 2
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 3
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
; CHECK-NEXT: Returning a blemished ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 0
define i32* @return_clean() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  ret i32* %cleanptr
}
; CHECK-LABEL: return_blemished
; CHECK: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished ptr from a func: 1
; CHECK-NEXT: Returning a dirty ptr from a func: 0
define i32* @return_blemished() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %blemptr = getelementptr i32, i32* %cleanptr, i32 2
  ret i32* %blemptr
}
; CHECK-LABEL: return_dirty
; CHECK: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 1
define i32* @return_dirty() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 3
  ret i32* %dirtyptr
}
; CHECK-LABEL: return_void
; CHECK: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 0
define void @return_void() {
  %ptr = alloca [16 x i32]
  %cleanptr = bitcast [16 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 3
  ret void
}

; This tests the results of selecting various combinations of clean/dirty
; pointers.
; CHECK-LABEL: select_ptrs
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 1
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 2
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
define void @select_ptrs(i1 %arg1, i1 %arg2, i32* %unkptr1, i32* %unkptr2) {
  %initialptr1 = alloca [16 x i32]
  %cleanptr1 = bitcast [16 x i32]* %initialptr1 to i32*
  %initialptr2 = alloca [16 x i32]
  %cleanptr2 = bitcast [16 x i32]* %initialptr2 to i32*
  %blemptr = getelementptr i32, i32* %cleanptr1, i32 2
  %dirtyptr = getelementptr i32, i32* %cleanptr1, i32 9
  %ptr1 = select i1 %arg1, i32* %cleanptr1, i32* %cleanptr2  ; clean,clean = clean
  store i32 37, i32* %ptr1
  %ptr2 = select i1 %arg2, i32* %cleanptr1, i32* %dirtyptr  ; clean,dirty = dirty
  store i32 63, i32* %ptr2
  %ptr3 = select i1 %arg1, i32* %blemptr, i32* %cleanptr2  ; blem,clean = blem
  %loaded1 = load i32, i32* %ptr3
  %ptr4 = select i1 %arg2, i32* %blemptr, i32* %unkptr1  ; blem,unk = unk
  %loaded2 = load i32, i32* %ptr4
  %ptr5 = select i1 %arg1, i32* %dirtyptr, i32* %unkptr2  ; dirty,unk = dirty
  %loaded3 = load i32, i32* %ptr5
  %ptr6 = select i1 %arg2, i32* %unkptr1, i32* %unkptr2  ; unk,unk = unk
  %loaded4 = load i32, i32* %ptr6
  ret void
}

; This checks that inttoptr produces dirty pointers
; CHECK-LABEL: inttoptr_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK: Producing a ptr (assumed dirty) from inttoptr: 1
define void @inttoptr_dirty(i64 %arg) {
  %ptr = inttoptr i64 %arg to i32*
  %loaded = load i32, i32* %ptr
  ret void
}

; This checks that function parameters are considered UNKNOWN pointers
; (which is our current assumption).
; CHECK-LABEL: func_param
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 1
define void @func_param(i32* %ptr) {
  store i32 3, i32* %ptr
  ret void
}

; This checks that pointers returned from calls are considered UNKNOWN
; (which is our current assumption).
; CHECK-LABEL: func_ret
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
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

; This checks that pointers loaded from memory are considered UNKNOWN
; (which is our current assumption).
; CHECK-LABEL: from_mem
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @from_mem(i32 %arg) {
  %ptrptr = alloca [16 x i32*]
  %castedptrptr = bitcast [16 x i32*]* %ptrptr to i32**
  %loadedptr = load i32*, i32** %castedptrptr
  %res = load i32, i32* %loadedptr
  ret i32 %res
}

; This checks that a nonzero GEP on an UNKNOWN pointer produces a dirty
; pointer.
; CHECK-LABEL: gep_on_unk
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with blemished addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
define void @gep_on_unk(i32* %arg) {
  %gepptr = getelementptr i32, i32* %arg, i32 1
  store i32 3, i32* %gepptr
  ret void
}
