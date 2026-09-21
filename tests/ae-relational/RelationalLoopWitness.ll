; Exercises a loop head, widening, and a relation created in the body. The
; guard proves the body add cannot wrap; y <= x is therefore infeasible.

source_filename = "relational-loop-witness.c"

define i32 @main() {
entry:
  br label %head

head:
  %x = phi i32 [ 0, %entry ], [ %next, %latch ]
  %continue = icmp slt i32 %x, 10
  br i1 %continue, label %body, label %exit

body:
  %y = add i32 %x, 1
  %impossible = icmp sle i32 %y, %x
  br i1 %impossible, label %bad, label %latch

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

latch:
  %next = add i32 %x, 1
  br label %head

exit:
  ret i32 0
}
