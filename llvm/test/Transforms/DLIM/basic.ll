; RUN: opt -passes=dlim -disable-output < %s 2>&1 | FileCheck %s

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_load
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
define i32 @clean_load() {
  %ptr = alloca i32, align 4
  %res = load i32, i32* %ptr, align 4
  ret i32 %res
}

; This checks that a pointer fresh from an alloca is considered clean.
; CHECK-LABEL: clean_store
; CHECK-NEXT: Clean loads: 0
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 1
; CHECK-NEXT: Dirty stores: 0
define void @clean_store(i32 %arg) {
  %ptr = alloca i32, align 4
  store i32 %arg, i32* %ptr, align 4
  ret void
}

; This checks that bitcasting a clean pointer is still a clean pointer.
; CHECK-LABEL: bitcast_clean
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
define i8 @bitcast_clean(i32 %arg) {
  %ptr = alloca i32, align 4
  %newptr = bitcast i32* %ptr to i8*
  %res = load i8, i8* %newptr
  ret i8 %res
}

; This checks that a pointer we've done arithmetic on is considered dirty.
; CHECK-LABEL: dirty_load
; CHECK-NEXT: Clean loads: 0
; CHECK-NEXT: Dirty loads: 1
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 0
; CHECK-NEXT: Dirty loads: 1
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
define i32 @dirty_load_more_complicated() {
  %ptr = alloca [4 x { i32, i32 }]
  %castedptr = bitcast [4 x { i32, i32 }]* %ptr to { i32, i32 }*
  %newptr = getelementptr { i32, i32 }, { i32, i32 }* %castedptr, i32 0, i32 1
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that a pointer we've added 0 to is still clean.
; CHECK-LABEL: gep_still_clean
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
define i32 @gep_still_clean() {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 0
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that bitcasting a dirty pointer is still a dirty pointer.
; CHECK-LABEL: bitcast_dirty
; CHECK-NEXT: Clean loads: 0
; CHECK-NEXT: Dirty loads: 1
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 1
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 1
define i32 @store_makes_clean(i32 %arg) {
  %ptr = alloca [4 x i32]
  %castedptr = bitcast [4 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 2
  store i32 %arg, i32* %newptr
  %res = load i32, i32* %newptr
  ret i32 %res
}

; This checks that the load is still clean even when the alloca was in a
; different block.
; CHECK-LABEL: clean_load_different_block
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 0
; CHECK-NEXT: Dirty loads: 1
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 2
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 0
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
; CHECK-NEXT: Clean loads: 1
; CHECK-NEXT: Dirty loads: 0
; CHECK-NEXT: Clean stores: 0
; CHECK-NEXT: Dirty stores: 1
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

; This checks that function parameters are considered clean pointers
; (which is our current assumption).

; This checks that pointers loaded from memory are considered clean
; (which is our current assumption).
