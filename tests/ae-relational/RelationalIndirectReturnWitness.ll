; Two Andersen-resolved indirect callees implement the same identity summary.
; Their RetPE bindings are alternatives: each must execute on its own return
; edge before the alternatives are joined.  Sequentially strong-assigning the
; shared return node, or dropping the semi-sparse formal-return summary, loses
; the result == argument relation and leaves the bad branch feasible.

source_filename = "relational-indirect-return-witness.c"

define i32 @identity.left(i32 %value) {
entry:
  ret i32 %value
}

define i32 @identity.right(i32 %value) {
entry:
  ret i32 %value
}

define i32 @main(i32 %value, ptr %argv) {
entry:
  %choose.left = icmp sge i32 %value, 0
  %callee = select i1 %choose.left, ptr @identity.left, ptr @identity.right
  %result = call i32 %callee(i32 %value)
  %different = icmp ne i32 %result, %value
  br i1 %different, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
