; Schema-only test of the legacy iconv exception: do not execute.
declare i64 @iconv(ptr, ptr, ptr, ptr, ptr)
define i32 @main() {
  %in = alloca ptr
  %out = alloca ptr
  %inlen = alloca i64
  %outlen = alloca i64
  store ptr null, ptr %in
  store ptr null, ptr %out
  store i64 0, ptr %inlen
  store i64 0, ptr %outlen
  %r = call i64 @iconv(ptr null, ptr %in, ptr %inlen, ptr %out, ptr %outlen)
  ret i32 0
}
