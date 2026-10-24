; RUN: opt -S -verify-each -passes='copy-specialization' -jeandle-vm-callback-log=%S/Inputs/integer-width.cblog %s | FileCheck %s

; A native-width constant count must be compared with the scalarization
; limit before narrowing. 4294967297 must not wrap to a one-element copy.
@ArrayCopyLoadStoreMaxElem = constant i32 8
@WordSize = constant i64 8
@personality = global ptr null
declare void @jeandle.copy(ptr addrspace(1), i64, ptr addrspace(1), i64, i64, ptr, ptr, i64, i64)
declare void @StubRoutines_arrayof_jlong_disjoint_arraycopy(ptr addrspace(1), ptr addrspace(1), i64)
declare hotspotcc void @SharedRuntime_slow_arraycopy_C(ptr addrspace(1), i32, ptr addrspace(1), i32, i32, ptr)

define void @large_count(ptr addrspace(1) "java-klass"="101" %src, ptr addrspace(1) "java-klass"="101" %dest) #0 personality ptr @personality {
entry:
  invoke void @jeandle.copy(ptr addrspace(1) %src, i64 536870912, ptr addrspace(1) %dest, i64 536870912, i64 4294967297, ptr null, ptr null, i64 4831838209, i64 4831838209) #1 [ "deopt"(i32 0) ] to label %normal unwind label %exception
normal:
  ret void
exception:
  %lp = landingpad i64 cleanup
  resume i64 %lp
}

; CHECK-LABEL: define void @large_count(
; CHECK-NOT: load atomic
; CHECK: getelementptr i8, ptr addrspace(1) {{%.*}}, i64 4294967296
; CHECK: call void @StubRoutines_arrayof_jlong_disjoint_arraycopy({{.*}}i64 4294967297)
; CHECK-NOT: @jeandle.copy

attributes #0 = { "java-method"="0" }
attributes #1 = { "jeandle.copy.kind"="arraycopy" "jeandle.copy.validated" }
!java-method-compilation = !{}
!current-thread = !{!0}
!0 = !{!"r15"}
