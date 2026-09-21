; Both alternatives satisfy y = x + 1, so select should retain the common
; affine equality rather than joining only the two unary intervals.
source_filename = "relational-select-witness.c"
declare i1 @nondet_i1()
define i32 @main(i32 %x, ptr %argv) {
entry:
  %lower = icmp sge i32 %x, 0
  br i1 %lower, label %guard.upper, label %exit
guard.upper:
  %upper = icmp sle i32 %x, 10
  br i1 %upper, label %compute, label %exit
compute:
  %left = add i32 %x, 1
  %right = add i32 %x, 1
  %condition = call i1 @nondet_i1()
  %y = select i1 %condition, i32 %left, i32 %right
  %impossible = icmp sle i32 %y, %x
  br i1 %impossible, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
