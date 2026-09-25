; LLVM phi instructions in one basic block are simultaneous.  SVF may split
; them across sequential ICFG nodes, but the analyzer must still join whole
; predecessor tuples: both arms entail x - y == 1.  The second assertion is a
; paired negative control and must remain May.
source_filename = "relational-phi-tuple-witness.c"

declare i1 @nondet_i1()
declare void @svf_assert(i32)

define i32 @main(i32 %a, i32 %b) {
entry:
  %a.lower = icmp sge i32 %a, -3
  br i1 %a.lower, label %guard.a.upper, label %return

guard.a.upper:
  %a.upper = icmp sle i32 %a, 3
  br i1 %a.upper, label %guard.b.lower, label %return

guard.b.lower:
  %b.lower = icmp sge i32 %b, -3
  br i1 %b.lower, label %guard.b.upper, label %return

guard.b.upper:
  %b.upper = icmp sle i32 %b, 3
  br i1 %b.upper, label %choose, label %return

choose:
  %condition = call i1 @nondet_i1()
  br i1 %condition, label %left, label %right

left:
  %left.x = add nsw i32 %b, 1
  br label %merge

right:
  %right.x = add nsw i32 %a, 1
  br label %merge

merge:
  %x = phi i32 [ %left.x, %left ], [ %right.x, %right ]
  %y = phi i32 [ %b, %left ], [ %a, %right ]
  %difference = sub nsw i32 %x, %y
  %good = icmp eq i32 %difference, 1
  %good.argument = zext i1 %good to i32
  call void @svf_assert(i32 %good.argument)
  %bad = icmp eq i32 %difference, 0
  %bad.argument = zext i1 %bad to i32
  call void @svf_assert(i32 %bad.argument)
  br label %return

return:
  ret i32 0
}
