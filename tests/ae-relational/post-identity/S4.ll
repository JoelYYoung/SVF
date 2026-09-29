declare i32 @nondet_i32()
define i32 @main() {
entry:
  %c = call i32 @nondet_i32()
  %b = icmp sgt i32 %c, 0
  br i1 %b, label %L, label %L        ; both successors identical
L:
  ret i32 0
}
