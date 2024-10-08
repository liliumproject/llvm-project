; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: %if x86-registered-target %{ opt -S -passes=slp-vectorizer -mtriple x86_64-unknown-linux-gnu < %s | FileCheck %s %}
; RUN: %if aarch64-registered-target %{ opt -S -passes=slp-vectorizer -mtriple aarch64-unknown-linux-gnu < %s | FileCheck %s %}

define <4 x double> @test(ptr %p2, double %i1754, double %i1781, double %i1778) {
; CHECK-LABEL: @test(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[I1771:%.*]] = getelementptr inbounds double, ptr [[P2:%.*]], i64 54
; CHECK-NEXT:    [[I1772:%.*]] = load double, ptr [[I1771]], align 8
; CHECK-NEXT:    [[I1795:%.*]] = getelementptr inbounds double, ptr [[P2]], i64 55
; CHECK-NEXT:    [[I1796:%.*]] = load double, ptr [[I1795]], align 8
; CHECK-NEXT:    [[I1797:%.*]] = fmul fast double [[I1796]], [[I1781:%.*]]
; CHECK-NEXT:    [[TMP0:%.*]] = insertelement <4 x double> poison, double [[I1754:%.*]], i32 0
; CHECK-NEXT:    [[TMP1:%.*]] = insertelement <4 x double> [[TMP0]], double [[I1778:%.*]], i32 1
; CHECK-NEXT:    [[TMP2:%.*]] = insertelement <4 x double> [[TMP1]], double [[I1781]], i32 2
; CHECK-NEXT:    [[TMP3:%.*]] = insertelement <4 x double> [[TMP2]], double [[I1772]], i32 3
; CHECK-NEXT:    [[TMP4:%.*]] = shufflevector <4 x double> [[TMP3]], <4 x double> poison, <4 x i32> zeroinitializer
; CHECK-NEXT:    [[TMP5:%.*]] = fmul fast <4 x double> [[TMP3]], [[TMP4]]
; CHECK-NEXT:    [[TMP6:%.*]] = insertelement <4 x double> <double 1.000000e+00, double 1.000000e+00, double 1.000000e+00, double poison>, double [[I1797]], i32 3
; CHECK-NEXT:    [[TMP7:%.*]] = fadd fast <4 x double> [[TMP5]], [[TMP6]]
; CHECK-NEXT:    ret <4 x double> [[TMP7]]
;
entry:
  %i1771 = getelementptr inbounds double, ptr %p2, i64 54
  %i1772 = load double, ptr %i1771, align 8
  %i1773 = fmul fast double %i1772, %i1754
  %i1782 = fmul fast double %i1754, %i1754
  %i1783 = fadd fast double %i1782, 1.000000e+00
  %i1787 = fmul fast double %i1778, %i1754
  %i1788 = fadd fast double %i1787, 1.000000e+00
  %i1792 = fmul fast double %i1754, %i1781
  %i1793 = fadd fast double %i1792, 1.000000e+00
  %i1795 = getelementptr inbounds double, ptr %p2, i64 55
  %i1796 = load double, ptr %i1795, align 8
  %i1797 = fmul fast double %i1796, %i1781
  %i1798 = fadd fast double %i1773, %i1797
  %i1976 = insertelement <4 x double> zeroinitializer, double %i1783, i64 0
  %i1982 = insertelement <4 x double> %i1976, double %i1788, i64 1
  %i1988 = insertelement <4 x double> %i1982, double %i1793, i64 2
  %i1994 = insertelement <4 x double> %i1988, double %i1798, i64 3
  ret <4 x double> %i1994
}
