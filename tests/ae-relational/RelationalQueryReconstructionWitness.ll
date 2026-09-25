; The guard and assertion use distinct subtraction temporaries. An Octagon
; cannot store either three-variable assignment `%t = %a - %b` directly, so
; proving the assertion requires reconstructing both SSA definitions into the
; octagonal predicate `%a - %b <= 1`.
source_filename = "relational-query-reconstruction-witness.c"

declare i32 @gt_input_int(i32)
declare void @svf_assert(i32)

define i32 @main() {
entry:
  %a = call i32 @gt_input_int(i32 0)
  %b = call i32 @gt_input_int(i32 1)
  %a.lower = icmp sge i32 %a, -3
  br i1 %a.lower, label %check.a.upper, label %exit

check.a.upper:
  %a.upper = icmp sle i32 %a, 3
  br i1 %a.upper, label %check.b.lower, label %exit

check.b.lower:
  %b.lower = icmp sge i32 %b, -3
  br i1 %b.lower, label %check.b.upper, label %exit

check.b.upper:
  %b.upper = icmp sle i32 %b, 3
  br i1 %b.upper, label %guard, label %exit

guard:
  %guard.diff = sub i32 %a, %b
  %guard.ok = icmp sle i32 %guard.diff, 1
  br i1 %guard.ok, label %check, label %exit

check:
  %query.diff = sub i32 %a, %b
  %query.ok = icmp sle i32 %query.diff, 1
  %argument = select i1 %query.ok, i32 1, i32 0
  call void @svf_assert(i32 %argument)
  br label %exit

exit:
  ret i32 0
}
