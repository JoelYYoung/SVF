; The first assignment records x-a=2.  The second subtraction must ask the
; relational domain for bound(x-a), rather than requiring interval(x)-interval(a)
; to prove no-wrap before the relational expression is even constructed.
source_filename = "relational-expression-bound-witness.c"

declare i32 @gt_input_int(i32)
declare void @svf_assert(i32)

define i32 @main() {
entry:
  %a = call i32 @gt_input_int(i32 0)
  %lower = icmp sge i32 %a, -3
  br i1 %lower, label %check.upper, label %exit

check.upper:
  %upper = icmp sle i32 %a, 3
  br i1 %upper, label %check, label %exit

check:
  %x = add i32 %a, 2
  %difference = sub i32 %x, %a
  %equal = icmp eq i32 %difference, 2
  %argument = select i1 %equal, i32 1, i32 0
  call void @svf_assert(i32 %argument)
  br label %exit

exit:
  ret i32 0
}
