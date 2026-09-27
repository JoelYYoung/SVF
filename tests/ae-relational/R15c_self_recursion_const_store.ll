; ModuleID = 'r15c.raw.ll'
source_filename = "r15c.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i32 noundef %0) #0 !dbg !14 {
    #dbg_value(i32 %0, !18, !DIExpression(), !19)
  %2 = icmp sgt i32 %0, 0, !dbg !20
  br i1 %2, label %3, label %5, !dbg !20

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !22
  call void @rec(i32 noundef %4), !dbg !23
  br label %6, !dbg !23

5:                                                ; preds = %1
  store i32 99, ptr @g, align 4, !dbg !24
  br label %6

6:                                                ; preds = %5, %3
  ret void, !dbg !25
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !26 {
  store i32 1, ptr @g, align 4, !dbg !29
  call void @rec(i32 noundef 1), !dbg !30
  %1 = load i32, ptr @g, align 4, !dbg !31
  %2 = icmp eq i32 %1, 1, !dbg !32
  call void @svf_assert(i1 noundef zeroext %2), !dbg !33
  ret i32 0, !dbg !34
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 3, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "r15c.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r15", checksumkind: CSK_MD5, checksum: "c4fcf113b8a132b539ff084c7a614172")
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
!14 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 4, type: !15, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !17)
!15 = !DISubroutineType(types: !16)
!16 = !{null, !5}
!17 = !{}
!18 = !DILocalVariable(name: "n", arg: 1, scope: !14, file: !3, line: 4, type: !5)
!19 = !DILocation(line: 0, scope: !14)
!20 = !DILocation(line: 4, column: 51, scope: !21)
!21 = distinct !DILexicalBlock(scope: !14, file: !3, line: 4, column: 49)
!22 = !DILocation(line: 4, column: 62, scope: !21)
!23 = !DILocation(line: 4, column: 56, scope: !21)
!24 = !DILocation(line: 4, column: 75, scope: !21)
!25 = !DILocation(line: 4, column: 81, scope: !14)
!26 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 5, type: !27, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!27 = !DISubroutineType(types: !28)
!28 = !{!5}
!29 = !DILocation(line: 6, column: 5, scope: !26)
!30 = !DILocation(line: 7, column: 3, scope: !26)
!31 = !DILocation(line: 8, column: 14, scope: !26)
!32 = !DILocation(line: 8, column: 16, scope: !26)
!33 = !DILocation(line: 8, column: 3, scope: !26)
!34 = !DILocation(line: 9, column: 3, scope: !26)
