; A branch that is infeasible only when the affine relation y = x + 1 is
; retained. The preceding guards make the add non-wrapping for signed i32.
source_filename = "relational-witness.c"
define i32 @main(i32 %argc, ptr %argv) {
entry:
  %lower = icmp sge i32 %argc, 0
  br i1 %lower, label %check.upper, label %exit
check.upper:
  %upper = icmp sle i32 %argc, 10
  br i1 %upper, label %compute, label %exit
compute:
  %y = add i32 %argc, 1
  %impossible = icmp sle i32 %y, %argc
  br i1 %impossible, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
