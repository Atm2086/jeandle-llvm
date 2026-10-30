; RUN: opt -S -passes="type-check-elimination" -jeandle-vm-callback-log=%S/Inputs/null-edge-shapes.cblog %s 2>&1 | FileCheck %s

; Null-edge shapes that the oracle has to recognize:
;  - a diamond whose null arm is the FIRST successor of the null test,
;  - the same diamond with the null arm as the SECOND successor (the answer
;    must not depend on the successor order),
;  - an unreachable block (it must stay unreachable and must not contribute
;    facts to the merge).
; In every case the null arm is skipped, so the remaining arm's passing
; check folds the probe to true.

declare i1 @jeandle.check_instanceof(ptr addrspace(0), ptr addrspace(1) nonnull)

; The null arm is the first successor of `br i1 %is_null`.
define i1 @diamond_null_arm_first(ptr addrspace(1) %obj) gc "hotspotgc" {
entry:
  %is_null = icmp eq ptr addrspace(1) %obj, null
  br i1 %is_null, label %nullbb, label %nonbb

nullbb:
  br label %merge

nonbb:
  %is_sub = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  br i1 %is_sub, label %merge, label %fail

merge:
  %r = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  ret i1 %r

fail:
  ret i1 false
}

; The null arm is the second successor of the branch: the same shape with the
; null test inverted (icmp ne), so the null path is the false successor. The
; answer must not depend on the successor order.
define i1 @diamond_null_arm_last(ptr addrspace(1) %obj) gc "hotspotgc" {
entry:
  %is_not_null = icmp ne ptr addrspace(1) %obj, null
  br i1 %is_not_null, label %nonbb, label %nullbb

nullbb:
  br label %merge

nonbb:
  %is_sub = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  br i1 %is_sub, label %merge, label %fail

merge:
  %r = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  ret i1 %r

fail:
  ret i1 false
}

; An unreachable block: it has no predecessor, so it must not contribute
; facts to %merge and the live fold must still happen.
define i1 @unreachable_block_is_ignored(ptr addrspace(1) %obj) gc "hotspotgc" {
entry:
  %is_null = icmp eq ptr addrspace(1) %obj, null
  br i1 %is_null, label %nullbb, label %nonbb

nullbb:
  br label %merge

nonbb:
  %is_sub = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  br i1 %is_sub, label %merge, label %fail

merge:
  %r = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  ret i1 %r

fail:
  ret i1 false

dead:
  %dead_probe = call i1 @jeandle.check_instanceof(ptr addrspace(0) inttoptr (i64 22 to ptr addrspace(0)), ptr addrspace(1) nonnull %obj)
  ret i1 %dead_probe
}

; CHECK-LABEL: @diamond_null_arm_first(
; CHECK: merge:
; CHECK-NEXT: ret i1 true
; CHECK-LABEL: @diamond_null_arm_last(
; CHECK: merge:
; CHECK-NEXT: ret i1 true
; CHECK-LABEL: @unreachable_block_is_ignored(
; CHECK: merge:
; CHECK-NEXT: ret i1 true

!java-method-compilation = !{}
