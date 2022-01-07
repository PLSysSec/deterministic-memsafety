; RUN: clang -fdms=dynamic-stdout %s -o %t && %t | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,dms-bounds -disable-output < %s 2>&1 > /dev/null

; Since we currently print dynamic counts on a per-module basis, the following
; totals are for this entire file.
; CHECK-LABEL: DMS dynamic counts
; CHECK-NEXT: =====
; CHECK-NEXT: Loads with clean addr: 9
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 1
; CHECK-NEXT: Loads with dirty addr: 5
; CHECK-NEXT: Loads with unknown addr: 0
; CHECK-NEXT: Stores with clean addr: 2
; CHECK-NEXT: Stores with blemished16 addr: 0
; CHECK-NEXT: Stores with blemished32 addr: 0
; CHECK-NEXT: Stores with blemished64 addr: 0
; CHECK-NEXT: Stores with blemishedconst addr: 0
; CHECK-NEXT: Stores with dirty addr: 0
; CHECK-NEXT: Stores with unknown addr: 0
; CHECK-NEXT: Storing a clean ptr to mem: 1
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
; CHECK-NEXT: Nonzero constant pointer arithmetic on a clean ptr: 2
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished16 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished32 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemished64 ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a blemishedconst ptr: 0
; CHECK-NEXT: Nonzero constant pointer arithmetic on a dirty ptr: 2
; CHECK-NEXT: Nonzero constant pointer arithmetic on an unknown ptr: 0
; CHECK-NEXT: Producing a ptr from inttoptr: 0

define i32 @main() {
  %call = call i32 @dynclean(i32 2)
  %call2 = call i32 @dyndirty(i32 2)
  ret i32 0
}

; pointer arithmetic on a dynamically clean pointer
define i32 @dynclean(i32 %arg) {
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %ptrptr = alloca i32*, align 4
  store i32* %arr, i32** %ptrptr, align 4 ; storing a clean ptr to clean address
  %loadedptr_a = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_a will have DYN_CLEAN status
  %added0 = getelementptr i32, i32* %loadedptr_a, i32 0 ; added 0 to DYN_CLEAN ptr, should still be DYN_CLEAN
  %loaded1 = load i32, i32* %added0 ; loading from DYN_CLEAN ptr
  %loadedptr_b = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_b will have DYN_CLEAN status
  %added2 = getelementptr i32, i32* %loadedptr_b, i32 2 ; added 8 bytes to DYN_CLEAN ptr, should be DYN_BLEMISHED16
  %loaded2 = load i32, i32* %added2 ; loading from DYN_BLEMISHED16 ptr
  %loadedptr_c = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_c will have DYN_CLEAN status
  %added64 = getelementptr i32, i32* %loadedptr_c, i32 16 ; added 64 bytes to DYN_CLEAN ptr, should be DYN_BLEMISHED_OTHER
  %loaded3 = load i32, i32* %added64 ; loading from DYN_BLEMISHED_OTHER ptr
  %loadedptr_d = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_d will have DYN_CLEAN status
  %addednonconst = getelementptr i32, i32* %loadedptr_d, i32 %arg ; added non-constant offset to DYN_CLEAN ptr, should be DYN_DIRTY
  %loaded4 = load i32, i32* %addednonconst ; loading from DYN_DIRTY ptr
  ret i32 %loaded4
}

; pointer arithmetic on a dynamically dirty pointer
define i32 @dyndirty(i32 %arg) {
  %allocated = alloca [64 x i32], align 4
  %arr = bitcast [64 x i32]* %allocated to i32*
  %dirtyptr = getelementptr i32, i32* %arr, i32 %arg ; dirty because the offset is not a compile-time constant
  %ptrptr = alloca i32*, align 4
  store i32* %dirtyptr, i32** %ptrptr, align 4 ; storing a dirty ptr to clean address
  %loadedptr_a = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_a will have DYN_DIRTY status
  %added0 = getelementptr i32, i32* %loadedptr_a, i32 0 ; added 0 to DYN_DIRTY ptr, should still be DYN_DIRTY
  %loaded1 = load i32, i32* %added0 ; loading from DYN_DIRTY ptr
  %loadedptr_b = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_b will have DYN_DIRTY status
  %added2 = getelementptr i32, i32* %loadedptr_b, i32 2 ; added 8 bytes to DYN_DIRTY ptr, should be DYN_DIRTY
  %loaded2 = load i32, i32* %added2 ; loading from DYN_DIRTY ptr
  %loadedptr_c = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_c will have DYN_DIRTY status
  %added64 = getelementptr i32, i32* %loadedptr_c, i32 16 ; added 64 bytes to DYN_DIRTY ptr, should be DYN_DIRTY
  %loaded3 = load i32, i32* %added64 ; loading from DYN_DIRTY ptr
  %loadedptr_d = load i32*, i32** %ptrptr, align 4 ; loading from clean address. result %loadedptr_d will have DYN_DIRTY status
  %addednonconst = getelementptr i32, i32* %loadedptr_d, i32 %arg ; added non-constant offset to DYN_DIRTY ptr, should be DYN_DIRTY
  %loaded4 = load i32, i32* %addednonconst ; loading from DYN_DIRTY ptr
  ret i32 %loaded4
}
