; ModuleID = 'S6.raw.ll'
source_filename = "S6_switch_refine.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !10 {
  %1 = call i32 @nondet_i32(), !dbg !15
    #dbg_value(i32 %1, !16, !DIExpression(), !17)
  %2 = icmp slt i32 %1, -5, !dbg !18
  br i1 %2, label %5, label %3, !dbg !20

3:                                                ; preds = %0
  %4 = icmp sgt i32 %1, 5, !dbg !21
  br i1 %4, label %5, label %6, !dbg !20

5:                                                ; preds = %3, %0
  br label %16, !dbg !22

6:                                                ; preds = %3
  switch i32 %1, label %13 [
    i32 -1, label %7
    i32 1, label %9
    i32 2, label %9
    i32 3, label %11
  ], !dbg !23

7:                                                ; preds = %6
  %8 = icmp eq i32 %1, -1, !dbg !24
  call void @svf_assert(i1 noundef zeroext %8), !dbg !26
  br label %15, !dbg !27

9:                                                ; preds = %6, %6
  %10 = icmp sge i32 %1, 1, !dbg !28
  call void @svf_assert(i1 noundef zeroext %10), !dbg !29
  br label %15, !dbg !30

11:                                               ; preds = %6
  %12 = icmp eq i32 %1, 3, !dbg !31
  call void @svf_assert(i1 noundef zeroext %12), !dbg !32
  br label %15, !dbg !33

13:                                               ; preds = %6
  %14 = icmp eq i32 %1, -1, !dbg !34
  call void @svf_assert(i1 noundef zeroext %14), !dbg !35
  br label %15, !dbg !36

15:                                               ; preds = %13, %11, %9, %7
  br label %16, !dbg !37

16:                                               ; preds = %15, %5
  ret i32 0, !dbg !38
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}
!llvm.dbg.cu = !{!7}
!llvm.ident = !{!9}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!1 = !{i32 7, !"Dwarf Version", i32 5}
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !{i32 1, !"wchar_size", i32 4}
!4 = !{i32 8, !"PIC Level", i32 2}
!5 = !{i32 7, !"uwtable", i32 1}
!6 = !{i32 7, !"frame-pointer", i32 1}
!7 = distinct !DICompileUnit(language: DW_LANG_C11, file: !8, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!8 = !DIFile(filename: "S6_switch_refine.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/51ab49a9-30ea-46ef-a16d-d36be8c20fba/scratchpad/postid", checksumkind: CSK_MD5, checksum: "20a52ec7e26ee9c4c889f40ddcdcee96")
!9 = !{!"Homebrew clang version 21.1.8"}
!10 = distinct !DISubprogram(name: "main", scope: !8, file: !8, line: 4, type: !11, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !14)
!11 = !DISubroutineType(types: !12)
!12 = !{!13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{}
!15 = !DILocation(line: 5, column: 11, scope: !10)
!16 = !DILocalVariable(name: "x", scope: !10, file: !8, line: 5, type: !13)
!17 = !DILocation(line: 0, scope: !10)
!18 = !DILocation(line: 6, column: 9, scope: !19)
!19 = distinct !DILexicalBlock(scope: !10, file: !8, line: 6, column: 7)
!20 = !DILocation(line: 6, column: 14, scope: !19)
!21 = !DILocation(line: 6, column: 19, scope: !19)
!22 = !DILocation(line: 6, column: 24, scope: !19)
!23 = !DILocation(line: 7, column: 3, scope: !10)
!24 = !DILocation(line: 8, column: 25, scope: !25)
!25 = distinct !DILexicalBlock(scope: !10, file: !8, line: 7, column: 14)
!26 = !DILocation(line: 8, column: 12, scope: !25)
!27 = !DILocation(line: 8, column: 33, scope: !25)
!28 = !DILocation(line: 9, column: 32, scope: !25)
!29 = !DILocation(line: 9, column: 19, scope: !25)
!30 = !DILocation(line: 9, column: 39, scope: !25)
!31 = !DILocation(line: 10, column: 24, scope: !25)
!32 = !DILocation(line: 10, column: 11, scope: !25)
!33 = !DILocation(line: 10, column: 31, scope: !25)
!34 = !DILocation(line: 11, column: 25, scope: !25)
!35 = !DILocation(line: 11, column: 12, scope: !25)
!36 = !DILocation(line: 11, column: 33, scope: !25)
!37 = !DILocation(line: 13, column: 3, scope: !10)
!38 = !DILocation(line: 14, column: 1, scope: !10)
