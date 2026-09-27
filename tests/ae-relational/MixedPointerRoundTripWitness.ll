; Exact valid pointer roundtrip. If choice is zero, q is written and o stays 1.
source_filename = "R07_mixed_roundtrip.c"
target triple = "arm64-apple-macosx26.0.0"
@o = global i32 0, align 4
@q = global i32 0, align 4
declare i32 @nondet_i32()
declare void @svf_assert(i1)
define i32 @main() {
entry:
  store i32 1, ptr @o, align 4
  store i32 2, ptr @q, align 4
  %address = ptrtoint ptr @q to i64
  %roundtrip = inttoptr i64 %address to ptr
  %choice = call i32 @nondet_i32()
  %cond = icmp ne i32 %choice, 0
  %p = select i1 %cond, ptr @o, ptr %roundtrip
  store i32 7, ptr %p, align 4
  %y = load i32, ptr @o, align 4
  %claimed = icmp eq i32 %y, 7
  call void @svf_assert(i1 %claimed)
  ret i32 0
}
