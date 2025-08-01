//- WebAssembly.td - Describe the WebAssembly Target Machine --*- tablegen -*-//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This is a target description file for the WebAssembly architecture,
/// which is also known as "wasm".
///
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Target-independent interfaces which we are implementing
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// WebAssembly Subtarget features.
//===----------------------------------------------------------------------===//

def FeatureAtomics : SubtargetFeature<"atomics", "HasAtomics", "true",
                                      "Enable Atomics">;

def FeatureBulkMemory :
      SubtargetFeature<"bulk-memory", "HasBulkMemory", "true",
                       "Enable bulk memory operations">;

def FeatureBulkMemoryOpt :
      SubtargetFeature<"bulk-memory-opt", "HasBulkMemoryOpt", "true",
                       "Enable bulk memory optimization operations">;

def FeatureCallIndirectOverlong :
      SubtargetFeature<"call-indirect-overlong", "HasCallIndirectOverlong", "true",
                       "Enable overlong encoding for call_indirect immediates">;

def FeatureExceptionHandling :
      SubtargetFeature<"exception-handling", "HasExceptionHandling", "true",
                       "Enable Wasm exception handling">;

def FeatureExtendedConst :
      SubtargetFeature<"extended-const", "HasExtendedConst", "true",
                       "Enable extended const expressions">;

def FeatureFP16 :
      SubtargetFeature<"fp16", "HasFP16", "true",
                       "Enable FP16 instructions">;

def FeatureGC : SubtargetFeature<"gc", "HasGC", "true", "Enable wasm gc">;

def FeatureMultiMemory :
      SubtargetFeature<"multimemory", "HasMultiMemory", "true",
                       "Enable multiple memories">;

def FeatureMultivalue :
      SubtargetFeature<"multivalue",
                       "HasMultivalue", "true",
                       "Enable multivalue blocks, instructions, and functions">;

def FeatureMutableGlobals :
      SubtargetFeature<"mutable-globals", "HasMutableGlobals", "true",
                       "Enable mutable globals">;

def FeatureNontrappingFPToInt :
      SubtargetFeature<"nontrapping-fptoint",
                       "HasNontrappingFPToInt", "true",
                       "Enable non-trapping float-to-int conversion operators">;

def FeatureReferenceTypes :
      SubtargetFeature<"reference-types", "HasReferenceTypes", "true",
                       "Enable reference types">;

def FeatureRelaxedSIMD :
      SubtargetFeature<"relaxed-simd", "SIMDLevel", "RelaxedSIMD",
                       "Enable relaxed-simd instructions">;

def FeatureSignExt :
      SubtargetFeature<"sign-ext", "HasSignExt", "true",
                       "Enable sign extension operators">;

def FeatureSIMD128 : SubtargetFeature<"simd128", "SIMDLevel", "SIMD128",
                                      "Enable 128-bit SIMD">;

def FeatureTailCall :
      SubtargetFeature<"tail-call", "HasTailCall", "true",
                       "Enable tail call instructions">;

def FeatureWideArithmetic :
      SubtargetFeature<"wide-arithmetic", "HasWideArithmetic", "true",
                       "Enable wide-arithmetic instructions">;

//===----------------------------------------------------------------------===//
// Architectures.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Register File Description
//===----------------------------------------------------------------------===//

include "WebAssemblyRegisterInfo.td"

//===----------------------------------------------------------------------===//
// Instruction Descriptions
//===----------------------------------------------------------------------===//

include "WebAssemblyInstrInfo.td"

def WebAssemblyInstrInfo : InstrInfo;

//===----------------------------------------------------------------------===//
// WebAssembly Processors supported.
//===----------------------------------------------------------------------===//

// Minimal Viable Product.
def : ProcessorModel<"mvp", NoSchedModel, []>;

// Generic processor: latest stable version.
//
// This includes features that have achieved phase 4 of the standards process,
// and that are expected to work for most users in the current time, with
// consideration given to available support in relevant engines and tools, and
// the importance of the features.
def : ProcessorModel<"generic", NoSchedModel,
                      [FeatureBulkMemory, FeatureBulkMemoryOpt,
                       FeatureCallIndirectOverlong, FeatureMultivalue,
                       FeatureMutableGlobals, FeatureNontrappingFPToInt,
                       FeatureReferenceTypes, FeatureSignExt]>;

// Lime1: <https://github.com/WebAssembly/tool-conventions/blob/main/Lime.md#lime1>
def : ProcessorModel<"lime1", NoSchedModel,
                      [FeatureBulkMemoryOpt, FeatureCallIndirectOverlong,
                       FeatureExtendedConst, FeatureMultivalue,
                       FeatureMutableGlobals, FeatureNontrappingFPToInt,
                       FeatureSignExt]>;

// Latest and greatest experimental version of WebAssembly. Bugs included!
def : ProcessorModel<"bleeding-edge", NoSchedModel,
                     [FeatureAtomics, FeatureBulkMemory, FeatureBulkMemoryOpt,
                      FeatureCallIndirectOverlong, FeatureExceptionHandling,
                      FeatureExtendedConst, FeatureFP16, FeatureGC,
                      FeatureMultiMemory, FeatureMultivalue, FeatureMutableGlobals,
                      FeatureNontrappingFPToInt, FeatureRelaxedSIMD,
                      FeatureReferenceTypes, FeatureSIMD128,
                      FeatureSignExt, FeatureTailCall]>;

//===----------------------------------------------------------------------===//
// Target Declaration
//===----------------------------------------------------------------------===//

def WebAssemblyAsmParser : AsmParser {
  // The physical register names are not in the binary format or asm text
  let ShouldEmitMatchRegisterName = 0;
}

def WebAssemblyAsmWriter : AsmWriter {
  string AsmWriterClassName  = "InstPrinter";
  int PassSubtarget = 0;
  int Variant = 0;
  bit isMCAsmWriter = 1;
}

def WebAssembly : Target {
  let InstructionSet = WebAssemblyInstrInfo;
  let AssemblyParsers  = [WebAssemblyAsmParser];
  let AssemblyWriters = [WebAssemblyAsmWriter];
}
