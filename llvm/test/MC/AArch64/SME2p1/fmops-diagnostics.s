// RUN: not llvm-mc -triple=aarch64 -show-encoding -mattr=+sme-f16f16 2>&1 < %s | FileCheck %s

// --------------------------------------------------------------------------//
// Invalid predicate register

fmops za1.h, p8/m, p5/m, z12.h, z11.h
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid restricted predicate register, expected p0..p7 (without element suffix)
// CHECK-NEXT: fmops za1.h, p8/m, p5/m, z12.h, z11.h
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

fmops za1.h, p5/m, p8/m, z12.h, z11.h
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid restricted predicate register, expected p0..p7 (without element suffix)
// CHECK-NEXT: fmops za1.h, p5/m, p8/m, z12.h, z11.h
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

fmops za1.h, p5.h, p5/m, z12.h, z11.h
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid restricted predicate register, expected p0..p7 (without element suffix)
// CHECK-NEXT: fmops za1.h, p5.h, p5/m, z12.h, z11.h
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

// --------------------------------------------------------------------------//
// Invalid matrix operand

fmops za2.h, p5/m, p5/m, z12.h, z11.h
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid operand for instruction
// CHECK-NEXT: fmops za2.h, p5/m, p5/m, z12.h, z11.h
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:

// --------------------------------------------------------------------------//
// Invalid register suffixes

fmops za1.h, p5/m, p5/m, z12.h, z11.b
// CHECK: [[@LINE-1]]:{{[0-9]+}}: error: invalid element width
// CHECK-NEXT: fmops za1.h, p5/m, p5/m, z12.h, z11.b
// CHECK-NOT: [[@LINE-1]]:{{[0-9]+}}:
