; A relational phi summary must cover every feasible operand.  The assertion
; fails when %a == -3 because %n == 0 and the loop executes zero times.  If the
; entry phi operands are skipped while only the backedge alternatives are met
; into the flow state, the analyzer unsoundly infers %n >= 1 at loop exit.
source_filename = "relational-phi-partial-witness.c"

declare void @svf_assert(i32)

define i32 @main(i32 %a, ptr %argv) {
entry:
  %lower = icmp sge i32 %a, -3
  br i1 %lower, label %guard.upper, label %return

guard.upper:
  %upper = icmp sle i32 %a, 3
  br i1 %upper, label %prepare, label %return

prepare:
  %n = add nsw i32 %a, 3
  br label %loop

loop:
  %x = phi i32 [ 0, %prepare ], [ %x.next, %body ]
  %i = phi i32 [ 0, %prepare ], [ %i.next, %body ]
  %condition = icmp slt i32 %i, %n
  br i1 %condition, label %body, label %exit

body:
  %x.next = add nsw i32 %x, %i
  %i.next = add nsw i32 %i, 1
  br label %loop

exit:
  %check = icmp sge i32 %n, 1
  %argument = zext i1 %check to i32
  call void @svf_assert(i32 %argument)
  br label %return

return:
  ret i32 0
}
