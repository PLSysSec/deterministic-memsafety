; RUN: opt -passes=dms-static -disable-output < %s 2>&1 | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,dms-bounds -disable-output < %s 2>&1 > /dev/null

; This very basic loop has a clean load in every iteration.
; CHECK-LABEL: basic_loop
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @basic_loop(i32 %len) {
entry:
  %allocated = alloca [64 x i32]
  %arr = bitcast [64 x i32]* %allocated to i32*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 0, %entry ]
  %sum = phi i32 [ %newsum, %loop ], [ 0, %entry ]
  %loaded = load i32, i32* %arr
  %newsum = add i32 %sum, %loaded
  %new_i = add i32 %i, 1
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i32 %newsum
}

; In this loop, each load's address is at most 4 more than the previous,
; so the load should be considered BLEMISHED16.
; This example continuously increments the same pointer.
; CHECK-LABEL: loop_with_ptr_inc
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @loop_with_ptr_inc(i32 %len) {
entry:
  %allocated = alloca [64 x i32]
  %arr = bitcast [64 x i32]* %allocated to i32*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 0, %entry ]
  %sum = phi i32 [ %newsum, %loop ], [ 0, %entry ]
  %arrptr = phi i32* [ %newarrptr, %loop ], [ %arr, %entry ]
  %loaded = load i32, i32* %arrptr
  %newsum = add i32 %sum, %loaded
  %new_i = add i32 %i, 1
  %newarrptr = getelementptr i32, i32* %arrptr, i32 1
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i32 %newsum
}

; Same, but the loads are array-style: each iteration, they are computed
; based on the array base and the index variable
; CHECK-LABEL: loop_over_array
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @loop_over_array(i32 %len) {
entry:
  %allocated = alloca [64 x i32]
  %arr = bitcast [64 x i32]* %allocated to i32*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 0, %entry ]
  %sum = phi i32 [ %newsum, %loop ], [ 0, %entry ]
  %arrptr = getelementptr i32, i32* %arr, i32 %i
  %loaded = load i32, i32* %arrptr
  %newsum = add i32 %sum, %loaded
  %new_i = add i32 %i, 1
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i32 %newsum
}

; Should get the same result even if `i` starts at 1
; CHECK-LABEL: loop_over_array_from_1
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @loop_over_array_from_1(i32 %len) {
entry:
  %allocated = alloca [64 x i32]
  %arr = bitcast [64 x i32]* %allocated to i32*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 1, %entry ]
  %sum = phi i32 [ %newsum, %loop ], [ 0, %entry ]
  %arrptr = getelementptr i32, i32* %arr, i32 %i
  %loaded = load i32, i32* %arrptr
  %newsum = add i32 %sum, %loaded
  %new_i = add i32 %i, 1
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i32 %newsum
}

; Should get the same result even if the array accesses both `i` and `i-1`
; CHECK-LABEL: loop_over_array_minus
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 2
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @loop_over_array_minus(i32 %len) {
entry:
  %allocated = alloca [64 x i32]
  %arr = bitcast [64 x i32]* %allocated to i32*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 1, %entry ]
  %sum = phi i32 [ %newsum2, %loop ], [ 0, %entry ]
  %arrptr = getelementptr i32, i32* %arr, i32 %i
  %loaded1 = load i32, i32* %arrptr
  %newsum = add i32 %sum, %loaded1
  %iMinusOne = add i32 %i, -1
  %arrptrMinusOne = getelementptr i32, i32* %arr, i32 %iMinusOne
  %loaded2 = load i32, i32* %arrptrMinusOne
  %newsum2 = add i32 %newsum, %loaded2
  %new_i = add i32 %i, 1
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i32 %newsum2
}

; Here, in each iteration the pointer is 24 more than it was in the previous
; iteration. So, the load should be BLEMISHED32
; CHECK-LABEL: loop_larger_stride
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 1
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i64 @loop_larger_stride(i32 %len) {
entry:
  %allocated = alloca [64 x i64]
  %arr = bitcast [64 x i64]* %allocated to i64*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 0, %entry ]
  %sum = phi i64 [ %newsum, %loop ], [ 0, %entry ]
  %arrptr = getelementptr i64, i64* %arr, i32 %i
  %loaded = load i64, i64* %arrptr
  %newsum = add i64 %sum, %loaded
  %new_i = add i32 %i, 3
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i64 %newsum
}

; Here, in each iteration the pointer is 30 more than it was in the previous
; iteration. So, the load should be BLEMISHED32. (The important part of this
; test is that the array is of i16, which is different from i8 and from the
; pointer size)
; CHECK-LABEL: loop_another_larger_stride
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 1
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i16 @loop_another_larger_stride(i32 %len) {
entry:
  %allocated = alloca [64 x i16]
  %arr = bitcast [64 x i16]* %allocated to i16*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %loop ], [ 0, %entry ]
  %sum = phi i16 [ %newsum, %loop ], [ 0, %entry ]
  %arrptr = getelementptr i16, i16* %arr, i32 %i
  %loaded = load i16, i16* %arrptr
  %newsum = add i16 %sum, %loaded
  %new_i = add i32 %i, 15
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  ret i16 %newsum
}

; In this loop, we continually increment a pointer without deref'ing it.
; The eventual deref (after the loop) should be marked dirty, since the pointer
; could be incremented without bound (from the standpoint of the static
; analysis)
; CHECK-LABEL: loop_with_phi_ptr
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
@str = private unnamed_addr constant [96 x i8] c"Hello world hello world hello world hello world hello world hello world hello world hello world\00", align 1
define i8 @loop_with_phi_ptr(i8 %max) {
entry:
  br label %loop

loop:
  %ptr = phi i8* [ %new_ptr, %loop ], [ getelementptr inbounds ([96 x i8], [96 x i8]* @str, i32 0, i32 0), %entry ]
  %sum = phi i8 [ %new_sum, %loop ], [ 0, %entry ]
  %new_ptr = getelementptr i8, i8* %ptr, i32 1
  %new_sum = add i8 %sum, 1
  %cmp = icmp ult i8 %new_sum, %max
  br i1 %cmp, label %loop, label %done

done:
  %loaded = load i8, i8* %new_ptr
  ret i8 %new_sum
}

; Generalization of the above, where there is more than one phi in the cyclic
; definition. Like the above, this is a simplified version of real code observed
; in 471.omnetpp
define i8 @loop_with_multiple_phi_ptrs(i8 %max) {
entry:
  br label %loop

loop:
  %ptr = phi i8* [ %new_ptr, %loop2 ], [ getelementptr inbounds ([96 x i8], [96 x i8]* @str, i32 0, i32 0), %entry ]
  %sum = phi i8 [ %new_sum, %loop2 ], [ 0, %entry ]
  %cmp1 = icmp eq i8 %max, 0
  br i1 %cmp1, label %trueblock, label %falseblock

trueblock:
  br label %loop2

falseblock:
  br label %loop2

loop2:
  %ptr2 = phi i8* [ %ptr, %trueblock ], [ getelementptr inbounds ([96 x i8], [96 x i8]* @str, i32 0, i32 0), %falseblock ]
  %new_ptr = getelementptr i8, i8* %ptr2, i32 1
  %new_sum = add i8 %sum, 1
  %cmp2 = icmp ult i8 %new_sum, %max
  br i1 %cmp2, label %loop, label %done

done:
  %loaded = load i8, i8* %new_ptr
  ret i8 %new_sum
}

; In this case, since we _might not_ dereference the pointer in _every_ iteration,
; we can't use the induction assumption. This access has to be dirty.
; CHECK-LABEL: loop_might_not_deref
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 2
; CHECK-NEXT: Loads with unknown addr: 0
declare i1 @should_load()
define i32 @loop_might_not_deref(i32 %len) {
entry:
  %allocated = alloca [64 x i32]
  %arr = bitcast [64 x i32]* %allocated to i32*
  br label %loop

loop:
  %i = phi i32 [ %new_i, %finish_loop ], [ 0, %entry ]
  %sum = phi i32 [ %newsum, %finish_loop ], [ 0, %entry ]
  %arrptr = getelementptr i32, i32* %arr, i32 %i
  %should_load = call i1 @should_load()
  br i1 %should_load, label %do_load, label %finish_loop

do_load:
  %loaded = load i32, i32* %arrptr
  %loaded_newsum = add i32 %sum, %loaded
  br label %finish_loop

finish_loop:
  %newsum = phi i32 [ %loaded_newsum, %do_load ], [ %sum, %loop ]
  %new_i = add i32 %i, 1
  %cmp = icmp ult i32 %new_i, %len
  br i1 %cmp, label %loop, label %done

done:
  %final_load = load i32, i32* %arrptr ; this one also has to be dirty - arrptr could have been incremented arbitrarily (dynamically) far since last deref
  ret i32 %final_load
}
