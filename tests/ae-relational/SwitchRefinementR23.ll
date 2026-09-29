; ModuleID = 'S7.raw.ll'
source_filename = "S7_switch_mem.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !14 {
  %1 = call i32 @nondet_i32(), !dbg !17
  store i32 %1, ptr @g, align 4, !dbg !18
  %2 = load i32, ptr @g, align 4, !dbg !19
  %3 = icmp slt i32 %2, -5, !dbg !21
  br i1 %3, label %7, label %4, !dbg !22

4:                                                ; preds = %0
  %5 = load i32, ptr @g, align 4, !dbg !23
  %6 = icmp sgt i32 %5, 5, !dbg !24
  br i1 %6, label %7, label %8, !dbg !22

7:                                                ; preds = %4, %0
  br label %23, !dbg !25

8:                                                ; preds = %4
  %9 = load i32, ptr @g, align 4, !dbg !26
  switch i32 %9, label %19 [
    i32 -1, label %10
    i32 1, label %13
    i32 2, label %13
    i32 3, label %16
  ], !dbg !27

10:                                               ; preds = %8
  %11 = load i32, ptr @g, align 4, !dbg !28
  %12 = icmp eq i32 %11, -1, !dbg !30
  call void @svf_assert(i1 noundef zeroext %12), !dbg !31
  br label %22, !dbg !32

13:                                               ; preds = %8, %8
  %14 = load i32, ptr @g, align 4, !dbg !33
  %15 = icmp sge i32 %14, 1, !dbg !34
  call void @svf_assert(i1 noundef zeroext %15), !dbg !35
  br label %22, !dbg !36

16:                                               ; preds = %8
  %17 = load i32, ptr @g, align 4, !dbg !37
  %18 = icmp eq i32 %17, 3, !dbg !38
  call void @svf_assert(i1 noundef zeroext %18), !dbg !39
  br label %22, !dbg !40

19:                                               ; preds = %8
  %20 = load i32, ptr @g, align 4, !dbg !41
  %21 = icmp eq i32 %20, -1, !dbg !42
  call void @svf_assert(i1 noundef zeroext %21), !dbg !43
  br label %22, !dbg !44

22:                                               ; preds = %19, %16, %13, %10
  br label %23, !dbg !45

23:                                               ; preds = %22, %7
  ret i32 0, !dbg !46
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 4, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "S7_switch_mem.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/51ab49a9-30ea-46ef-a16d-d36be8c20fba/scratchpad/postid", checksumkind: CSK_MD5, checksum: "f0bda0cb7328038c80f536cc653f8362")
!4 = !{!0}
!5 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!6 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!7 = !{i32 7, !"Dwarf Version", i32 5}
!8 = !{i32 2, !"Debug Info Version", i32 3}
!9 = !{i32 1, !"wchar_size", i32 4}
!10 = !{i32 8, !"PIC Level", i32 2}
!11 = !{i32 7, !"uwtable", i32 1}
!12 = !{i32 7, !"frame-pointer", i32 1}
!13 = !{!"Homebrew clang version 21.1.8"}
!14 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 5, type: !15, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!15 = !DISubroutineType(types: !16)
!16 = !{!5}
!17 = !DILocation(line: 6, column: 7, scope: !14)
!18 = !DILocation(line: 6, column: 5, scope: !14)
!19 = !DILocation(line: 7, column: 7, scope: !20)
!20 = distinct !DILexicalBlock(scope: !14, file: !3, line: 7, column: 7)
!21 = !DILocation(line: 7, column: 9, scope: !20)
!22 = !DILocation(line: 7, column: 14, scope: !20)
!23 = !DILocation(line: 7, column: 17, scope: !20)
!24 = !DILocation(line: 7, column: 19, scope: !20)
!25 = !DILocation(line: 7, column: 24, scope: !20)
!26 = !DILocation(line: 8, column: 11, scope: !14)
!27 = !DILocation(line: 8, column: 3, scope: !14)
!28 = !DILocation(line: 9, column: 23, scope: !29)
!29 = distinct !DILexicalBlock(scope: !14, file: !3, line: 8, column: 14)
!30 = !DILocation(line: 9, column: 25, scope: !29)
!31 = !DILocation(line: 9, column: 12, scope: !29)
!32 = !DILocation(line: 9, column: 33, scope: !29)
!33 = !DILocation(line: 10, column: 30, scope: !29)
!34 = !DILocation(line: 10, column: 32, scope: !29)
!35 = !DILocation(line: 10, column: 19, scope: !29)
!36 = !DILocation(line: 10, column: 39, scope: !29)
!37 = !DILocation(line: 11, column: 22, scope: !29)
!38 = !DILocation(line: 11, column: 24, scope: !29)
!39 = !DILocation(line: 11, column: 11, scope: !29)
!40 = !DILocation(line: 11, column: 31, scope: !29)
!41 = !DILocation(line: 12, column: 23, scope: !29)
!42 = !DILocation(line: 12, column: 25, scope: !29)
!43 = !DILocation(line: 12, column: 12, scope: !29)
!44 = !DILocation(line: 12, column: 33, scope: !29)
!45 = !DILocation(line: 14, column: 3, scope: !14)
!46 = !DILocation(line: 15, column: 1, scope: !14)
