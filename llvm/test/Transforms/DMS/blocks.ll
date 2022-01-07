; RUN: opt -passes=static-dms -disable-output < %s 2>&1 | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,bounds-dms -disable-output < %s 2>&1 > /dev/null

; This checks that the load is still clean even when the alloca was in a
; different block.
; CHECK-LABEL: clean_load_different_block
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
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
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @dirty_load_different_block(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 %arg
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
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
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
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
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
; CHECK-NEXT: Stores with dirty addr: 2
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @clean_load_two_dirty_preds_no_phi(i32 %arg) {
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %newptr = getelementptr i32, i32* %castedptr, i32 %arg
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
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
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

; This checks that you can jump through many blocks, then through a loop, and
; the clean pointer still stays clean.
; CHECK-LABEL: many_blocks_loop
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @many_blocks_loop(i32 %arg) {
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
  %i = phi i32 [ %new_i, %d ], [ 0, %c ]
  %new_i = add i32 %i, 1
  %cmp = icmp ult i32 %i, %arg
  br i1 %cmp, label %d, label %e

e:
  %res_e = load i32, i32* %ptr, align 4
  br label %end

end:
  %res = phi i32 [ %res_e, %e ], [ %res_a, %a ]
  ret i32 %res
}

; This is a simplified example of something that came up in real code --
; we want to check that the load here is properly marked UNKNOWN.
; (Yes, LLVM had the blocks in this order in the real code.)
; CHECK-LABEL: multi_for_loops_example
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 1
declare i1 @do_cmp()
define i32 @multi_for_loops_example(i32* %unkptr) {
entry:
  %cmp0 = call i1 @do_cmp()
  br i1 %cmp0, label %forloop1.end, label %forloop1.body

forloop1.body:  ; preds = forloop1.body, entry
  %cmp1 = call i1 @do_cmp()
  br i1 %cmp1, label %forloop1.end, label %forloop1.body

forloop1.end:  ; preds = forloop1.body, entry
  %cmp2 = call i1 @do_cmp()
  br i1 %cmp2, label %forloop2.preheader, label %forloop2.body

forloop2.preheader:  ; preds = forloop2.body, forloop1.end
  %cmp3 = call i1 @do_cmp()
  br i1 %cmp3, label %done, label %forloop3.bodyupper

forloop2.body:  ; preds = forloop1.end, forloop2.body
  %cmp4 = call i1 @do_cmp()
  br i1 %cmp4, label %forloop2.preheader, label %forloop2.body

forloop3.preheader:  ; preds = forloop3.bottom
  %cmp5 = call i1 @do_cmp()
  br i1 %cmp5, label %do_load, label %done

do_load:  ; preds = forloop3.preheader
  %loaded = load i32, i32* %unkptr
  ret i32 %loaded

forloop3.bodyupper:  ; preds = forloop2.preheader, forloop3.bottom
  %cmp6 = call i1 @do_cmp()
  br i1 %cmp6, label %forloop3.bottom, label %forloop3.bodyinner

forloop3.bodyinner:  ; preds = forloop3.bodyupper, forloop3.bodyinner
  %cmp7 = call i1 @do_cmp()
  br i1 %cmp7, label %forloop3.bottom, label %forloop3.bodyinner

forloop3.bottom:  ; preds = forloop3.bodyupper, forloop3.bodyinner
  %cmp8 = call i1 @do_cmp()
  br i1 %cmp8, label %forloop3.bodyupper, label %forloop3.preheader

done:  ; preds = forloop2.preheader, forloop3.preheader
  ret i32 0
}

; This checks that loading from a PHI'd pointer, where both possibilities
; are clean, is a clean load.
; CHECK-LABEL: phi_both_clean
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
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
define i32 @phi_both_clean(i32 %arg) {
start:
  %ptr = alloca [16 x i32]  ; clean
  %castedptr = bitcast [16 x i32]* %ptr to i32*  ; clean
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %castedptr, %start ], [ %newptr, %body ]  ; clean
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be clean load
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %end, label %body

body:
  %newptr = getelementptr i32, i32* %loop_ptr, i32 1  ; blemished
  store i32 1, i32* %newptr  ; blemished store, but now %newptr is clean
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
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @phi_one_dirty(i32 %arg) {
start:
  %ptr = alloca [16 x i32]  ; clean
  %castedptr = bitcast [16 x i32]* %ptr to i32*  ; clean
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %castedptr, %start ], [ %newptr, %body ]  ; dirty because newptr is dirty
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be dirty load
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %end, label %body

body:
  %newptr = getelementptr i32, i32* %loop_ptr, i32 %arg  ; dirty
  %new_res = add i32 %loop_res, %loaded
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; In this test, a pointer is made clean in one branch, but stays dirty in the
; other. At the bottom, we must pessimistically assume dirty.
; CHECK-LABEL: half_dirty
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
; CHECK-NEXT: Stores with dirty addr: 2
; CHECK-NEXT: Stores with unknown addr: 0
define i32 @half_dirty(i32 %arg) {
start:
  %initialptr = alloca [16 x i32]  ; clean
  %castedptr = bitcast [16 x i32]* %initialptr to i32*  ; clean
  %ptr = getelementptr i32, i32* %castedptr, i32 %arg  ; dirty
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %branch1, label %branch2

branch1:
  store i32 7, i32* %ptr  ; now ptr is clean
  br label %end

branch2:
  %anotherptr = getelementptr i32, i32* %ptr, i32 5  ; both ptr and anotherptr are still dirty
  store i32 7, i32* %anotherptr  ; now anotherptr is clean, but ptr is still dirty
  br label %end

end:
  %res = load i32, i32* %ptr  ; must assume dirty since it's dirty if we come from branch2
  ret i32 %res
}
