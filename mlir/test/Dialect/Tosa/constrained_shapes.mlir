// RUN: mlir-opt %s | mlir-opt | FileCheck %s
// RUN: mlir-opt %s --mlir-print-op-generic | mlir-opt | FileCheck %s


// -----
// Uses argmax as canonical example to validate constrained TOSA tensor shapes.
// CHECK-LABEL: argmax
func.func @test_argmax(%arg0: tensor<?xf32>) -> tensor<i32> {
  %0 = "tosa.argmax"(%arg0) {axis = 0 : i32} : (tensor<?xf32>) -> tensor<i32>
  return %0 : tensor<i32>
}
