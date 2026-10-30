; RUN: opt -disable-output -jeandle-java-type-stats -passes="type-check-elimination,print<java-type-analysis>,recover-type-info,print<java-type-analysis>" -jeandle-vm-callback-log=%S/Inputs/tce-recover.cblog %s 2>&1 | FileCheck %s --check-prefix=CROSS
; RUN: opt -disable-output -jeandle-java-type-stats -jeandle-java-type-pass-local -passes="type-check-elimination,print<java-type-analysis>,recover-type-info,print<java-type-analysis>" -jeandle-vm-callback-log=%S/Inputs/tce-recover.cblog %s 2>&1 | FileCheck %s --check-prefix=LOCAL
; RUN: opt -disable-output -jeandle-java-type-stats -jeandle-java-type-pass-local -passes="module(function(type-check-elimination),function(recover-type-info),print<java-type-analysis-summary>)" -jeandle-vm-callback-log=%S/Inputs/tce-recover.cblog %s 2>&1 | FileCheck %s --check-prefix=SUMMARY
; RUN: opt -S -passes="type-check-elimination,recover-type-info" -jeandle-vm-callback-log=%S/Inputs/tce-recover.cblog %s 2>&1 | FileCheck %s --check-prefix=IR
;
; The production pass-local hook clears the Java type cache at the entry of
; every real consumer pass, so the downstream RecoverTypeInfo pass starts with
; an empty cache without an explicit boundary pass. The upstream TCE entries
; are dropped exactly as the manual boundary pass dropped them.

declare i1 @jeandle.check_instanceof(ptr, ptr addrspace(1) nonnull)

define void @tce_recover(ptr addrspace(1) nonnull %obj) #0 gc "hotspotgc" {
entry:
  %c0 = call i1 @jeandle.check_instanceof(ptr inttoptr (i64 9 to ptr), ptr addrspace(1) nonnull %obj)
  %c1 = call i1 @jeandle.check_instanceof(ptr inttoptr (i64 9 to ptr), ptr addrspace(1) nonnull %obj)
  %p0 = getelementptr i8, ptr addrspace(1) %obj, i64 16
  %l0 = load ptr addrspace(1), ptr addrspace(1) %p0, align 8
  %p1 = getelementptr i8, ptr addrspace(1) %obj, i64 24
  %l1 = load ptr addrspace(1), ptr addrspace(1) %p1, align 8
  ret void
}

; CROSS-LABEL: JavaTypeAnalysis for function: tce_recover
; CROSS:         queries: 2
; CROSS:         final type hits: 1
; CROSS:         final type misses: 1
; CROSS:         base insertions: 1
; CROSS-LABEL: JavaTypeAnalysis for function: tce_recover
; CROSS:         queries: 4
; CROSS:         final type hits: 3
; CROSS:         final type misses: 1
; CROSS:         base insertions: 1
; CROSS:         clears: 0

; LOCAL-LABEL: JavaTypeAnalysis for function: tce_recover
; LOCAL:         queries: 2
; LOCAL:         final type hits: 1
; LOCAL:         final type misses: 1
; LOCAL:         base insertions: 1
; LOCAL-LABEL: JavaTypeAnalysis for function: tce_recover
; LOCAL:         queries: 4
; LOCAL:         final type hits: 2
; LOCAL:         final type misses: 2
; LOCAL:         base insertions: 2
; LOCAL:         clears: 1

; The pass-local hook is counted separately from the mutation-driven clears.
; SUMMARY:       pass-local cache clears: 1
; SUMMARY:       consumer invocations: type-check-elimination=1 recover-type-info=1

; The receiver has no java-klass attribute, so RecoverTypeInfo can query the
; loads but cannot emit useful field metadata. Both modes keep the input
; semantics unchanged.
; IR:         %l0 = load ptr addrspace(1), ptr addrspace(1) %p0, align 8
; IR-NEXT:    %p1 = getelementptr
; IR-NEXT:    %l1 = load
; IR-NOT:     !java-klass

attributes #0 = { "java-method"="0" }
!java-method-compilation = !{}
