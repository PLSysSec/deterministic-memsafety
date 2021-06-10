; RUN: opt -passes=static-dlim -disable-output < %s 2>&1 | FileCheck %s

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
