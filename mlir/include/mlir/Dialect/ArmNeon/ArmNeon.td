//===-- ArmNeonOps.td - ArmNeon dialect op definitions -----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the basic operations for the ArmNeon dialect.
//
//===----------------------------------------------------------------------===//

#ifndef ARMNEON_OPS
#define ARMNEON_OPS

include "mlir/Dialect/LLVMIR/LLVMOpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// ArmNeon dialect definition
//===----------------------------------------------------------------------===//

def ArmNeon_Dialect : Dialect {
  let name = "arm_neon";
  let cppNamespace = "::mlir::arm_neon";

  // Note: this does not need to depend on LLVMDialect as long as functions in
  // this dialect (such as canonicalization) do not produce entities belonging
  // to the LLVMDialect (ops or types).
}

//===----------------------------------------------------------------------===//
// ArmNeon type definition
//===----------------------------------------------------------------------===//

class NeonVectorOfLength<int length, Type elementType> : ShapedContainerType<
  [elementType], And<[IsVectorOfShape<[length]>, IsFixedVectorOfAnyRankTypePred]>,
  "a vector with length " # length,
  "::mlir::VectorType">;

//===----------------------------------------------------------------------===//
// ArmNeon op definitions
//===----------------------------------------------------------------------===//

// ArmNeon dialect op that corresponds (and is convertible to) an LLVM IR
// intrinsic.
class ArmNeon_IntrOp<string mnemonic, list<int> overloadedResults,
                     list<int> overloadedOperands, int numResults,
                     list<Trait> traits = [], bit requiresAccessGroup = 0,
                     bit requiresAliasAnalysis = 0>
    : LLVM_IntrOpBase</*dialect=*/ArmNeon_Dialect,
                      /*opName=*/"intr." # mnemonic,
                      /*enumName=*/"aarch64_neon_" # !subst(".", "_", mnemonic),
                      /*overloadedResults=*/overloadedResults,
                      /*overloadedOperands=*/overloadedOperands,
                      /*traits=*/traits,
                      /*numResults=*/numResults,
                      /*requiresAccessGroup=*/requiresAccessGroup,
                      /*requiresAliasAnalysis=*/requiresAliasAnalysis>;

// ArmNeon dialect op that corresponds to an LLVM IR intrinsic with one
// overloaded result.
class ArmNeon_OverloadedOneResultIntrOp<string mnemonic,
                                        list<Trait> traits = []>
  : ArmNeon_IntrOp<mnemonic, [0], [], 1, traits>;

// ArmNeon dialect op that corresponds to an LLVM IR intrinsic with one
// overloaded result and overloaded operands list.
class ArmNeon_OverloadedOperandsWithOneResultIntrOp<string mnemonic,
                                                    list<int> overloadedOperands,
                                                    list<Trait> traits = []>
  : ArmNeon_IntrOp<mnemonic, [0], overloadedOperands, 1, traits>;

def SMullOp : ArmNeon_OverloadedOneResultIntrOp<"smull", [
       Pure,
       AllTypesMatch<["a", "b"]>,
       TypesMatchWith<
         "res has same vector shape and element bitwidth scaled by 2 as a",
         "a", "res", "::llvm::cast<VectorType>($_self).scaleElementBitwidth(2)">
    ]> {
  let summary = "smull roundscale op";
  let description = [{
    Signed Multiply Long (vector). This instruction multiplies corresponding
    signed integer values in the lower or upper half of the vectors of the two
    source SIMD&FP registers, places the results in a vector, and writes the
    vector to the destination SIMD&FP register.

    Source:
    https://developer.arm.com/architectures/instruction-sets/simd-isas/neon/intrinsics
  }];

  // Supports either:
  //   (vector<8xi8>, vector<8xi8>) -> (vector<8xi16>)
  //   (vector<4xi16>, vector<4xi16>) -> (vector<4xi32>)
  //   (vector<2xi32>, vector<2xi32>) -> (vector<2xi64>)
  let arguments = (ins VectorOfLengthAndType<[8, 4, 2], [I8, I16, I32]>:$a,
                       VectorOfLengthAndType<[8, 4, 2], [I8, I16, I32]>:$b);
  let results = (outs VectorOfLengthAndType<[8, 4, 2], [I16, I32, I64]>:$res);
  let assemblyFormat =
    "$a `,` $b attr-dict `:` type($a) `to` type($res)";
}

def SdotOp : ArmNeon_OverloadedOperandsWithOneResultIntrOp<"sdot",[1], [
      Pure,
      AllTypesMatch<["b", "c"]>,
      AllTypesMatch<["a", "res"]>,
      TypesMatchWith<"res has the same number of elements as operand b",
                     "b", "res",
                     "VectorType::get({::llvm::cast<VectorType>($_self).getShape()[0] / 4},"
                     "IntegerType::get($_self.getContext(), 32))">]> {
  let summary = "sdot op";
  let description = [{
    Signed integer addition of dot product (vector). This instruction performs
    the following operation on signed integer vectors: res = dot(b, c) + a,
    where vector operands are partitioned into groups of four elements.

    Source:
    https://developer.arm.com/architectures/instruction-sets/simd-isas/neon/intrinsics
  }];
  // Supports either:
  //   (vector<2xi32>, vector<8xi8>, vector<8xi8>) -> vector<2xi32>
  //   (vector<4xi32>, vector<16xi8>, vector<16xi8>) -> vector<4xi32>
  let arguments = (ins VectorOfLengthAndType<[4, 2], [I32]>:$a,
                       VectorOfLengthAndType<[16, 8], [I8]>:$b,
                       VectorOfLengthAndType<[16, 8], [I8]>:$c);
  let results = (outs VectorOfLengthAndType<[4, 2], [I32]>:$res);
  let assemblyFormat =
    "$a `,` $b `,` $c attr-dict `:` type($b) `,` type($c) `to` type($res)";
  }

def SmmlaOp : ArmNeon_OverloadedOperandsWithOneResultIntrOp<"smmla",[1], [
                Pure,
                AllTypesMatch<["src1", "src2"]>,
                AllTypesMatch<["acc", "res"]>,
              ]> {
  let summary = "Matrix-matrix multiply and accumulate op";
  let description = [{
    SMMLA: Signed integer matrix multiply-accumulate.

    Signed 8-bit integer matrix multiply-accumulate. This instruction multiplies
    the 2x8 matrix of signed 8-bit integer values in the first source vector by
    the 8x2 matrix of signed 8-bit integer values in the second source vector.
    The resulting 2x2 32-bit integer matrix product is destructively added to
    the 32-bit integer matrix accumulator in the destination vector. This is
    equivalent to performing an 8-way dot product per destination element.

    Source:
    https://developer.arm.com/architectures/instruction-sets/intrinsics/#f:@navigationhierarchiessimdisa=[Neon]&q=smmla
  }];
  // Supports (vector<16xi8>, vector<16xi8>) -> (vector<4xi32>)
  let arguments = (ins
          NeonVectorOfLength<4, I32>:$acc,
          NeonVectorOfLength<16, I8>:$src1,
          NeonVectorOfLength<16, I8>:$src2
  );
  let results = (outs NeonVectorOfLength<4, I32>:$res);
  let assemblyFormat =
    "$acc `,` $src1 `,` $src2 attr-dict `:` type($src1) `to` type($res)";
}

def UmmlaOp : ArmNeon_OverloadedOperandsWithOneResultIntrOp<"ummla",[1], [
                Pure,
                AllTypesMatch<["src1", "src2"]>,
                AllTypesMatch<["acc", "res"]>,
              ]> {
  let summary = "Unsinged matrix-matrix multiply and accumulate op";
  let description = [{
    UMMLA: Signed integer matrix multiply-accumulate.

    Unsigned 8-bit integer matrix multiply-accumulate. This instruction
    multiplies the 2x8 matrix of unsigned 8-bit integer values in the first
    source vector by the 8x2 matrix of unsigned 8-bit integer values in the
    second source vector. The resulting 2x2 32-bit integer matrix product is
    destructively added to the 32-bit integer matrix accumulator in the
    destination vector. This is equivalent to performing an 8-way dot product
    per destination element.

    Source:
    https://developer.arm.com/architectures/instruction-sets/intrinsics/#f:@navigationhierarchiessimdisa=[Neon]&q=ummla
  }];
  // Supports (vector<16xi8>, vector<16xi8>) -> (vector<4xi32>)
  let arguments = (ins
          NeonVectorOfLength<4, I32>:$acc,
          NeonVectorOfLength<16, I8>:$src1,
          NeonVectorOfLength<16, I8>:$src2
  );
  let results = (outs NeonVectorOfLength<4, I32>:$res);
  let assemblyFormat =
    "$acc `,` $src1 `,` $src2 attr-dict `:` type($src1) `to` type($res)";
}

def UsmmlaOp : ArmNeon_OverloadedOperandsWithOneResultIntrOp<"usmmla",[1], [
                Pure,
                AllTypesMatch<["src1", "src2"]>,
                AllTypesMatch<["acc", "res"]>,
              ]> {
  let summary = "Unsignged and signed matrix-matrix multiply and accumulate op";
  let description = [{
    USMMLA: Signed integer matrix multiply-accumulate.

    Unsigned and signed 8-bit integer matrix multiply-accumulate. This
    instruction multiplies the 2x8 matrix of unsigned 8-bit integer values in
    the first source vector by the 8x2 matrix of signed 8-bit integer values in
    the second source vector. The resulting 2x2 32-bit integer matrix product is
    destructively added to the 32-bit integer matrix accumulator in the
    destination vector. This is equivalent to performing an 8-way dot product
     per destination element.


    Source:
    https://developer.arm.com/architectures/instruction-sets/intrinsics/#f:@navigationhierarchiessimdisa=[Neon]&q=usmmla
  }];
  // Supports (vector<16xi8>, vector<16xi8>) -> (vector<4xi32>)
  let arguments = (ins
          NeonVectorOfLength<4, I32>:$acc,
          NeonVectorOfLength<16, I8>:$src1,
          NeonVectorOfLength<16, I8>:$src2
  );
  let results = (outs NeonVectorOfLength<4, I32>:$res);
  let assemblyFormat =
    "$acc `,` $src1 `,` $src2 attr-dict `:` type($src1) `to` type($res)";
}

def BfmmlaOp : ArmNeon_IntrOp<"bfmmla", [], [], 1, [
                 Pure,
                 AllTypesMatch<["src1", "src2"]>,
                 AllTypesMatch<["acc", "res"]>,
               ]> {
  let summary = "BFloat16 matrix multiply-accumulate to single-precision";
  let description = [{
    BFMMLA: BFloat16 matrix multiply-accumulate to single-precision.

    The operation multiplies the 2x4 BFloat16 matrix in the first source vector
    with the 4x2 BFloat16 matrix in the second source vector, then accumulates
    this intermediate result with the 2x2 Float32 matrix in the accumulator
    vector, yielding the final 2x2 Float32 result.

    Source:
    https://developer.arm.com/architectures/instruction-sets/intrinsics/vbfmmlaq_f32
  }];
  // Supports (vector<8xbf16>, vector<8xbf16>) -> (vector<2xf32>)
  let arguments = (ins
    NeonVectorOfLength<4, F32>:$acc,
    NeonVectorOfLength<8, BF16>:$src1,
    NeonVectorOfLength<8, BF16>:$src2
  );
  let results = (outs NeonVectorOfLength<4, F32>:$res);
  let assemblyFormat =
    "$acc `,` $src1 `,` $src2 attr-dict `:` type($src1) `to` type($res)";
}

class ArmNeon_2dOp<string mnemonic, list<Trait> traits = []>
    : Op</*dialect=*/ArmNeon_Dialect,
         /*opName=*/"2d." # mnemonic,
         /*traits=*/traits>;

def Sdot2dOp : ArmNeon_2dOp<"sdot", [
      Pure,
      AllTypesMatch<["b", "c"]>,
      AllTypesMatch<["a", "res"]>,
      PredOpTrait<
        "operand `a` should be 1-dimensional",
        CPred<"::llvm::cast<VectorType>(getA().getType()).getShape().size() == 1">
      >,
      PredOpTrait<
        "operand `b` should be 2-dimensional",
        CPred<"::llvm::cast<VectorType>(getB().getType()).getShape().size() == 2">
      >,
      PredOpTrait<
        "operand `b` should have 4 columns",
        CPred<"::llvm::cast<VectorType>(getB().getType()).getShape()[1] == 4">
      >,
      PredOpTrait<
        "operand `b` should have as many rows as the size of operand `a`",
        CPred<"::llvm::cast<VectorType>(getB().getType()).getShape()[0] == ::llvm::cast<VectorType>(getA().getType()).getShape()[0]">
      >,
      ]
  > {
  let summary = "sdot op";
  let description = [{
    The two input vectors `b` and `c` have a 2D shape, consisting of either 2
    or 4 rows, each row having length 4. This operation computes the pair-wise
    dot-products of the rows of `b` and `c` and accumulates them with the
    corresponding entry of `a`:

    ```
    res[i] := a[i] + dot_product(b[i, ...], c[i, ...])
    ```

  }];
  // Supports either:
  //   (vector<2xi32>, vector<2x4xi8>, vector<2x4xi8>) -> vector<2xi32>
  //   (vector<4xi32>, vector<4x4xi8>, vector<4x4xi8>) -> vector<4xi32>
  // TODO: how do we express 2D shape requirements here?
  let arguments = (ins VectorOfLengthAndType<[4, 2], [I32]>:$a,
                       VectorOfLengthAndType<[16, 8], [I8]>:$b,
                       VectorOfLengthAndType<[16, 8], [I8]>:$c);
  let results = (outs VectorOfLengthAndType<[4, 2], [I32]>:$res);
  let assemblyFormat =
    "$a `,` $b `,` $c attr-dict `:` type($b) `,` type($c) `to` type($res)";
  let extraClassDeclaration = [{
    static constexpr int kReductionSize = 4;
  }];
}

#endif // ARMNEON_OPS
