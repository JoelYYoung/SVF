; Both incoming values satisfy y = x + 1. A relational phi transfer should
; preserve that common equality after joining the predecessor states.
source_filename = "relational-phi-witness.c"
declare i1 @nondet_i1()
define i32 @main(i32 %x, ptr %argv) {
entry:
  %lower = icmp sge i32 %x, 0
  br i1 %lower, label %guard.upper, label %exit
guard.upper:
  %upper = icmp sle i32 %x, 10
  br i1 %upper, label %choose, label %exit
choose:
  %condition = call i1 @nondet_i1()
  br i1 %condition, label %left, label %right
left:
  %left.value = add i32 %x, 1
  br label %merge
right:
  %right.value = add i32 %x, 1
  br label %merge
merge:
  %y = phi i32 [ %left.value, %left ], [ %right.value, %right ]
  %impossible = icmp sle i32 %y, %x
  br i1 %impossible, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
