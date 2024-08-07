; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -mtriple=wasm32-unknown-unknown -S --passes=expand-variadics --expand-variadics-override=optimize < %s | FileCheck %s -check-prefixes=OPT
; RUN: opt -mtriple=wasm32-unknown-unknown -S --passes=expand-variadics --expand-variadics-override=lowering < %s | FileCheck %s -check-prefixes=ABI
; REQUIRES: webassembly-registered-target

declare void @vararg(...)
@vararg_ptr = hidden global ptr @vararg, align 4

%struct.libcS = type { i8, i16, i32, i32, float, double }

define hidden void @fptr_single_i32(i32 noundef %x) {
; OPT-LABEL: @fptr_single_i32(
; OPT-NEXT:  entry:
; OPT-NEXT:    [[TMP0:%.*]] = load volatile ptr, ptr @vararg_ptr, align 4
; OPT-NEXT:    tail call void (...) [[TMP0]](i32 noundef [[X:%.*]])
; OPT-NEXT:    ret void
;
; ABI-LABEL: @fptr_single_i32(
; ABI-NEXT:  entry:
; ABI-NEXT:    [[VARARG_BUFFER:%.*]] = alloca [[FPTR_SINGLE_I32_VARARG:%.*]], align 16
; ABI-NEXT:    [[TMP0:%.*]] = load volatile ptr, ptr @vararg_ptr, align 4
; ABI-NEXT:    call void @llvm.lifetime.start.p0(i64 4, ptr [[VARARG_BUFFER]])
; ABI-NEXT:    [[TMP1:%.*]] = getelementptr inbounds nuw [[FPTR_SINGLE_I32_VARARG]], ptr [[VARARG_BUFFER]], i32 0, i32 0
; ABI-NEXT:    store i32 [[X:%.*]], ptr [[TMP1]], align 4
; ABI-NEXT:    call void [[TMP0]](ptr [[VARARG_BUFFER]])
; ABI-NEXT:    call void @llvm.lifetime.end.p0(i64 4, ptr [[VARARG_BUFFER]])
; ABI-NEXT:    ret void
;
entry:
  %0 = load volatile ptr, ptr @vararg_ptr, align 4
  tail call void (...) %0(i32 noundef %x)
  ret void
}

define hidden void @fptr_libcS(ptr noundef byval(%struct.libcS) align 8 %x) {
; OPT-LABEL: @fptr_libcS(
; OPT-NEXT:  entry:
; OPT-NEXT:    [[TMP0:%.*]] = load volatile ptr, ptr @vararg_ptr, align 4
; OPT-NEXT:    tail call void (...) [[TMP0]](ptr noundef nonnull byval([[STRUCT_LIBCS:%.*]]) align 8 [[X:%.*]])
; OPT-NEXT:    ret void
;
; ABI-LABEL: @fptr_libcS(
; ABI-NEXT:  entry:
; ABI-NEXT:    [[INDIRECTALLOCA:%.*]] = alloca [[STRUCT_LIBCS:%.*]], align 8
; ABI-NEXT:    [[VARARG_BUFFER:%.*]] = alloca [[FPTR_LIBCS_VARARG:%.*]], align 16
; ABI-NEXT:    [[TMP0:%.*]] = load volatile ptr, ptr @vararg_ptr, align 4
; ABI-NEXT:    call void @llvm.memcpy.p0.p0.i64(ptr [[INDIRECTALLOCA]], ptr [[X:%.*]], i64 24, i1 false)
; ABI-NEXT:    call void @llvm.lifetime.start.p0(i64 4, ptr [[VARARG_BUFFER]])
; ABI-NEXT:    [[TMP1:%.*]] = getelementptr inbounds nuw [[FPTR_LIBCS_VARARG]], ptr [[VARARG_BUFFER]], i32 0, i32 0
; ABI-NEXT:    store ptr [[INDIRECTALLOCA]], ptr [[TMP1]], align 4
; ABI-NEXT:    call void [[TMP0]](ptr [[VARARG_BUFFER]])
; ABI-NEXT:    call void @llvm.lifetime.end.p0(i64 4, ptr [[VARARG_BUFFER]])
; ABI-NEXT:    ret void
;
entry:
  %0 = load volatile ptr, ptr @vararg_ptr, align 4
  tail call void (...) %0(ptr noundef nonnull byval(%struct.libcS) align 8 %x)
  ret void
}
