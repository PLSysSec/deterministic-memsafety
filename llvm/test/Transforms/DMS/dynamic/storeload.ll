; RUN: clang -fdms=dynamic-stdout %s -o %t && %t | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,dms-bounds -disable-output < %s 2>&1 > /dev/null

; Since we currently print dynamic counts on a per-module basis, the following
; totals are for this entire file.
; CHECK-LABEL: DMS dynamic counts
; CHECK-NEXT: =====
; CHECK-NEXT: Loads with clean addr: 3
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 1
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 3
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 2
; CHECK-NEXT: Storing a blemished16 ptr to mem: 0
; CHECK-NEXT: Storing a blemished32 ptr to mem: 0
; CHECK-NEXT: Storing a blemished64 ptr to mem: 0
; CHECK-NEXT: Storing a blemishedconst ptr to mem: 0
; CHECK-NEXT: Storing a dirty ptr to mem: 1
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
; CHECK-NEXT: Nonzero constant pointer arithmetic on a clean ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished16 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished32 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished64 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemishedconst ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a dirty ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on an unknown ptr: 0
; CHECK-NEXT: Producing a ptr from inttoptr: 0

define i32 @main() {
  %call = call i32 @dynclean()
  %call2 = call i32 @dyndirty(i32 2)
  %call3 = call i32 @storenull()
  ret i32 0
}

; check that storing and loading pointers works, and that we correctly determine
; that the loaded pointer is dynamically clean
define i32 @dynclean() {
  %ptr = alloca i32, align 4
  %ptrptr = alloca i32*, align 4
  store i32* %ptr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %loadedptr = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr will have DYN_CLEAN status
  %loaded2 = load i32, i32* %loadedptr ; loading from DYN_CLEAN ptr
  ret i32 %loaded2
}

; now store and load a dirty pointer
define i32 @dyndirty(i32 %arg) {
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %dirtyptr = getelementptr i32, i32* %arr, i32 %arg ; dirty because the offset is not a compile-time constant
  %ptrptr = alloca i32*, align 4
  store i32* %dirtyptr, i32** %ptrptr, align 4 ; storing dirty ptr to clean address
  %loadedptr = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr will have DYN_DIRTY status
  %loaded2 = load i32, i32* %loadedptr ; loading from DYN_DIRTY ptr
  ret i32 %loaded2
}

; check that storing NULL works, and counts as storing a clean pointer
; (we won't ever need to bounds-check a pointer we know is NULL, so we can
; safely consider it clean)
define i32 @storenull() {
  %ptrptr = alloca i32*, align 4
  store i32* null, i32** %ptrptr, align 4
  ret i32 0
}
