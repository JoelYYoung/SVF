; The context-insensitive callee is invoked with two different actuals.  The
; analysis may lose per-call equalities, but it must not correlate the two
; return values and prove the concretely feasible r1 != r2 branch unreachable.

source_filename = "shared-callee-witness.c"

define i32 @plus_one(i32 %value) {
entry:
  %result = add i32 %value, 1
  ret i32 %result
}

define i32 @main(i32 %x, ptr %argv) {
entry:
  %lower = icmp sge i32 %x, 0
  br i1 %lower, label %guard.upper, label %exit

guard.upper:
  %upper = icmp sle i32 %x, 10
  br i1 %upper, label %calls, label %exit

calls:
  %second.actual = add i32 %x, 10
  %first = call i32 @plus_one(i32 %x)
  %second = call i32 @plus_one(i32 %second.actual)
  %different = icmp ne i32 %first, %second
  br i1 %different, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
