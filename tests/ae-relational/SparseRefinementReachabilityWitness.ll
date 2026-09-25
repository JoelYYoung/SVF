; A phi-defined Boolean is correlated with the predecessor branch, but both
; successors of the phi branch are concretely reachable.  A path-insensitive
; auxiliary refinement may retain one predecessor's Boolean value; it must not
; use that stale constraint to prune the other successor.
source_filename = "sparse-refinement-reachability-witness.c"

define i32 @main(i32 %value, ptr %argv) {
entry:
  %choose = icmp eq i32 %value, 0
  br i1 %choose, label %left, label %right

left:
  %left.flag = icmp eq i32 %value, 0
  br label %merge

right:
  %right.flag = icmp eq i32 %value, 0
  br label %merge

merge:
  %flag = phi i1 [ %left.flag, %left ], [ %right.flag, %right ]
  br i1 %flag, label %true.path, label %false.path

true.path:
  %true.load = load i32, ptr null, align 4
  br label %exit

false.path:
  %false.load = load i32, ptr null, align 4
  br label %exit

exit:
  ret i32 0
}
