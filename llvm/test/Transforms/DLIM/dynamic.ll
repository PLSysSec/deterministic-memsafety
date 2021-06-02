; RUN: opt %s -passes=dynamic-stdout-dlim -o=%t.instrumented.bc && clang %t.instrumented.bc -o %t && %t | FileCheck %s

; Since we currently print dynamic counts on a per-module basis, the following
; totals are for this entire file.
; CHECK-LABEL: DLIM dynamic counts
; CHECK-NEXT: =====
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 1
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 143
; CHECK-NEXT: Storing a clean ptr to mem: 0
; CHECK-NEXT: Storing a blemished16 ptr to mem: 0
; CHECK-NEXT: Storing a blemished32 ptr to mem: 0
; CHECK-NEXT: Storing a blemished64 ptr to mem: 0
; CHECK-NEXT: Storing a blemishedconst ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 0
; CHECK-NEXT: Storing an unknown ptr to mem: 0
; CHECK-NEXT: Passing a clean ptr to a func: 1
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
; CHECK-NEXT: Producing a ptr from inttoptr: 0

define i32 @main() {
  %ptr = alloca i32, align 4
  store volatile i32 7, i32* %ptr, align 4 ; clean store
  %call = call i32 @foo(i32* nonnull %ptr)
  %loaded = load volatile i32, i32* %ptr, align 4 ; clean load
  ret i32 0
}

; Function Attrs: nofree noinline norecurse nounwind
define i32 @foo(i32* %ptr) {
entry:
  br label %for.body

exit:
  ret i32 37

for.body:
  %index = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  store volatile i32 2, i32* %ptr, align 4 ; unknown store. (Technically after the first iteration this could be considered clean, but we currently don't do this optimization.)
  %inc = add nuw nsw i32 %index, 1
  %exitcond = icmp eq i32 %inc, 143
  br i1 %exitcond, label %exit, label %for.body
}
