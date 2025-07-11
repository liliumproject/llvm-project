; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -passes=slp-vectorizer -mtriple=arm64-apple-ios -S %s | FileCheck %s

; Test case where not vectorizing is more profitable because multiple
; fmul/{fadd,fsub} pairs can be lowered to fma instructions.
define void @slp_not_profitable_with_fast_fmf(ptr %A, ptr %B) {
; CHECK-LABEL: @slp_not_profitable_with_fast_fmf(
; CHECK-NEXT:    [[GEP_B_1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 1
; CHECK-NEXT:    [[A_0:%.*]] = load float, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[B_0:%.*]] = load float, ptr [[B]], align 4
; CHECK-NEXT:    [[GEP_B_2:%.*]] = getelementptr inbounds float, ptr [[B]], i64 2
; CHECK-NEXT:    [[B_2:%.*]] = load float, ptr [[GEP_B_2]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = load <2 x float>, ptr [[GEP_B_1]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <2 x float> poison, float [[B_0]], i32 0
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <2 x float> [[TMP2]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP4:%.*]] = fmul fast <2 x float> [[TMP3]], [[TMP1]]
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <2 x float> [[TMP4]], <2 x float> poison, <2 x i32> <i32 1, i32 0>
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <2 x float> poison, float [[A_0]], i32 0
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <2 x float> [[TMP6]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = fmul fast <2 x float> [[TMP1]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = fsub fast <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP10:%.*]] = fadd fast <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP11:%.*]] = shufflevector <2 x float> [[TMP9]], <2 x float> [[TMP10]], <2 x i32> <i32 0, i32 3>
; CHECK-NEXT:    store <2 x float> [[TMP11]], ptr [[A]], align 4
; CHECK-NEXT:    store float [[B_2]], ptr [[B]], align 4
; CHECK-NEXT:    ret void
;
  %gep.B.1 = getelementptr inbounds float, ptr %B, i64 1
  %A.0 = load float, ptr %A, align 4
  %B.1 = load float, ptr %gep.B.1, align 4
  %mul.0 = fmul fast float %B.1, %A.0
  %B.0 = load float, ptr %B, align 4
  %gep.B.2 = getelementptr inbounds float, ptr %B, i64 2
  %B.2 = load float, ptr %gep.B.2, align 4
  %mul.1 = fmul fast float %B.2, %B.0
  %sub = fsub fast float %mul.0, %mul.1
  %mul.2  = fmul fast float %B.0, %B.1
  %mul.3 = fmul fast float %B.2, %A.0
  %add = fadd fast float %mul.3, %mul.2
  store float %sub, ptr %A, align 4
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  store float %add, ptr %gep.A.1, align 4
  store float %B.2, ptr %B, align 4
  ret void
}

define void @slp_not_profitable_with_reassoc_fmf(ptr %A, ptr %B) {
; CHECK-LABEL: @slp_not_profitable_with_reassoc_fmf(
; CHECK-NEXT:    [[GEP_B_1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 1
; CHECK-NEXT:    [[A_0:%.*]] = load float, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[B_0:%.*]] = load float, ptr [[B]], align 4
; CHECK-NEXT:    [[GEP_B_2:%.*]] = getelementptr inbounds float, ptr [[B]], i64 2
; CHECK-NEXT:    [[B_2:%.*]] = load float, ptr [[GEP_B_2]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = load <2 x float>, ptr [[GEP_B_1]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <2 x float> poison, float [[B_0]], i32 0
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <2 x float> [[TMP2]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP4:%.*]] = fmul <2 x float> [[TMP3]], [[TMP1]]
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <2 x float> [[TMP4]], <2 x float> poison, <2 x i32> <i32 1, i32 0>
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <2 x float> poison, float [[A_0]], i32 0
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <2 x float> [[TMP6]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = fmul reassoc <2 x float> [[TMP1]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = fsub reassoc <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP10:%.*]] = fadd reassoc <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP11:%.*]] = shufflevector <2 x float> [[TMP9]], <2 x float> [[TMP10]], <2 x i32> <i32 0, i32 3>
; CHECK-NEXT:    store <2 x float> [[TMP11]], ptr [[A]], align 4
; CHECK-NEXT:    store float [[B_2]], ptr [[B]], align 4
; CHECK-NEXT:    ret void
;
  %gep.B.1 = getelementptr inbounds float, ptr %B, i64 1
  %A.0 = load float, ptr %A, align 4
  %B.1 = load float, ptr %gep.B.1, align 4
  %mul.0 = fmul reassoc float %B.1, %A.0
  %B.0 = load float, ptr %B, align 4
  %gep.B.2 = getelementptr inbounds float, ptr %B, i64 2
  %B.2 = load float, ptr %gep.B.2, align 4
  %mul.1 = fmul float %B.2, %B.0
  %sub = fsub reassoc float %mul.0, %mul.1
  %mul.2  = fmul float %B.0, %B.1
  %mul.3 = fmul reassoc float %B.2, %A.0
  %add = fadd reassoc float %mul.3, %mul.2
  store float %sub, ptr %A, align 4
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  store float %add, ptr %gep.A.1, align 4
  store float %B.2, ptr %B, align 4
  ret void
}

; FMA cannot be used due to missing fast-math flags, so SLP should kick in.
define void @slp_profitable_missing_fmf_on_fadd_fsub(ptr %A, ptr %B) {
; CHECK-LABEL: @slp_profitable_missing_fmf_on_fadd_fsub(
; CHECK-NEXT:    [[GEP_B_1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 1
; CHECK-NEXT:    [[A_0:%.*]] = load float, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[B_0:%.*]] = load float, ptr [[B]], align 4
; CHECK-NEXT:    [[GEP_B_2:%.*]] = getelementptr inbounds float, ptr [[B]], i64 2
; CHECK-NEXT:    [[B_2:%.*]] = load float, ptr [[GEP_B_2]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = load <2 x float>, ptr [[GEP_B_1]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <2 x float> poison, float [[B_0]], i32 0
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <2 x float> [[TMP2]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP4:%.*]] = fmul fast <2 x float> [[TMP3]], [[TMP1]]
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <2 x float> [[TMP4]], <2 x float> poison, <2 x i32> <i32 1, i32 0>
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <2 x float> poison, float [[A_0]], i32 0
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <2 x float> [[TMP6]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = fmul fast <2 x float> [[TMP1]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = fsub <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP10:%.*]] = fadd <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP11:%.*]] = shufflevector <2 x float> [[TMP9]], <2 x float> [[TMP10]], <2 x i32> <i32 0, i32 3>
; CHECK-NEXT:    store <2 x float> [[TMP11]], ptr [[A]], align 4
; CHECK-NEXT:    store float [[B_2]], ptr [[B]], align 4
; CHECK-NEXT:    ret void
;
  %gep.B.1 = getelementptr inbounds float, ptr %B, i64 1
  %A.0 = load float, ptr %A, align 4
  %B.1 = load float, ptr %gep.B.1, align 4
  %mul.0 = fmul fast float %B.1, %A.0
  %B.0 = load float, ptr %B, align 4
  %gep.B.2 = getelementptr inbounds float, ptr %B, i64 2
  %B.2 = load float, ptr %gep.B.2, align 4
  %mul.1 = fmul fast float %B.2, %B.0
  %sub = fsub float %mul.0, %mul.1
  %mul.2  = fmul fast float %B.0, %B.1
  %mul.3 = fmul fast float %B.2, %A.0
  %add = fadd float %mul.3, %mul.2
  store float %sub, ptr %A, align 4
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  store float %add, ptr %gep.A.1, align 4
  store float %B.2, ptr %B, align 4
  ret void
}

; FMA cannot be used due to missing fast-math flags, so SLP should kick in.
define void @slp_profitable_missing_fmf_on_fmul_fadd_fsub(ptr %A, ptr %B) {
; CHECK-LABEL: @slp_profitable_missing_fmf_on_fmul_fadd_fsub(
; CHECK-NEXT:    [[GEP_B_1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 1
; CHECK-NEXT:    [[A_0:%.*]] = load float, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[B_0:%.*]] = load float, ptr [[B]], align 4
; CHECK-NEXT:    [[GEP_B_2:%.*]] = getelementptr inbounds float, ptr [[B]], i64 2
; CHECK-NEXT:    [[B_2:%.*]] = load float, ptr [[GEP_B_2]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = load <2 x float>, ptr [[GEP_B_1]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <2 x float> poison, float [[B_0]], i32 0
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <2 x float> [[TMP2]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP4:%.*]] = fmul <2 x float> [[TMP3]], [[TMP1]]
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <2 x float> [[TMP4]], <2 x float> poison, <2 x i32> <i32 1, i32 0>
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <2 x float> poison, float [[A_0]], i32 0
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <2 x float> [[TMP6]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = fmul <2 x float> [[TMP1]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = fsub <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP10:%.*]] = fadd <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP11:%.*]] = shufflevector <2 x float> [[TMP9]], <2 x float> [[TMP10]], <2 x i32> <i32 0, i32 3>
; CHECK-NEXT:    store <2 x float> [[TMP11]], ptr [[A]], align 4
; CHECK-NEXT:    store float [[B_2]], ptr [[B]], align 4
; CHECK-NEXT:    ret void
;
  %gep.B.1 = getelementptr inbounds float, ptr %B, i64 1
  %A.0 = load float, ptr %A, align 4
  %B.1 = load float, ptr %gep.B.1, align 4
  %mul.0 = fmul float %B.1, %A.0
  %B.0 = load float, ptr %B, align 4
  %gep.B.2 = getelementptr inbounds float, ptr %B, i64 2
  %B.2 = load float, ptr %gep.B.2, align 4
  %mul.1 = fmul float %B.2, %B.0
  %sub = fsub float %mul.0, %mul.1
  %mul.2  = fmul float %B.0, %B.1
  %mul.3 = fmul float %B.2, %A.0
  %add = fadd float %mul.3, %mul.2
  store float %sub, ptr %A, align 4
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  store float %add, ptr %gep.A.1, align 4
  store float %B.2, ptr %B, align 4
  ret void
}

; FMA cannot be used due to missing fast-math flags, so SLP should kick in.
define void @slp_profitable_missing_fmf_nnans_only(ptr %A, ptr %B) {
; CHECK-LABEL: @slp_profitable_missing_fmf_nnans_only(
; CHECK-NEXT:    [[GEP_B_1:%.*]] = getelementptr inbounds float, ptr [[B:%.*]], i64 1
; CHECK-NEXT:    [[A_0:%.*]] = load float, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[B_0:%.*]] = load float, ptr [[B]], align 4
; CHECK-NEXT:    [[GEP_B_2:%.*]] = getelementptr inbounds float, ptr [[B]], i64 2
; CHECK-NEXT:    [[B_2:%.*]] = load float, ptr [[GEP_B_2]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = load <2 x float>, ptr [[GEP_B_1]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <2 x float> poison, float [[B_0]], i32 0
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <2 x float> [[TMP2]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP4:%.*]] = fmul nnan <2 x float> [[TMP3]], [[TMP1]]
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <2 x float> [[TMP4]], <2 x float> poison, <2 x i32> <i32 1, i32 0>
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <2 x float> poison, float [[A_0]], i32 0
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <2 x float> [[TMP6]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = fmul nnan <2 x float> [[TMP1]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = fsub nnan <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP10:%.*]] = fadd nnan <2 x float> [[TMP8]], [[TMP5]]
; CHECK-NEXT:    [[TMP11:%.*]] = shufflevector <2 x float> [[TMP9]], <2 x float> [[TMP10]], <2 x i32> <i32 0, i32 3>
; CHECK-NEXT:    store <2 x float> [[TMP11]], ptr [[A]], align 4
; CHECK-NEXT:    store float [[B_2]], ptr [[B]], align 4
; CHECK-NEXT:    ret void
;
  %gep.B.1 = getelementptr inbounds float, ptr %B, i64 1
  %A.0 = load float, ptr %A, align 4
  %B.1 = load float, ptr %gep.B.1, align 4
  %mul.0 = fmul nnan float %B.1, %A.0
  %B.0 = load float, ptr %B, align 4
  %gep.B.2 = getelementptr inbounds float, ptr %B, i64 2
  %B.2 = load float, ptr %gep.B.2, align 4
  %mul.1 = fmul nnan float %B.2, %B.0
  %sub = fsub nnan float %mul.0, %mul.1
  %mul.2  = fmul nnan float %B.0, %B.1
  %mul.3 = fmul nnan float %B.2, %A.0
  %add = fadd nnan float %mul.3, %mul.2
  store float %sub, ptr %A, align 4
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  store float %add, ptr %gep.A.1, align 4
  store float %B.2, ptr %B, align 4
  ret void
}

; Test case where not vectorizing is more profitable because multiple
; fmul/{fadd,fsub} pairs can be lowered to fma instructions.
define float @slp_not_profitable_in_loop(float %x, ptr %A) {
; CHECK-LABEL: @slp_not_profitable_in_loop(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[TMP0:%.*]] = load <2 x float>, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[L_3:%.*]] = load float, ptr [[A]], align 4
; CHECK-NEXT:    [[TMP1:%.*]] = insertelement <2 x float> <float poison, float 3.000000e+00>, float [[X:%.*]], i32 0
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i64 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LOOP]] ]
; CHECK-NEXT:    [[RED:%.*]] = phi float [ 0.000000e+00, [[ENTRY]] ], [ [[RED_NEXT:%.*]], [[LOOP]] ]
; CHECK-NEXT:    [[TMP2:%.*]] = fmul fast <2 x float> [[TMP1]], [[TMP0]]
; CHECK-NEXT:    [[MUL16:%.*]] = fmul fast float 3.000000e+00, [[L_3]]
; CHECK-NEXT:    [[ADD13:%.*]] = call fast float @llvm.vector.reduce.fadd.v2f32(float 0.000000e+00, <2 x float> [[TMP2]])
; CHECK-NEXT:    [[RED_NEXT]] = fadd fast float [[ADD13]], [[MUL16]]
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; CHECK-NEXT:    [[CMP:%.*]] = icmp eq i64 [[IV]], 10
; CHECK-NEXT:    br i1 [[CMP]], label [[EXIT:%.*]], label [[LOOP]]
; CHECK:       exit:
; CHECK-NEXT:    ret float [[RED_NEXT]]
;
entry:
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  %l.0 = load float, ptr %gep.A.1, align 4
  %gep.A.2 = getelementptr inbounds float, ptr %A, i64 2
  %l.1 = load float, ptr %gep.A.2, align 4
  %l.2 = load float, ptr %A, align 4
  %l.3 = load float, ptr %A, align 4
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %red = phi float [ 0.000000e+00, %entry ], [ %red.next, %loop ]
  %mul11 = fmul fast float 3.000000e+00, %l.0
  %mul12 = fmul fast float 3.000000e+00, %l.1
  %mul14 = fmul fast float %x, %l.2
  %mul16 = fmul fast float 3.000000e+00, %l.3
  %add = fadd fast float %mul12, %mul11
  %add13 = fadd fast float %add, %mul14
  %red.next = fadd fast float %add13, %mul16
  %iv.next = add nuw nsw i64 %iv, 1
  %cmp = icmp eq i64 %iv, 10
  br i1 %cmp, label %exit, label %loop

exit:
  ret float %red.next
}

define void @slp_profitable(ptr %A, ptr %B, float %0) {
; CHECK-LABEL: @slp_profitable(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[SUB_I1096:%.*]] = fsub fast float 1.000000e+00, [[TMP0:%.*]]
; CHECK-NEXT:    [[TMP1:%.*]] = load <2 x float>, ptr [[A:%.*]], align 4
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <2 x float> poison, float [[TMP0]], i32 0
; CHECK-NEXT:    [[TMP3:%.*]] = shufflevector <2 x float> [[TMP2]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP4:%.*]] = fmul fast <2 x float> [[TMP1]], [[TMP3]]
; CHECK-NEXT:    [[TMP5:%.*]] = shufflevector <2 x float> [[TMP4]], <2 x float> poison, <2 x i32> <i32 1, i32 0>
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <2 x float> poison, float [[SUB_I1096]], i32 0
; CHECK-NEXT:    [[TMP7:%.*]] = shufflevector <2 x float> [[TMP6]], <2 x float> poison, <2 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP8:%.*]] = fmul fast <2 x float> [[TMP1]], [[TMP7]]
; CHECK-NEXT:    [[TMP9:%.*]] = fadd fast <2 x float> [[TMP5]], [[TMP8]]
; CHECK-NEXT:    [[TMP10:%.*]] = fsub fast <2 x float> [[TMP5]], [[TMP8]]
; CHECK-NEXT:    [[TMP11:%.*]] = shufflevector <2 x float> [[TMP9]], <2 x float> [[TMP10]], <2 x i32> <i32 0, i32 3>
; CHECK-NEXT:    store <2 x float> [[TMP11]], ptr [[B:%.*]], align 4
; CHECK-NEXT:    ret void
;
entry:
  %gep.A.1 = getelementptr inbounds float, ptr %A, i64 1
  %sub.i1096 = fsub fast float 1.000000e+00, %0
  %1 = load float, ptr %A, align 4
  %mul.i1100 = fmul fast float %1, %sub.i1096
  %2 = load float, ptr %gep.A.1, align 4
  %mul7.i1101 = fmul fast float %2, %0
  %add.i1102 = fadd fast float %mul7.i1101, %mul.i1100
  %mul14.i = fmul fast float %1, %0
  %3 = fmul fast float %2, %sub.i1096
  %add15.i = fsub fast float %mul14.i, %3
  store float %add.i1102, ptr %B, align 4
  %gep.B.1 = getelementptr inbounds float, ptr %B, i64 1
  store float %add15.i, ptr %gep.B.1, align 4
  ret void
}
