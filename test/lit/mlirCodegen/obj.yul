// RUN: solc --yul --mlir-action=print-init %s | FileCheck %s
// RUN: solc --yul --mlir-action=print-init --mmlir --mlir-print-debuginfo %s | FileCheck --check-prefix=DBG %s

object "Foo" {
  code {}
  object "Foo_deployed" {
    code {}
  }
}
// NOTE: Assertions have been autogenerated by test/updFileCheckTest.py
// CHECK: module {
// CHECK-NEXT:   sol.object @Foo {
// CHECK-NEXT:     sol.object @Foo_deployed {
// CHECK-NEXT:     }
// CHECK-NEXT:   }
// CHECK-NEXT: }
// DBG: module {
// DBG-NEXT:   sol.object @Foo {
// DBG-NEXT:     sol.object @Foo_deployed {
// DBG-NEXT:     } loc(#loc)
// DBG-NEXT:   } loc(#loc)
// DBG-NEXT: } loc(#loc)
// DBG-NEXT: #loc = loc(unknown)
