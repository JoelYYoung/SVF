; A real invoke gives the recursive call's RetICFGNode two successors.
; Both continuations read a may-written global. Neither query is Safe.
@g = global i32 1
declare void @svf_assert(i1)
declare i32 @__gxx_personality_v0(...)
define i32 @rec(i32 %n) {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %again, label %base
again:
  %m = sub i32 %n, 1
  %r = call i32 @rec(i32 %m)
  ret i32 %r
base:
  store i32 99, ptr @g
  ret i32 0
}
define i32 @main() personality ptr @__gxx_personality_v0 {
entry:
  %r = invoke i32 @rec(i32 1) to label %normal unwind label %exception
normal:
  %x = load i32, ptr @g
  %ok = icmp eq i32 %x, 1
  call void @svf_assert(i1 %ok)
  ret i32 0
exception:
  %lp = landingpad { ptr, i32 } cleanup
  %y = load i32, ptr @g
  %bad = icmp eq i32 %y, 1
  call void @svf_assert(i1 %bad)
  ret i32 0
}
