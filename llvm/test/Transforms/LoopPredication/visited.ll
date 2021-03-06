; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -loop-predication < %s 2>&1 | FileCheck %s
; RUN: opt -S -passes='require<scalar-evolution>,loop-mssa(loop-predication)' -verify-memoryssa < %s 2>&1 | FileCheck %s

declare void @llvm.experimental.guard(i1, ...)

define i32 @test_visited(i32* %array, i32 %length, i32 %n, i32 %x) {
; CHECK-LABEL: @test_visited(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[TMP5:%.*]] = icmp eq i32 [[N:%.*]], 0
; CHECK-NEXT:    br i1 [[TMP5]], label [[EXIT:%.*]], label [[LOOP_PREHEADER:%.*]]
; CHECK:       loop.preheader:
; CHECK-NEXT:    [[TMP0:%.*]] = icmp ule i32 [[N]], [[LENGTH:%.*]]
; CHECK-NEXT:    [[TMP1:%.*]] = icmp ult i32 0, [[LENGTH]]
; CHECK-NEXT:    [[TMP2:%.*]] = and i1 [[TMP1]], [[TMP0]]
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[LOOP_ACC:%.*]] = phi i32 [ [[LOOP_ACC_NEXT:%.*]], [[LOOP]] ], [ 0, [[LOOP_PREHEADER]] ]
; CHECK-NEXT:    [[I:%.*]] = phi i32 [ [[I_NEXT:%.*]], [[LOOP]] ], [ 0, [[LOOP_PREHEADER]] ]
; CHECK-NEXT:    [[UNRELATED_COND:%.*]] = icmp eq i32 [[X:%.*]], [[I]]
; CHECK-NEXT:    [[TMP3:%.*]] = and i1 [[UNRELATED_COND]], [[TMP2]]
; CHECK-NEXT:    call void (i1, ...) @llvm.experimental.guard(i1 [[TMP3]], i32 9) [ "deopt"() ]
; CHECK-NEXT:    [[I_I64:%.*]] = zext i32 [[I]] to i64
; CHECK-NEXT:    [[ARRAY_I_PTR:%.*]] = getelementptr inbounds i32, i32* [[ARRAY:%.*]], i64 [[I_I64]]
; CHECK-NEXT:    [[ARRAY_I:%.*]] = load i32, i32* [[ARRAY_I_PTR]], align 4
; CHECK-NEXT:    [[LOOP_ACC_NEXT]] = add i32 [[LOOP_ACC]], [[ARRAY_I]]
; CHECK-NEXT:    [[I_NEXT]] = add nuw i32 [[I]], 1
; CHECK-NEXT:    [[CONTINUE:%.*]] = icmp ult i32 [[I_NEXT]], [[N]]
; CHECK-NEXT:    br i1 [[CONTINUE]], label [[LOOP]], label [[EXIT_LOOPEXIT:%.*]]
; CHECK:       exit.loopexit:
; CHECK-NEXT:    [[LOOP_ACC_NEXT_LCSSA:%.*]] = phi i32 [ [[LOOP_ACC_NEXT]], [[LOOP]] ]
; CHECK-NEXT:    br label [[EXIT]]
; CHECK:       exit:
; CHECK-NEXT:    [[RESULT:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[LOOP_ACC_NEXT_LCSSA]], [[EXIT_LOOPEXIT]] ]
; CHECK-NEXT:    ret i32 [[RESULT]]
;
entry:
  %tmp5 = icmp eq i32 %n, 0
  br i1 %tmp5, label %exit, label %loop.preheader

loop.preheader:
  br label %loop

loop:
  %loop.acc = phi i32 [ %loop.acc.next, %loop ], [ 0, %loop.preheader ]
  %i = phi i32 [ %i.next, %loop ], [ 0, %loop.preheader ]
  %within.bounds = icmp ult i32 %i, %length
  %unrelated.cond = icmp eq i32 %x, %i
  %guard.cond.2 = and i1 %within.bounds, %unrelated.cond
  %guard.cond.3 = and i1 %guard.cond.2, %unrelated.cond
  %guard.cond.4 = and i1 %guard.cond.3, %guard.cond.2
  %guard.cond.5 = and i1 %guard.cond.4, %guard.cond.3
  %guard.cond.6 = and i1 %guard.cond.5, %guard.cond.4
  %guard.cond.7 = and i1 %guard.cond.6, %guard.cond.5
  %guard.cond.8 = and i1 %guard.cond.7, %guard.cond.6
  %guard.cond.9 = and i1 %guard.cond.8, %guard.cond.7
  %guard.cond.10 = and i1 %guard.cond.9, %guard.cond.8
  %guard.cond.11 = and i1 %guard.cond.10, %guard.cond.9
  %guard.cond.12 = and i1 %guard.cond.11, %guard.cond.10
  %guard.cond.13 = and i1 %guard.cond.12, %guard.cond.11
  %guard.cond.14 = and i1 %guard.cond.13, %guard.cond.12
  %guard.cond.15 = and i1 %guard.cond.14, %guard.cond.13
  %guard.cond.16 = and i1 %guard.cond.15, %guard.cond.14
  %guard.cond.17 = and i1 %guard.cond.16, %guard.cond.15
  %guard.cond.18 = and i1 %guard.cond.17, %guard.cond.16
  %guard.cond.19 = and i1 %guard.cond.18, %guard.cond.17
  %guard.cond.20 = and i1 %guard.cond.19, %guard.cond.18
  %guard.cond.21 = and i1 %guard.cond.20, %guard.cond.19
  %guard.cond.22 = and i1 %guard.cond.21, %guard.cond.20
  %guard.cond.23 = and i1 %guard.cond.22, %guard.cond.21
  %guard.cond.24 = and i1 %guard.cond.23, %guard.cond.22
  %guard.cond.25 = and i1 %guard.cond.24, %guard.cond.23
  %guard.cond.26 = and i1 %guard.cond.25, %guard.cond.24
  %guard.cond.27 = and i1 %guard.cond.26, %guard.cond.25
  %guard.cond.28 = and i1 %guard.cond.27, %guard.cond.26
  %guard.cond.29 = and i1 %guard.cond.28, %guard.cond.27
  %guard.cond.30 = and i1 %guard.cond.29, %guard.cond.28
  %guard.cond.31 = and i1 %guard.cond.30, %guard.cond.29
  %guard.cond.32 = and i1 %guard.cond.31, %guard.cond.30
  %guard.cond.33 = and i1 %guard.cond.32, %guard.cond.31
  %guard.cond.34 = and i1 %guard.cond.33, %guard.cond.32
  %guard.cond.35 = and i1 %guard.cond.34, %guard.cond.33
  %guard.cond.36 = and i1 %guard.cond.35, %guard.cond.34
  %guard.cond.37 = and i1 %guard.cond.36, %guard.cond.35
  %guard.cond.38 = and i1 %guard.cond.37, %guard.cond.36
  %guard.cond.39 = and i1 %guard.cond.38, %guard.cond.37
  %guard.cond.40 = and i1 %guard.cond.39, %guard.cond.38
  %guard.cond.41 = and i1 %guard.cond.40, %guard.cond.39
  %guard.cond.42 = and i1 %guard.cond.41, %guard.cond.40
  %guard.cond.43 = and i1 %guard.cond.42, %guard.cond.41
  %guard.cond.44 = and i1 %guard.cond.43, %guard.cond.42
  %guard.cond.45 = and i1 %guard.cond.44, %guard.cond.43
  %guard.cond.46 = and i1 %guard.cond.45, %guard.cond.44
  %guard.cond.47 = and i1 %guard.cond.46, %guard.cond.45
  %guard.cond.48 = and i1 %guard.cond.47, %guard.cond.46
  %guard.cond.49 = and i1 %guard.cond.48, %guard.cond.47
  %guard.cond.50 = and i1 %guard.cond.49, %guard.cond.48
  %guard.cond.51 = and i1 %guard.cond.50, %guard.cond.49
  %guard.cond.52 = and i1 %guard.cond.51, %guard.cond.50
  %guard.cond.53 = and i1 %guard.cond.52, %guard.cond.51
  %guard.cond.54 = and i1 %guard.cond.53, %guard.cond.52
  %guard.cond.55 = and i1 %guard.cond.54, %guard.cond.53
  %guard.cond.56 = and i1 %guard.cond.55, %guard.cond.54
  %guard.cond.57 = and i1 %guard.cond.56, %guard.cond.55
  %guard.cond.58 = and i1 %guard.cond.57, %guard.cond.56
  %guard.cond.59 = and i1 %guard.cond.58, %guard.cond.57
  %guard.cond.60 = and i1 %guard.cond.59, %guard.cond.58
  %guard.cond.61 = and i1 %guard.cond.60, %guard.cond.59
  %guard.cond.62 = and i1 %guard.cond.61, %guard.cond.60
  %guard.cond.63 = and i1 %guard.cond.62, %guard.cond.61
  %guard.cond.64 = and i1 %guard.cond.63, %guard.cond.62
  %guard.cond.65 = and i1 %guard.cond.64, %guard.cond.63
  %guard.cond.66 = and i1 %guard.cond.65, %guard.cond.64
  %guard.cond.67 = and i1 %guard.cond.66, %guard.cond.65
  %guard.cond.68 = and i1 %guard.cond.67, %guard.cond.66
  %guard.cond.69 = and i1 %guard.cond.68, %guard.cond.67
  %guard.cond.70 = and i1 %guard.cond.69, %guard.cond.68
  %guard.cond.71 = and i1 %guard.cond.70, %guard.cond.69
  %guard.cond.72 = and i1 %guard.cond.71, %guard.cond.70
  %guard.cond.73 = and i1 %guard.cond.72, %guard.cond.71
  %guard.cond.74 = and i1 %guard.cond.73, %guard.cond.72
  %guard.cond.75 = and i1 %guard.cond.74, %guard.cond.73
  %guard.cond.76 = and i1 %guard.cond.75, %guard.cond.74
  %guard.cond.77 = and i1 %guard.cond.76, %guard.cond.75
  %guard.cond.78 = and i1 %guard.cond.77, %guard.cond.76
  %guard.cond.79 = and i1 %guard.cond.78, %guard.cond.77
  %guard.cond.80 = and i1 %guard.cond.79, %guard.cond.78
  %guard.cond.81 = and i1 %guard.cond.80, %guard.cond.79
  %guard.cond.82 = and i1 %guard.cond.81, %guard.cond.80
  %guard.cond.83 = and i1 %guard.cond.82, %guard.cond.81
  %guard.cond.84 = and i1 %guard.cond.83, %guard.cond.82
  %guard.cond.85 = and i1 %guard.cond.84, %guard.cond.83
  %guard.cond.86 = and i1 %guard.cond.85, %guard.cond.84
  %guard.cond.87 = and i1 %guard.cond.86, %guard.cond.85
  %guard.cond.88 = and i1 %guard.cond.87, %guard.cond.86
  %guard.cond.89 = and i1 %guard.cond.88, %guard.cond.87
  %guard.cond.90 = and i1 %guard.cond.89, %guard.cond.88
  %guard.cond.91 = and i1 %guard.cond.90, %guard.cond.89
  %guard.cond.92 = and i1 %guard.cond.91, %guard.cond.90
  %guard.cond.93 = and i1 %guard.cond.92, %guard.cond.91
  %guard.cond.94 = and i1 %guard.cond.93, %guard.cond.92
  %guard.cond.95 = and i1 %guard.cond.94, %guard.cond.93
  %guard.cond.96 = and i1 %guard.cond.95, %guard.cond.94
  %guard.cond.97 = and i1 %guard.cond.96, %guard.cond.95
  %guard.cond.98 = and i1 %guard.cond.97, %guard.cond.96
  %guard.cond.99 = and i1 %guard.cond.98, %guard.cond.97
  call void (i1, ...) @llvm.experimental.guard(i1 %guard.cond.99, i32 9) [ "deopt"() ]

  %i.i64 = zext i32 %i to i64
  %array.i.ptr = getelementptr inbounds i32, i32* %array, i64 %i.i64
  %array.i = load i32, i32* %array.i.ptr, align 4
  %loop.acc.next = add i32 %loop.acc, %array.i

  %i.next = add nuw i32 %i, 1
  %continue = icmp ult i32 %i.next, %n
  br i1 %continue, label %loop, label %exit

exit:
  %result = phi i32 [ 0, %entry ], [ %loop.acc.next, %loop ]
  ret i32 %result
}
