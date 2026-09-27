; A dead CFG predecessor removes %x from static all-path availability.
; Post's reachable-graph availability includes it at merge. Reconstruction
; must not silently reset its initialization while replay retains it.
@g = global i32 7
declare void @svf_assert(i1)
define i32 @main() {
entry:
  br i1 false, label %dead, label %live
dead:
  br label %merge
live:
  %x = load i32, ptr @g
  %ok = icmp eq i32 %x, 7
  call void @svf_assert(i1 %ok)
  br label %merge
merge:
  ret i32 0
}
