; RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
; RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
; (we just test that we can compile this with bounds checks and then run it and
; it exits successfully, no bounds-check violations or other crashes)
; (we do test with several different optimization levels)

define i32 @main(i32 %argc, i8** nocapture readonly %argv) {
  call i32 @staticboundscheck(i32 2)
  call i32 @staticboundscheck2(i32 2)
  call i32 @dynboundscheck(i32 2)
  call i32 @staticboundscheck_twopreds(i32 2)
  call i32 @dynboundscheck_twopreds(i32 2)
  call i32 @manyblocks_static(i32 2)
  call i32 @manyblocks_dynamic(i32 2)
  call i32 @phi_both_static(i32 2)
  call i32 @phi_both_dynamic(i32 2)
  call i32 @phi_static_dynamic(i32 2)
  call i32 @loop_array(i32 12)
  call i8 @loop_nt_array()
  call i32 @loop_with_const_geps()
  ret i32 0
}

declare noalias i8* @malloc(i64) nounwind
declare void @free(i8*) nounwind

; these two functions are used to create dynamic sizes and offsets
; (that aren't known at compile time, for our analysis)
define i64 @get_mallocsize(i64 %a) noinline {
  %b = mul i64 %a, 2
  ret i64 %b
}
define i64 @get_offset(i64 %a) noinline {
  %b = add i64 %a, 17
  ret i64 %b
}

; here the dereference requiring a static bounds check is in a different block
; from where the dirty pointer was created
define i32 @staticboundscheck(i32 %arg) noinline {
  %cleanptr = call i8* @malloc(i64 234)
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 67
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %b_res = load volatile i32, i32* %dirtyptr_cast
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; same as above, but only the malloc is in the first block; the pointer becomes
; dirty and is dereferenced in a later block
define i32 @staticboundscheck2(i32 %arg) noinline {
  %cleanptr = call i8* @malloc(i64 234)
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 67
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
  %b_res = load volatile i32, i32* %dirtyptr_cast
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; here the dereference requiring a dynamic bounds check is in a different block
; from where the dirty pointer was created
define i32 @dynboundscheck(i32 %arg) noinline {
  %mallocsize = call i64 @get_mallocsize(i64 117)
  %cleanptr = call i8* @malloc(i64 %mallocsize)
  %offset = call i64 @get_offset(i64 50)
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 %offset
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  %a_res = add i32 %arg, 26
  br label %end

b:
  %b_res = load volatile i32, i32* %dirtyptr_cast
  br label %end

end:
  %res = phi i32 [ %a_res, %a ], [ %b_res, %b ]
  ret i32 %res
}

; static bounds check on a pointer which came from either of two predecessors of
; the block. (No PHI.)
define i32 @staticboundscheck_twopreds(i32 %arg) noinline {
  %cleanptr = call i8* @malloc(i64 234)
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 67
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  br label %end

b:
  br label %end

end:
  %loaded = load volatile i32, i32* %dirtyptr_cast
  ret i32 %loaded
}

; dynamic bounds check on a pointer which came from either of two predecessors
; of the block. (No PHI.)
define i32 @dynboundscheck_twopreds(i32 %arg) noinline {
  %mallocsize = call i64 @get_mallocsize(i64 117)
  %cleanptr = call i8* @malloc(i64 %mallocsize)
  %offset = call i64 @get_offset(i64 50)
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 %offset
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
  %cond = icmp sgt i32 %arg, 4
  br i1 %cond, label %a, label %b

a:
  br label %end

b:
  br label %end

end:
  %loaded = load volatile i32, i32* %dirtyptr_cast
  ret i32 %loaded
}

; This tests that you can jump through many blocks, including a loop, and the
; static bounds info remains valid.
define i32 @manyblocks_static(i32 %arg) noinline {
  %cleanptr = call i8* @malloc(i64 234)
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 67
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
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
  %res_e = load i32, i32* %dirtyptr_cast, align 4
  br label %end

end:
  %res = phi i32 [ %res_e, %e ], [ %res_a, %a ]
  ret i32 %res
}

; This tests that you can jump through many blocks, including a loop, and the
; dynamic bounds info remains valid.
define i32 @manyblocks_dynamic(i32 %arg) noinline {
  %mallocsize = call i64 @get_mallocsize(i64 117)
  %cleanptr = call i8* @malloc(i64 %mallocsize)
  %offset = call i64 @get_offset(i64 50)
  %dirtyptr = getelementptr i8, i8* %cleanptr, i64 %offset
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
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
  %res_e = load i32, i32* %dirtyptr_cast, align 4
  br label %end

end:
  %res = phi i32 [ %res_e, %e ], [ %res_a, %a ]
  ret i32 %res
}

; dereference a PHI'd pointer, both possibilities have static bounds
define i32 @phi_both_static(i32 %arg) noinline {
start:
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %castedptr, %start ], [ %newptr_cast, %body ]
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %incd_ptr = getelementptr i32, i32* %loop_ptr, i32 7
  %loaded = load i32, i32* %incd_ptr
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %body, label %end

body:
  %newptr = call i8* @malloc(i64 64)
  %newptr_cast = bitcast i8* %newptr to i32*
  store i32 1, i32* %newptr_cast
  %new_res = add i32 %loop_res, %loaded
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; dereference a PHI'd pointer, both possibilities have dynamic bounds
define i32 @phi_both_dynamic(i32 %arg) noinline {
start:
  %mallocsize = call i64 @get_mallocsize(i64 117)
  %ptr = call i8* @malloc(i64 %mallocsize)
  %offset = call i64 @get_offset(i64 50)
  %dirtyptr = getelementptr i8, i8* %ptr, i64 %offset
  %dirtyptr_cast = bitcast i8* %dirtyptr to i32*
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %dirtyptr_cast, %start ], [ %newdirtyptr_cast, %body ]
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %incd_ptr = getelementptr i32, i32* %loop_ptr, i32 7
  %loaded = load i32, i32* %incd_ptr
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %body, label %end

body:
  %newptr = call i8* @malloc(i64 %mallocsize)
  %newoffset = call i64 @get_offset(i64 80)
  %newdirtyptr = getelementptr i8, i8* %newptr, i64 %offset
  %newdirtyptr_cast = bitcast i8* %newdirtyptr to i32*
  store i32 1, i32* %newdirtyptr_cast
  %new_res = add i32 %loop_res, %loaded
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; dereference a PHI'd pointer, one static and one dynamic bounds
define i32 @phi_static_dynamic(i32 %arg) noinline {
start:
  %ptr = alloca [16 x i32]
  %castedptr = bitcast [16 x i32]* %ptr to i32*
  %cond = icmp ugt i32 %arg, 4
  br i1 %cond, label %end, label %loop

loop:
  %loop_ptr = phi i32* [ %castedptr, %start ], [ %newdirtyptr_cast, %body ]
  %loop_res = phi i32 [ %arg, %start ], [ %new_res, %body ]
  %incd_ptr = getelementptr i32, i32* %loop_ptr, i32 7
  %loaded = load i32, i32* %incd_ptr
  %loop_cond = icmp ugt i32 %arg, 3
  br i1 %cond, label %body, label %end

body:
  %mallocsize = call i64 @get_mallocsize(i64 222)
  %newptr = call i8* @malloc(i64 %mallocsize)
  %newoffset = call i64 @get_offset(i64 80)
  %newdirtyptr = getelementptr i8, i8* %newptr, i64 %newoffset
  %newdirtyptr_cast = bitcast i8* %newdirtyptr to i32*
  store i32 1, i32* %newdirtyptr_cast
  %new_res = add i32 %loop_res, %loaded
  br label %loop

end:
  %res = phi i32 [ %loop_res, %loop ], [ %arg, %start ]
  ret i32 %res
}

; walk an array in a loop, dereferencing every element
define i32 @loop_array(i32 %len) noinline {
start:
  %arr = alloca [16 x i32]
  %arrptr = bitcast [16 x i32]* %arr to i32*
  br label %loop

loop:
  %curptr = phi i32* [ %arrptr, %start ], [ %newptr, %loop ]
  %accumulator = phi i32 [ 0, %start ], [ %newacc, %loop ]
  %i = phi i32 [ 0, %start ], [ %newi, %loop ]
  %loaded = load i32, i32* %curptr
  %newacc = add i32 %accumulator, %loaded
  %newi = add i32 %i, 1
  %newptr = getelementptr i32, i32* %curptr, i32 1
  %cond = icmp ult i32 %newi, %len
  br i1 %cond, label %loop, label %end

end:
  ret i32 %newacc
}

; walk a null-terminated array in a loop, dereferencing every element
@str = private unnamed_addr constant [13 x i8] c"Hello world\0A\00", align 1
define i8 @loop_nt_array() noinline {
start:
  %ptrstorage = alloca i8*, align 8
  store volatile i8* getelementptr ([13 x i8], [13 x i8]* @str, i64 0, i64 0), i8** %ptrstorage, align 8
  %strptr = load volatile i8*, i8** %ptrstorage, align 8
  br label %loop

loop:
  %curptr = phi i8* [ %strptr, %start ], [ %newptr, %loop ]
  %accumulator = phi i8 [ 0, %start ], [ %newacc, %loop ]
  %loaded = load i8, i8* %curptr
  %newacc = xor i8 %accumulator, %loaded
  %newptr = getelementptr i8, i8* %curptr, i32 1
  %cond = icmp eq i8 %loaded, 0
  br i1 %cond, label %end, label %loop

end:
  ret i8 %newacc
}

; this example minimized from a problem originally observed in 464.h264ref
@a = dso_local global i8 0, align 1
@b = dso_local global i8 0, align 1
@c = dso_local global i8 0, align 1
@content = dso_local local_unnamed_addr global [4 x i8*] [i8* @a, i8* @b, i8* @c, i8* null], align 16
define i32 @loop_with_const_geps() noinline {
start:
  %0 = load i8*, i8** getelementptr inbounds ([4 x i8*], [4 x i8*]* @content, i64 0, i64 0), align 16
  %cond1 = icmp eq i8* %0, null
  br i1 %cond1, label %end, label %body

body:
  %i = phi i64 [ 0, %start ], [ %nexti, %body ]
  %curptr = phi i8* [ %0, %start ], [ %nextptr, %body ]
  store volatile i8 122, i8* %curptr, align 1
  %nexti = add nuw nsw i64 %i, 1
  %idx = getelementptr inbounds [4 x i8*], [4 x i8*]* @content, i64 0, i64 %nexti
  %nextptr = load i8*, i8** %idx, align 8
  %cond2 = icmp eq i8* %nextptr, null
  br i1 %cond2, label %end, label %body

end:
  ret i32 0
}
