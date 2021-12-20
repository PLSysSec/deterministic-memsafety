; RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
; (we just test that we can compile this with bounds checks and then run it and
; it exits successfully, no bounds-check violations or other crashes)
; (we do test with several different optimization levels)

; these globals simplified from / inspired by globals appearing when compiling 458.sjeng's eval.c

@globarr = dso_local local_unnamed_addr constant [8 x i32] [i32 26, i32 27, i32 28, i32 9999, i32 40, i32 3, i32 5, i32 1], align 16
@globtuple = dso_local local_unnamed_addr constant <{ [6 x i32], [22 x i32] }> <{ [6 x i32] [i32 0, i32 -5, i32 8, i32 0, i32 1, i32 3], [22 x i32] zeroinitializer }>, align 16
@globdoublearr = dso_local local_unnamed_addr global [5 x [7 x i32]] [[7 x i32] [i32 -5, i32 5, i32 10, i32 15, i32 50, i32 80, i32 200], [7 x i32] zeroinitializer, [7 x i32] [i32 240, i32 350, i32 500, i32 600, i32 700, i32 800, i32 900], [7 x i32] zeroinitializer, [7 x i32] zeroinitializer], align 16
@globdoublezeroarr = dso_local local_unnamed_addr global [144 x [144 x i8]] zeroinitializer, align 16
@externaldoublearr = external dso_local local_unnamed_addr global [144 x [144 x i32]], align 16

define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	%globarr_idx = getelementptr inbounds [8 x i32], [8 x i32]* @globarr, i64 0, i64 5
	%loaded1 = load volatile i32, i32* %globarr_idx, align 4
	%globtuple_idx = getelementptr inbounds [26 x i32], [26 x i32]* bitcast (<{ [6 x i32], [22 x i32] }>* @globtuple to [26 x i32]*), i64 0, i64 17
	%loaded2 = load volatile i32, i32* %globtuple_idx, align 4
	%globdoublearr_idx = getelementptr inbounds [5 x [7 x i32]], [5 x [7 x i32]]* @globdoublearr, i64 0, i64 3, i64 4
	%loaded3 = load volatile i32, i32* %globdoublearr_idx, align 4
	%globdoublezeroarr_idx = getelementptr inbounds [144 x [144 x i8]], [144 x [144 x i8]]* @globdoublezeroarr, i64 0, i64 89, i64 56
	%loaded4 = load volatile i8, i8* %globdoublezeroarr_idx, align 1
	ret i32 0
}
