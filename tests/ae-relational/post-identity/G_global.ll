; Frozen identity gate: anonymous blocks, conditional/switch, two calls,
; indirect call, global formation and memory accesses. No intentional UB.
@cell = global i32 7
@arr = global [4 x i32] zeroinitializer
@last = global ptr getelementptr ([4 x i32], ptr @arr, i64 0, i64 4)
define i32 @leaf(i32 %x) {
entry:
  %v = load i32, ptr @cell
  %r = add i32 %x, %v
  ret i32 %r
}
define i32 @main(i32 %argc, ptr %argv) {
  %a = call i32 @leaf(i32 %argc)
  %b = call i32 @leaf(i32 2)
  %c = icmp sgt i32 %argc, 0
  br i1 %c, label %yes, label %no
yes:
  store i32 8, ptr @cell
  br label %join
no:
  store i32 9, ptr @cell
  br label %join
join:
  %f = select i1 %c, ptr @leaf, ptr @leaf
  %d = call i32 %f(i32 %b)
  switch i32 %argc, label %other [i32 1, label %one
                                i32 -1, label %minus]
one:
  ret i32 %a
minus:
  ret i32 %b
other:
  ret i32 %d
}
