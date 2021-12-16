; RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
; (we just test that we can compile this with bounds checks and then run it and
; it exits successfully, no bounds-check violations or other crashes)
; (we do test with several different optimization levels)

define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
	call i64 @fwrite_to_stderr()
	call i64 @use_isdigit(i32 %argc, i8** %argv)
	ret i32 0
}

; call fwrite to stderr
%struct._IO_FILE = type { i32, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, %struct._IO_marker*, %struct._IO_FILE*, i32, i32, i64, i16, i8, [1 x i8], i8*, i64, %struct._IO_codecvt*, %struct._IO_wide_data*, %struct._IO_FILE*, i8*, i64, i32, [20 x i8] }
%struct._IO_marker = type opaque
%struct._IO_codecvt = type opaque
%struct._IO_wide_data = type opaque
@stderr = external dso_local local_unnamed_addr global %struct._IO_FILE*, align 8
@hi = private unnamed_addr constant [4 x i8] c"hi\0A\00", align 1
declare noundef i64 @fwrite(i8* nocapture noundef, i64 noundef, i64 noundef, %struct._IO_FILE* nocapture noundef) local_unnamed_addr
define i64 @fwrite_to_stderr() noinline {
	%1 = load %struct._IO_FILE*, %struct._IO_FILE** @stderr, align 8
	%ret = call i64 @fwrite(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @hi, i64 0, i64 0), i64 3, i64 1, %struct._IO_FILE* %1)
	ret i64 %ret
}

; this code is (slightly adapted from) the LLVM generated with
; `clang -g -Og -S -emit-llvm` on the following C file:
;
; #include <ctype.h>
; #include <stdio.h>
; int main(int argc, char* argv[]) {
;   char* arg0 = argv[0];
;   int i = 0;
;   while (arg0[i] != '\0') {
;     if (isdigit(arg0[i])) {
; 		  printf("Found a digit\n");
;     }
;     i++;
;   }
;   return 0;
; }
@str = private unnamed_addr constant [14 x i8] c"Found a digit\00", align 1
declare dso_local i16** @__ctype_b_loc() local_unnamed_addr
declare noundef i32 @puts(i8* nocapture noundef readonly) local_unnamed_addr
define i64 @use_isdigit(i32 %argc, i8** nocapture readonly %argv) noinline {
entry:
	%argv0 = load i8*, i8** %argv, align 8
	%argv00 = load i8, i8* %argv0, align 1
	%cond1 = icmp eq i8 %argv00, 0
	br i1 %cond1, label %while.end, label %while.body

while.body:
	%i = phi i64 [ 0, %entry ], [ %nexti, %if.end ]
	%char = phi i8 [ %argv00, %entry ], [ %nextchar, %if.end ]
	%call = call i16** @__ctype_b_loc()
	%ctypeptr = load i16*, i16** %call, align 8
	%char64 = sext i8 %char to i64
	%ctypeptrpluschar = getelementptr inbounds i16, i16* %ctypeptr, i64 %char64
	%somedata = load i16, i16* %ctypeptrpluschar, align 2
	%somedata_bit = and i16 %somedata, 2048
	%cond2 = icmp eq i16 %somedata_bit, 0
	br i1 %cond2, label %if.end, label %if.then

if.then:
	%puts = call i32 @puts(i8* nonnull dereferenceable(1) getelementptr inbounds ([14 x i8], [14 x i8]* @str, i64 0, i64 0))
	br label %if.end

if.end:
	%nexti = add nuw i64 %i, 1
	%nextarrayidx = getelementptr inbounds i8, i8* %argv0, i64 %nexti
	%nextchar = load i8, i8* %nextarrayidx, align 1
	%cond3 = icmp eq i8 %nextchar, 0
	br i1 %cond3, label %while.end, label %while.body

while.end:
	ret i64 0
}
