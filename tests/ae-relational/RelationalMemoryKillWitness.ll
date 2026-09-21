; The second store kills the earlier content = x relation. Since the external
; value is unconstrained, y != x remains feasible and must retain the alarm.
source_filename = "relational-memory-kill-witness.c"
declare i32 @nondet_i32()
define i32 @main(i32 %x, ptr %argv) {
entry:
  %slot = alloca i32, align 4
  store i32 %x, ptr %slot, align 4
  %unknown = call i32 @nondet_i32()
  store i32 %unknown, ptr %slot, align 4
  %y = load i32, ptr %slot, align 4
  %possible = icmp ne i32 %y, %x
  br i1 %possible, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
