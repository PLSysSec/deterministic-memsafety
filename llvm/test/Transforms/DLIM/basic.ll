; RUN: opt -passes=dlim -disable-output < %s 2>&1 | FileCheck %s

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_load
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @clean_load() {
  %ptr = alloca i32, align 4
  %res = load i32, i32* %ptr, align 4
  ret i32 %res
}

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_store
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with dirty addr: 0
define void @clean_store(i32 %arg) {
  %ptr = alloca i32, align 4
  store i32 %arg, i32* %ptr, align 4
  ret void
}

; This checks that bitcasting a clean pointer is still a clean pointer.
; CHECK-LABEL: bitcast_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i8 @bitcast_clean(i32 %arg) {
  %ptr = alloca i32, align 4
  %newptr = bitcast i32* %ptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that a pointer we've done arithmetic on is considered dirty.
; CHECK-LABEL: dirty_load
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @dirty_load() {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that some more complicated pointer arithmetic also results in a
; dirty pointer.
; CHECK-LABEL: dirty_load_more_complicated
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @dirty_load_more_complicated() {
  %ptr = alloca [4 x { i32, i32 }]
  %castedptr = bitcast [4 x { i32, i32 }]* %ptr to { i32, i32 }*
  %newptr = getelementptr { i32, i32 }, { i32, i32 }* %castedptr, i32 0, i32 1
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that a pointer we've added 0 to is still clean.
; CHECK-LABEL: gep_still_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @gep_still_clean() {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 0
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that bitcasting a dirty pointer is still a dirty pointer.
; CHECK-LABEL: bitcast_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i8 @bitcast_dirty(i32 %arg) {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %castedptr, i32 2
  %newptr = bitcast i32* %dirtyptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that loading from a dirty pointer makes it clean.
; CHECK-LABEL: load_makes_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @load_makes_clean() {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  %res = load i32, i32* %newptr
  %res2 = load i32, i32* %newptr
  ret i32 %res2
}

; This checks that storing to a dirty pointer makes it clean.
; CHECK-LABEL: store_makes_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
define i32 @store_makes_clean(i32 %arg) {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  store i32 %arg, i32* %newptr
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks our counting of storing clean/dirty pointers.
; CHECK-LABEL: store_clean_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 4
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 2
; CHECK-NEXT: Storing a dirty ptr to mem: 2
define void @store_clean_dirty() {
  %ptr = alloca [4 x i32]
  %cleanptr = bitcast [4 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 2
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
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 0
; CHECK-NEXT: Passing a clean ptr to a func: 6
; CHECK-NEXT: Passing a dirty ptr to a func: 4
declare i32* @takes_ptr(i32*)
declare i32* @takes_two_ptrs(i32*, i32*)
declare void @takes_lots_of_things(i32, i64, i32*, i64*, i1)
define void @passing_args() {
  %ptr = alloca [4 x i32]
  %cleanptr = bitcast [4 x i32]* %ptr to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr, i32 2
  %res1 = call i32* @takes_ptr(i32* %cleanptr)
  %res2 = call i32* @takes_ptr(i32* %dirtyptr)
  %res3 = call i32* @takes_ptr(i32* %cleanptr)
  %res4 = call i32* @takes_ptr(i32* %dirtyptr)
  %res5 = call i32* @takes_two_ptrs(i32* %cleanptr, i32* %dirtyptr)
  %res6 = call i32* @takes_two_ptrs(i32* %cleanptr, i32* %cleanptr)
  %clean64ptr = bitcast i32* %cleanptr to i64*
  call void @takes_lots_of_things(i32 76, i64 0, i32* %dirtyptr, i64* %clean64ptr, i1 1)
  ret void
}

; This checks that the load is still clean even when the alloca was in a
; different block.
; CHECK-LABEL: clean_load_different_block
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @clean_load_different_block(i32 %arg) {
  %ptr = alloca i32, align 4
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %b_res = load i32, i32* %ptr, align 4
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; This checks that the load is still dirty even when the GEP was in a
; different block.
; CHECK-LABEL: dirty_load_different_block
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @dirty_load_different_block(i32 %arg) {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %b_res = load i32, i32* %newptr, align 4
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; This checks that a GEP result adding 0 is still clean, even when the
; alloca was in a different block.
; CHECK-LABEL: clean_gep_far_alloca
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @clean_gep_far_alloca(i32 %arg) {
  %ptr = alloca i32, align 4
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %newptr = getelementptr i32, i32* %ptr, i32 0
  %b_res = load i32, i32* %newptr, align 4
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; This checks that the load is still clean even when the clean pointer
; comes from either of two predecessors of the block. (No PHI.)
; CHECK-LABEL: clean_load_two_preds_no_phi
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @clean_load_two_preds_no_phi(i32 %arg) {
  %ptr = alloca i32, align 4
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  br label %end

b:
  br label %end

end:
  %res = load i32, i32* %ptr, align 4
  ret i32 %res
}

; Same as the above case, but the pointer starts dirty and is made clean in
; both predecessors.
; CHECK-LABEL: clean_load_two_dirty_preds_no_phi
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 2
define i32 @clean_load_two_dirty_preds_no_phi(i32 %arg) {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %val_a = add i32 %arg, 3
  store i32 %val_a, i32* %newptr
  br label %end

b:
  %val_b = add i32 %arg, 5
  store i32 %val_b, i32* %newptr
  br label %end

end:
  %res = load i32, i32* %newptr, align 4
  ret i32 %res
}

; This checks that you can jump through many blocks and the clean pointer still
; stays clean.
; CHECK-LABEL: many_blocks
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @many_blocks(i32 %arg) {
  %ptr = alloca i32, align 4
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %res_a = add i32 %arg, 14
  br label %end

b:
  br label %c

c:
  br label %d

d:
  br label %e

e:
  %res_e = load i32, i32* %ptr, align 4
  br label %end

end:
  %res = phi i32 [ %res_e, %e ], [ %res_a, %a ]
  ret i32 %res
}

; This checks that loading from a PHI'd pointer, where both possibilities
; are clean, is a clean load.
; CHECK-LABEL: phi_both_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
define i32 @phi_both_clean(i32 %arg) {
start:
  %ptr = alloca [4 x i32]  ; clean
  %castedptr = bitcast [4 x i32]* %ptr to i32*  ; clean
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %castedptr, %start ], [ %newptr, %body ]  ; clean
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be clean load
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %end, label %body

body:
  %newptr = getelementptr i32, i32* %loop_ptr, i32 1  ; dirty
  store i32 1, i32* %newptr  ; dirty store, but now %newptr is clean
  %new_res = add i32 %loop_res, %loaded
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; This checks that loading from a PHI'd pointer, where one possibility is
; clean and one is dirty, is a dirty load.
; CHECK-LABEL: phi_one_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @phi_one_dirty(i32 %arg) {
start:
  %ptr = alloca [4 x i32]  ; clean
  %castedptr = bitcast [4 x i32]* %ptr to i32*  ; clean
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %castedptr, %start ], [ %newptr, %body ]  ; dirty because newptr is dirty
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be dirty load
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %end, label %body

body:
  %newptr = getelementptr i32, i32* %loop_ptr, i32 1  ; dirty
  %new_res = add i32 %loop_res, %loaded
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; In this test, the same pointer is used clean in one branch, but dirty in the
; other. At the bottom, we must pessimistically assume dirty.
; CHECK-LABEL: half_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 2
define i32 @half_dirty(i32 %arg) {
start:
  %initialptr = alloca [4 x i32]  ; clean
  %castedptr = bitcast [4 x i32]* %initialptr to i32*  ; clean
  %ptr = getelementptr i32, i32* %castedptr, i32 1  ; dirty
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %branch1, label %branch2

branch1:
  store i32 7, i32* %ptr  ; now ptr is clean
  br label %end

branch2:
  %anotherptr = getelementptr i32, i32* %ptr, i32 1  ; both ptr and anotherptr are still dirty
  store i32 7, i32* %anotherptr  ; now anotherptr is clean, but ptr is still dirty
  br label %end

end:
  %res = load i32, i32* %ptr  ; must assume dirty since it's dirty if we come from branch2
  ret i32 %res
}

; This tests that select of two clean pointers is clean, and select of a clean
; and a dirty pointer is dirty.
; CHECK-LABEL: select_ptrs
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with dirty addr: 1
define void @select_ptrs(i1 %arg1, i1 %arg2) {
  %initialptr1 = alloca [4 x i32]
  %cleanptr1 = bitcast [4 x i32]* %initialptr1 to i32*
  %initialptr2 = alloca [4 x i32]
  %cleanptr2 = bitcast [4 x i32]* %initialptr2 to i32*
  %dirtyptr = getelementptr i32, i32* %cleanptr1, i32 2
  %ptr1 = select i1 %arg1, i32* %cleanptr1, i32* %cleanptr2
  store i32 37, i32* %ptr1
  %ptr2 = select i1 %arg2, i32* %cleanptr1, i32* %dirtyptr
  store i32 63, i32* %ptr2
  ret void
}

; This checks that function parameters are considered clean pointers
; (which is our current assumption).
; CHECK-LABEL: func_param_clean
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with dirty addr: 0
define void @func_param_clean(i32* %ptr) {
  store i32 3, i32* %ptr
  ret void
}

; This checks that pointers returned from calls are considered clean
; (which is our current assumption).
; CHECK-LABEL: func_ret_clean
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 2
; CHECK-NEXT: Stores with dirty addr: 0
declare i32* @asdfjkl()
declare i32* @fdsajkl()
define void @func_ret_clean() {
  %ptr1 = call i32* @asdfjkl()
  %ptr2 = call nonnull align 8 dereferenceable(64) i32* @fdsajkl()
  store i32 3, i32* %ptr1
  store i32 4, i32* %ptr2
  ret void
}

; This checks that pointers loaded from memory are considered clean
; (which is our current assumption).
; CHECK-LABEL: clean_from_mem
; CHECK-NEXT: Loads with clean addr: 2
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @clean_from_mem(i32 %arg) {
  %ptrptr = alloca [4 x i32*]
  %castedptrptr = bitcast [4 x i32*]* %ptrptr to i32**
  %loadedptr = load i32*, i32** %castedptrptr
  %res = load i32, i32* %loadedptr
  ret i32 %res
}

; This checks that `inttoptr` produces dirty pointers
; CHECK-LABEL: inttoptr_dirty
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Stores with clean addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
define i32 @inttoptr_dirty(i64 %arg) {
  %ptr = inttoptr i64 %arg to i32*
  %res = load i32, i32* %ptr
  ret i32 %res
}
