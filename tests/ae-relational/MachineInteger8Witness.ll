; Three i8 boundary obligations.  The first two bad blocks are concretely
; reachable and must remain May; the zero-extension bad block is infeasible.
source_filename = "machine-integer-8-witness.c"

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %x = trunc i32 %argc to i8
  %is.127 = icmp eq i8 %x, 127
  br i1 %is.127, label %wrap.compute, label %after.wrap

wrap.compute:
  %wrapped = add i8 %x, 1
  %is.minus.128 = icmp eq i8 %wrapped, -128
  br i1 %is.minus.128, label %wrap.bad, label %after.wrap

wrap.bad:
  %wrap.fault = load i32, ptr null, align 4
  br label %after.wrap

after.wrap:
  %nonnegative = icmp sge i32 %argc, 0
  br i1 %nonnegative, label %trunc.upper, label %zext.guard

trunc.upper:
  %at.most.256 = icmp sle i32 %argc, 256
  br i1 %at.most.256, label %trunc.compute, label %zext.guard

trunc.compute:
  %truncated = trunc i32 %argc to i8
  %trunc.nonzero = icmp ne i8 %truncated, 0
  br i1 %trunc.nonzero, label %trunc.bad, label %zext.guard

trunc.bad:
  %trunc.fault = load i32, ptr null, align 4
  br label %zext.guard

zext.guard:
  %negative = icmp slt i8 %x, 0
  br i1 %negative, label %zext.compute, label %exit

zext.compute:
  %extended = zext i8 %x to i16
  %too.low = icmp slt i16 %extended, 128
  br i1 %too.low, label %zext.bad, label %exit

zext.bad:
  %zext.fault = load i32, ptr null, align 4
  br label %exit

exit:
  ret i32 0
}
