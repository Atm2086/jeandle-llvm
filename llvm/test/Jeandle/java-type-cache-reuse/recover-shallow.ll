; RUN: opt -S -jeandle-java-type-stats -passes="recover-type-info,print<java-type-analysis>" -jeandle-vm-callback-log=%S/Inputs/recover.cblog %s 2>&1 | FileCheck %s

; Two loads of different fields use the same QueryBase in sibling blocks.

define void @recover_shallow(ptr addrspace(1) "java-klass"="5" %obj, i1 %c) #0 gc "hotspotgc" {
entry:
  br i1 %c, label %left, label %right
left:
  %p0 = getelementptr i8, ptr addrspace(1) %obj, i64 16
  %l0 = load ptr addrspace(1), ptr addrspace(1) %p0, align 8
  br label %exit
right:
  %p1 = getelementptr i8, ptr addrspace(1) %obj, i64 24
  %l1 = load ptr addrspace(1), ptr addrspace(1) %p1, align 8
  br label %exit
exit:
  ret void
}

; CHECK-LABEL: JavaTypeAnalysis for function: recover_shallow
; CHECK:         queries: 2
; CHECK:         path facts hits: 0
; CHECK:         path facts misses: 2
; CHECK:         final type hits: 0
; CHECK:         final type misses: 2
; CHECK:         base insertions: 1
; CHECK:         current base entries: 1
; CHECK:         current path facts entries: 2
; CHECK:         current final type entries: 2
; CHECK:         %l0 = load{{.*}}!java-klass
; CHECK:         %l1 = load{{.*}}!java-klass

attributes #0 = { "java-method"="0" }
!java-method-compilation = !{}
