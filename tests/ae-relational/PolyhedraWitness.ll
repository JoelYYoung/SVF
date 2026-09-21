; z = x + y is a three-variable affine equality. Together with z == 15,
; x <= 7, and y <= 7 it is inconsistent, but every two-variable octagonal
; projection remains feasible.
source_filename = "polyhedra-witness.c"
declare i32 @nondet_i32()
define i32 @main(i32 %x, ptr %argv) {
entry:
  %y = call i32 @nondet_i32()
  %x.lower = icmp sge i32 %x, 0
  br i1 %x.lower, label %guard.y.lower, label %exit
guard.y.lower:
  %y.lower = icmp sge i32 %y, 0
  br i1 %y.lower, label %guard.x.upper, label %exit
guard.x.upper:
  %x.upper = icmp sle i32 %x, 10
  br i1 %x.upper, label %guard.y.upper, label %exit
guard.y.upper:
  %y.upper = icmp sle i32 %y, 10
  br i1 %y.upper, label %compute, label %exit
compute:
  %z = add i32 %x, %y
  %z.fixed = icmp eq i32 %z, 15
  br i1 %z.fixed, label %guard.x.seven, label %exit
guard.x.seven:
  %x.seven = icmp sle i32 %x, 7
  br i1 %x.seven, label %guard.y.seven, label %exit
guard.y.seven:
  %y.seven = icmp sle i32 %y, 7
  br i1 %y.seven, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
