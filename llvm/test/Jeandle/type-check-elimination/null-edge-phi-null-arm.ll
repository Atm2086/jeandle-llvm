; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/null-edge-shapes.cblog %s 2>&1 | FileCheck %s
; XFAIL: *

; Known gap: a phi with a null incoming (phi [null, entry], [obj, loop]) is
; still poisoned by the null arm, so the query on the phi stays unknown even
; though the other incoming value is typed. JavaType has no "known null"
; representation yet, so the phi cannot drop the null arm the way a CFG edge
; can. The plan calls this shape out as the typical Java oop-PHI form, and
; null-arm-type.ll pins the same gap for the non-loop variant.
;
; The test is expected to fail: when the null arm of a phi becomes vacuous,
; the fold below appears and this test must be updated (lit reports an
; unexpected pass then).

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

define i1 @phi_null_entry_typed_loop() gc "hotspotgc" {
entry:
  br label %loop

loop:
  %p = phi ptr addrspace(1) [ null, %entry ], [ %typed, %latch ]
  %r = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %p)
  ret i1 %r

latch:
  %typed = load ptr addrspace(1), ptr addrspace(1) null, !java-klass !0, !noundef !1
  br label %loop
}

; CHECK-LABEL: @phi_null_entry_typed_loop(
; CHECK: loop:
; CHECK-NEXT: ret i1 true

!0 = !{!"noundef"}
!1 = !{!"java-klass", i32 22}
!java-method-compilation = !{}
