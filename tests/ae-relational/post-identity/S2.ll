; ModuleID = 'S2_indirect.raw.ll'
source_filename = "S2_indirect.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @f(i32 noundef %0) #0 !dbg !10 {
    #dbg_value(i32 %0, !15, !DIExpression(), !16)
  %2 = add nsw i32 %0, 1, !dbg !17
  ret i32 %2, !dbg !18
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @g(i32 noundef %0) #0 !dbg !19 {
    #dbg_value(i32 %0, !20, !DIExpression(), !21)
  %2 = add nsw i32 %0, 2, !dbg !22
  ret i32 %2, !dbg !23
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !24 {
  %1 = call i32 @nondet_i32(), !dbg !27
  %2 = icmp ne i32 %1, 0, !dbg !27
  %3 = zext i1 %2 to i64, !dbg !27
  %4 = select i1 %2, ptr @f, ptr @g, !dbg !27
    #dbg_value(ptr %4, !28, !DIExpression(), !30)
  %5 = call i32 %4(i32 noundef 1), !dbg !31
    #dbg_value(i32 %5, !32, !DIExpression(), !30)
  %6 = call i32 @f(i32 noundef 2), !dbg !33
    #dbg_value(i32 %6, !34, !DIExpression(), !30)
  %7 = add nsw i32 %5, %6, !dbg !35
  ret i32 %7, !dbg !36
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
!8 = !DIFile(filename: "S2_indirect.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/51ab49a9-30ea-46ef-a16d-d36be8c20fba/scratchpad/postid", checksumkind: CSK_MD5, checksum: "d3888d4447d7b5afbead727b2e82c184")
!9 = !{!"Homebrew clang version 21.1.8"}
!10 = distinct !DISubprogram(name: "f", scope: !8, file: !8, line: 2, type: !11, scopeLine: 2, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !14)
!11 = !DISubroutineType(types: !12)
!12 = !{!13, !13}
!13 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!14 = !{}
!15 = !DILocalVariable(name: "a", arg: 1, scope: !10, file: !8, line: 2, type: !13)
!16 = !DILocation(line: 0, scope: !10)
!17 = !DILocation(line: 2, column: 51, scope: !10)
!18 = !DILocation(line: 2, column: 42, scope: !10)
!19 = distinct !DISubprogram(name: "g", scope: !8, file: !8, line: 3, type: !11, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !14)
!20 = !DILocalVariable(name: "a", arg: 1, scope: !19, file: !8, line: 3, type: !13)
!21 = !DILocation(line: 0, scope: !19)
!22 = !DILocation(line: 3, column: 51, scope: !19)
!23 = !DILocation(line: 3, column: 42, scope: !19)
!24 = distinct !DISubprogram(name: "main", scope: !8, file: !8, line: 4, type: !25, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !14)
!25 = !DISubroutineType(types: !26)
!26 = !{!13}
!27 = !DILocation(line: 5, column: 20, scope: !24)
!28 = !DILocalVariable(name: "fp", scope: !24, file: !8, line: 5, type: !29)
!29 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !11, size: 64)
!30 = !DILocation(line: 0, scope: !24)
!31 = !DILocation(line: 6, column: 11, scope: !24)
!32 = !DILocalVariable(name: "a", scope: !24, file: !8, line: 6, type: !13)
!33 = !DILocation(line: 7, column: 11, scope: !24)
!34 = !DILocalVariable(name: "b", scope: !24, file: !8, line: 7, type: !13)
!35 = !DILocation(line: 8, column: 12, scope: !24)
!36 = !DILocation(line: 8, column: 3, scope: !24)
