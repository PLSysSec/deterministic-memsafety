; RUN: clang -fdms=dynamic-stdout %s -o %t && %t | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,dms-bounds -disable-output < %s 2>&1 > /dev/null

; Since we currently print dynamic counts on a per-module basis, the following
; totals are for this entire file.
; CHECK-LABEL: DMS dynamic counts
; CHECK-NEXT: =====
; CHECK-NEXT: Loads with clean addr: 273
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 53
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 11
; CHECK-NEXT: Stores with blemished16 addr: 102
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 1
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 7
; CHECK-NEXT: Storing a blemished16 ptr to mem: 0
; CHECK-NEXT: Storing a blemished32 ptr to mem: 0
; CHECK-NEXT: Storing a blemished64 ptr to mem: 0
; CHECK-NEXT: Storing a blemishedconst ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 4
; CHECK-NEXT: Storing an unknown ptr to mem: 0
; CHECK-NEXT: Passing a clean ptr to a func: 0
; CHECK-NEXT: Passing a blemished16 ptr to a func: 0
; CHECK-NEXT: Passing a blemished32 ptr to a func: 0
; CHECK-NEXT: Passing a blemished64 ptr to a func: 0
; CHECK-NEXT: Passing a blemishedconst ptr to a func: 0
; CHECK-NEXT: Passing a dirty ptr to a func: 0
; CHECK-NEXT: Passing an unknown ptr to a func: 0
; CHECK-NEXT: Returning a clean ptr from a func: 0
; CHECK-NEXT: Returning a blemished16 ptr from a func: 0
; CHECK-NEXT: Returning a blemished32 ptr from a func: 0
; CHECK-NEXT: Returning a blemished64 ptr from a func: 0
; CHECK-NEXT: Returning a blemishedconst ptr from a func: 0
; CHECK-NEXT: Returning a dirty ptr from a func: 0
; CHECK-NEXT: Returning an unknown ptr from a func: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a clean ptr: 102
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished16 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished32 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished64 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemishedconst ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a dirty ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on an unknown ptr: 0
; CHECK-NEXT: Producing a ptr from inttoptr: 0

define i32 @main() {
  %call1 = call i32 @clean_load_different_block(i32 2)
  %call2 = call i32 @dirty_load_different_block(i32 2)
  %call3 = call i32 @clean_load_two_preds_no_phi(i32 2)
  %call4 = call i32 @clean_load_two_dirty_preds_no_phi(i32 2)
  %call5 = call i32 @many_blocks_loop(i32 2)
  %call6 = call i32 @phi_dyn_and_static_clean(i32 2)
  %call7 = call i32 @phi_both_dyn_clean(i32 2)
  %call8 = call i32 @phi_one_dyn_dirty(i32 2)
  %call9 = call i32 @phi_dyn_dirty_static_clean(i32 2)
  %call10 = call i32 @dyn_clean_nested_loop(i32 2)
  ret i32 0
}

; Load a DYN_CLEAN pointer, dereference it in a different block.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 2
; Stores with clean addr: 1
; Storing a clean ptr to mem: 1
define i32 @clean_load_different_block(i32 %arg) {
  %ptr = alloca i32, align 4
  %ptrptr = alloca i32*, align 4
  store i32* %ptr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %dyncleanptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %b_res = load i32, i32* %dyncleanptr, align 4
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; Load a DYN_DIRTY pointer, dereference it in a different block.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 1
; Loads with dirty addr: 1
; Stores with clean addr: 1
; Storing a dirty ptr to mem: 1
define i32 @dirty_load_different_block(i32 %arg) {
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %dirtyptr = getelementptr i32, i32* %arr, i32 %arg ; dirty because the offset is not a compile-time constant
  %ptrptr = alloca i32*, align 4
  store i32* %dirtyptr, i32** %ptrptr, align 4 ; storing dirty ptr to clean address
  %dyndirtyptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_DIRTY ptr
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %b_res = load i32, i32* %dyndirtyptr, align 4
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; This checks that the load is still clean even when the DYN_CLEAN pointer
; comes from either of two predecessors of the block. (No PHI.)
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 2
; Stores with clean addr: 1
; Storing a clean ptr to mem: 1
define i32 @clean_load_two_preds_no_phi(i32 %arg) {
  %ptr = alloca i32, align 4
  %ptrptr = alloca i32*, align 4
  store i32* %ptr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %dyncleanptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  br label %end

b:
  br label %end

end:
  %res = load i32, i32* %dyncleanptr, align 4
  ret i32 %res
}

; Same as the above case, but the pointer starts DYN_DIRTY and is made clean in
; both predecessors.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 2
; Stores with clean addr: 1
; Stores with dirty addr: 1
; Storing a dirty ptr to mem: 1
define i32 @clean_load_two_dirty_preds_no_phi(i32 %arg) {
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %dirtyptr = getelementptr i32, i32* %arr, i32 %arg ; dirty because the offset is not a compile-time constant
  %ptrptr = alloca i32*, align 4
  store i32* %dirtyptr, i32** %ptrptr, align 4 ; storing dirty ptr to clean address
  %dyndirtyptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_DIRTY ptr
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %val_a = add i32 %arg, 3
  store i32 %val_a, i32* %dyndirtyptr
  br label %end

b:
  %val_b = add i32 %arg, 5
  store i32 %val_b, i32* %dyndirtyptr
  br label %end

end:
  %res = load i32, i32* %dyndirtyptr, align 4
  ret i32 %res
}

; This checks that you can jump through many blocks, then through a loop, and
; the DYN_CLEAN pointer still stays DYN_CLEAN.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 2
; Stores with clean addr: 1
; Storing a clean ptr to mem: 1
define i32 @many_blocks_loop(i32 %arg) {
  %ptr = alloca i32, align 4
  %ptrptr = alloca i32*, align 4
  store i32* %ptr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %dyncleanptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
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
  %res_e = load i32, i32* %dyncleanptr, align 4
  br label %end

end:
  %res = phi i32 [ %res_e, %e ], [ %res_a, %a ]
  ret i32 %res
}

; This checks that loading from a PHI'd pointer, where one possibility is
; statically CLEAN and the other is DYN_CLEAN, is a clean load.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 1 + 52 = 53
; Stores with clean addr: 1
; Stores with blemished16 addr: 51
; Storing a clean ptr to mem: 1
; Nonzero constant pointer arithmetic on a clean ptr: 51
define i32 @phi_dyn_and_static_clean(i32 %arg) {
start:
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %ptrptr = alloca i32*, align 4
  store i32* %arr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %dyncleanptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %dyncleanptr, %start ], [ %newptr, %body ]  ; clean
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %i = phi i32 [ 0, %start ], [ %new_i, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be clean load
  %loop_cond = icmp uge i32 %i, 51
  br i1 %loop_cond, label %end, label %body

body:
  %newptr = getelementptr i32, i32* %loop_ptr, i32 1  ; blemished
  store i32 1, i32* %newptr  ; blemished store, but now %newptr is statically clean
  %new_res = add i32 %loop_res, %loaded
  %new_i = add i32 %i, 1
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; This checks that loading from a PHI'd pointer, where both possibilities
; are DYN_CLEAN, is a clean load.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 1 + 52 + 51 = 104
; Stores with clean addr: 1
; Storing a clean ptr to mem: 1
define i32 @phi_both_dyn_clean(i32 %arg) {
start:
  %ptr = alloca i32, align 4
  %ptrptr = alloca i32*, align 4
  store i32* %ptr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %dyncleanptr1 = load volatile i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %dyncleanptr1, %start ], [ %dyncleanptr2, %body ]  ; both DYN_CLEAN
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %i = phi i32 [ 0, %start ], [ %new_i, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be clean load
  %loop_cond = icmp uge i32 %i, 51
  br i1 %loop_cond, label %end, label %body

body:
  %dyncleanptr2 = load volatile i32*, i32** %ptrptr, align 4 ; same as dyncleanptr1
  %new_res = add i32 %loop_res, %loaded
  %new_i = add i32 %i, 1
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; This checks that loading from a PHI'd pointer, where one possibility is
; DYN_CLEAN and one is DYN_DIRTY, is clean or dirty depending on the actual
; dynamic PHI source each iteration.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 1 + 1 + 51 = 53
; Loads with dirty addr: 51
; Stores with clean addr: 2
; Storing a clean ptr to mem: 1
; Storing a dirty ptr to mem: 1
define i32 @phi_one_dyn_dirty(i32 %arg) {
start:
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %dirtyptr = getelementptr i32, i32* %arr, i32 %arg ; dirty because the offset is not a compile-time constant
  %ptrptrclean = alloca i32*, align 4
  %ptrptrdirty = alloca i32*, align 4
  store i32* %arr, i32** %ptrptrclean, align 4 ; storing clean ptr to clean address
  store i32* %dirtyptr, i32** %ptrptrdirty, align 4 ; storing dirty ptr to clean address
  %dyncleanptr = load i32*, i32** %ptrptrclean, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %dyncleanptr, %start ], [ %dyndirtyptr, %body ]
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %i = phi i32 [ 0, %start ], [ %new_i, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be clean load the first time, dirty all others
  %loop_cond = icmp uge i32 %i, 51
  br i1 %loop_cond, label %end, label %body

body:
  %dyndirtyptr = load volatile i32*, i32** %ptrptrdirty, align 4 ; loading the pointer back from clean address, resulting in DYN_DIRTY ptr
  %new_res = add i32 %loop_res, %loaded
  %new_i = add i32 %i, 1
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; This checks that loading from a PHI'd pointer, where one possibility is
; statically CLEAN and the other is DYN_DIRTY, is clean or dirty depending
; on the actual dynamic PHI source each iteration.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 1 + 51 = 52
; Loads with dirty addr: 1
; Stores with clean addr: 1
; Stores with blemished16 addr: 51
; Storing a dirty ptr to mem: 1
; Nonzero constant pointer arithmetic on a clean ptr: 51
define i32 @phi_dyn_dirty_static_clean(i32 %arg) {
start:
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %dirtyptr = getelementptr i32, i32* %arr, i32 %arg ; dirty because the offset is not a compile-time constant
  %ptrptr = alloca i32*, align 4
  store i32* %dirtyptr, i32** %ptrptr, align 4 ; storing dirty ptr to clean address
  %dyndirtyptr = load i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_DIRTY ptr
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %dyndirtyptr, %start ], [ %newptr, %body ]  ; clean
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %i = phi i32 [ 0, %start ], [ %new_i, %body ]
  %loaded = load i32, i32* %loop_ptr  ; should be dirty load
  %loop_cond = icmp uge i32 %i, 51
  br i1 %loop_cond, label %end, label %body

body:
  %newptr = getelementptr i32, i32* %loop_ptr, i32 1  ; blemished
  store i32 1, i32* %newptr  ; blemished store, but now %newptr is statically clean
  %new_res = add i32 %loop_res, %loaded
  %new_i = add i32 %i, 1
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; Here, the DYN_CLEAN pointer goes through some more complicated control flow
; (including a nested loop); it should still be DYN_CLEAN at the end.
; This test is more to make sure the dynamic pass can handle instrumenting this
; (including merging dynamic statuses at tops of blocks) and less about checking
; the runtime behavior.
; (Dynamic) totals for this function (arg <= 4):
; Loads with clean addr: 2
; Stores with clean addr: 1
; Storing a clean ptr to mem: 1
define i32 @dyn_clean_nested_loop(i32 %arg) {
start:
  %ptr = alloca i32, align 4
  %ptrptr = alloca i32*, align 4
  store i32* %ptr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %dyncleanptr = load volatile i32*, i32** %ptrptr, align 4 ; loading the pointer back from clean address, resulting in DYN_CLEAN ptr
  br label %a

a:  ; preds: start, d
  br label %b

b:  ; preds: a, c
  br label %c

c:  ; preds: b
  %cond1 = icmp ugt i32 %arg, 4
  br i1 %cond1, label %b, label %d

d:  ; preds: c
  %cond2 = icmp ugt i32 %arg, 6
  br i1 %cond2, label %a, label %exit

exit:  ; preds: d
  %res = load i32, i32* %dyncleanptr, align 4  ; should be dyn_clean load
  ret i32 %res
}
