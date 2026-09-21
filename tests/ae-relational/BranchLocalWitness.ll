; The affine relation is valid only on the left branch.  Joining with an
; unconstrained right-hand value must not leak y = x + 1 into the merge.

source_filename = "branch-local-witness.c"

declare i1 @nondet_i1()
declare i32 @nondet_i32()

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
  %right.value = call i32 @nondet_i32()
  br label %merge

merge:
  %y = phi i32 [ %left.value, %left ], [ %right.value, %right ]
  %possible = icmp sle i32 %y, %x
  br i1 %possible, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
