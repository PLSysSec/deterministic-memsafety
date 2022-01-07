; RUN: opt -passes=dms-static -disable-output < %s 2>&1 | FileCheck %s
; RUN: opt -passes=dms-bounds-modulepass,dms-bounds -disable-output < %s 2>&1 > /dev/null

; With trustLLVMStructTypes = true (ie, not paranoid), loading the second
; element of a struct (while the struct pointer itself was clean) is a
; clean load
; CHECK-LABEL: load_from_struct
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @load_from_struct() {
  %ptr = alloca [4 x { i32, i32 }]
  %castedptr = bitcast [4 x { i32, i32 }]* %ptr to { i32, i32 }*
  %newptr = getelementptr { i32, i32 }, { i32, i32 }* %castedptr, i32 0, i32 1
  %res = load i32, i32* %newptr
  ret i32 %res
}

; same result even with a larger struct
; CHECK-LABEL: load_from_larger_struct
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i32 @load_from_larger_struct() {
  %ptr = alloca [4 x { i32, i32, i64, i32*, i32 }]
  %castedptr = bitcast [4 x { i32, i32, i64, i32*, i32 }]* %ptr to { i32, i32, i64, i32*, i32 }*
  %newptr = getelementptr { i32, i32, i64, i32*, i32 }, { i32, i32, i64, i32*, i32 }* %castedptr, i32 0, i32 4
  %res = load i32, i32* %newptr
  ret i32 %res
}

; loading the first element of a struct from a BLEMISHED struct pointer is a
; blemished load
; CHECK-LABEL: load_first_from_blemished_struct
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @load_first_from_blemished_struct() {
  %ptr = alloca [4 x { i8, i8 }]
  %castedptr = bitcast [4 x { i8, i8 }]* %ptr to { i8, i8 }*
  %blemptr = getelementptr { i8, i8 }, { i8, i8 }* %castedptr, i32 1
  %newptr = getelementptr { i8, i8 }, { i8, i8 }* %blemptr, i32 0, i32 0
  %res = load i8, i8* %newptr
  ret i8 %res
}

; but loading the second element of a struct from a BLEMISHED16 struct pointer is
; (in this case) a BLEMISHED32 load
; CHECK-LABEL: load_second_from_blemished_struct
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 1
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @load_second_from_blemished_struct() {
  %ptr = alloca [4 x { i8, i8 }]
  %castedptr = bitcast [4 x { i8, i8 }]* %ptr to { i8, i8 }*
  %blemptr = getelementptr { i8, i8 }, { i8, i8 }* %castedptr, i32 1
  %newptr = getelementptr { i8, i8 }, { i8, i8 }* %blemptr, i32 0, i32 1
  %res = load i8, i8* %newptr
  ret i8 %res
}

; loading the first element of a first-class array is like loading the first
; element of a struct: it's whatever the original struct pointer was
; (in this case CLEAN)
; CHECK-LABEL: load_first_from_first_class_array
; CHECK-NEXT: Loads with clean addr: 1
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @load_first_from_first_class_array() {
  %ptr = alloca [64 x i8]
  %newptr = getelementptr [64 x i8], [64 x i8]* %ptr, i32 0, i32 0
  %res = load i8, i8* %newptr
  ret i8 %res
}

; loading the second element of a first-class array is currently not trusted
; (in this case it results in BLEMISHED16)
; CHECK-LABEL: load_second_from_first_class_array
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @load_second_from_first_class_array() {
  %ptr = alloca [64 x i8]
  %newptr = getelementptr [64 x i8], [64 x i8]* %ptr, i32 0, i32 1
  %res = load i8, i8* %newptr
  ret i8 %res
}

; still blemished when the array is nested inside a struct
; CHECK-LABEL: load_from_nested_array
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 1
; CHECK-NEXT: Loads with blemished32 addr: 0
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @load_from_nested_array() {
  %ptr = alloca [4 x { i8, [4 x i8] }]
  %newptr = getelementptr [4 x { i8, [4 x i8] }], [4 x { i8, [4 x i8] }]* %ptr, i32 0, i32 0, i32 1, i32 1
  %res = load i8, i8* %newptr
  ret i8 %res
}

; here it's actually blem32 because the offset is more than 16 bytes
; CHECK-LABEL: load_from_nested_array_larger_offset
; CHECK-NEXT: Loads with clean addr: 0
; CHECK-NEXT: Loads with blemished16 addr: 0
; CHECK-NEXT: Loads with blemished32 addr: 1
; CHECK-NEXT: Loads with blemished64 addr: 0
; CHECK-NEXT: Loads with blemishedconst addr: 0
; CHECK-NEXT: Loads with dirty addr: 0
; CHECK-NEXT: Loads with unknown addr: 0
define i8 @load_from_nested_array_larger_offset() {
  %ptr = alloca [4 x { i8, [4 x i8], i64 }]
  %newptr = getelementptr [4 x { i8, [4 x i8], i64 }], [4 x { i8, [4 x i8], i64 }]* %ptr, i32 0, i32 1, i32 1, i32 1
  %res = load i8, i8* %newptr
  ret i8 %res
}
