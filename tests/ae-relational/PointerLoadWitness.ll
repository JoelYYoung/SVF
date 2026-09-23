; A relation-only memory update must not clear the address facet installed by
; the ordinary pointer store/load transfer.  The non-null edge is infeasible.
source_filename = "pointer-load-witness.c"

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %slot = alloca ptr, align 8
  store ptr null, ptr %slot, align 8
  %value = load ptr, ptr %slot, align 8
  %nonnull = icmp ne ptr %value, null
  br i1 %nonnull, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
