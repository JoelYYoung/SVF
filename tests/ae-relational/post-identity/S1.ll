; ModuleID = 'S1_switch.raw.ll'
source_filename = "S1_switch.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !10 {
  %1 = call i32 @nondet_i32(), !dbg !15
    #dbg_value(i32 %1, !16, !DIExpression(), !17)
  switch i32 %1, label %5 [
    i32 -1, label %2
    i32 1, label %3
    i32 2, label %3
    i32 3, label %4
  ], !dbg !18

2:                                                ; preds = %0
    #dbg_value(i32 10, !19, !DIExpression(), !17)
  br label %6, !dbg !20

3:                                                ; preds = %0, %0
    #dbg_value(i32 20, !19, !DIExpression(), !17)
  br label %6, !dbg !22

4:                                                ; preds = %0
    #dbg_value(i32 30, !19, !DIExpression(), !17)
  br label %6, !dbg !23

5:                                                ; preds = %0
    #dbg_value(i32 40, !19, !DIExpression(), !17)
  br label %6, !dbg !24

6:                                                ; preds = %5, %4, %3, %2
  %.0 = phi i32 [ 40, %5 ], [ 10, %2 ], [ 20, %3 ], [ 30, %4 ], !dbg !25
    #dbg_value(i32 %.0, !19, !DIExpression(), !17)
  ret i32 %.0, !dbg !26
}

declare i32 @nondet_i32() #1

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
!8 = !DIFile(filename: "S1_switch.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/51ab49a9-30ea-46ef-a16d-d36be8c20fba/scratchpad/postid", checksumkind: CSK_MD5, checksum: "68f184e58e5e79eb74377fdcff48bf34")
!9 = !{!"Homebrew clang version 21.1.8"}
!10 = distinct !DISubprogram(name: "main", scope: !8, file: !8, line: 2, type: !11, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !14)
!11 = !DISubroutineType(types: !12)
!12 = !{!13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{}
!15 = !DILocation(line: 3, column: 11, scope: !10)
!16 = !DILocalVariable(name: "x", scope: !10, file: !8, line: 3, type: !13)
!17 = !DILocation(line: 0, scope: !10)
!18 = !DILocation(line: 4, column: 3, scope: !10)
!19 = !DILocalVariable(name: "r", scope: !10, file: !8, line: 3, type: !13)
!20 = !DILocation(line: 5, column: 20, scope: !21)
!21 = distinct !DILexicalBlock(scope: !10, file: !8, line: 4, column: 14)
!22 = !DILocation(line: 6, column: 27, scope: !21)
!23 = !DILocation(line: 7, column: 19, scope: !21)
!24 = !DILocation(line: 8, column: 20, scope: !21)
!25 = !DILocation(line: 0, scope: !21)
!26 = !DILocation(line: 10, column: 3, scope: !10)
