; RUN: not opt -disable-output -passes='copy-specialization' %s 2>&1 | FileCheck %s
; RUN: sed 's/; HELPER/declare hotspotcc void @jeandle.clone_payload(ptr addrspace(1), i64, ptr addrspace(1), i64, i64, i1)/' %s | not opt -disable-output -passes='copy-specialization' 2>&1 | FileCheck %s

; A missing or declaration-only helper must receive the same diagnostic in
; assertion-enabled and release builds. A raw copy cannot stand in for a
; collector-specific clone helper.
; CHECK: error: copy-specialization requires a definition of jeandle.clone_payload for payload clones
; CHECK-NOT: Assertion
; CHECK-NOT: PLEASE submit a bug report

; HELPER
declare void @jeandle.copy(ptr addrspace(1), i64, ptr addrspace(1), i64, i64, ptr, ptr, i64, i64)

define void @clone_without_helper(ptr addrspace(1) %src, ptr addrspace(1) %dest) #0 {
  call void @jeandle.copy(ptr addrspace(1) %src, i64 8, ptr addrspace(1) %dest, i64 8, i64 16, ptr null, ptr null, i64 136, i64 136) #1
  ret void
}

attributes #0 = { "java-method"="0" }
attributes #1 = { "jeandle.copy.kind"="clone-instance" }
