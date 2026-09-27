; ModuleID = 'R10.raw.ll'
source_filename = "R10_stale_branch_refinement_false_safe.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !14 {
  %1 = call i32 @nondet_i32(), !dbg !18
    #dbg_value(i32 %1, !19, !DIExpression(), !20)
  %2 = icmp slt i32 %1, -1000, !dbg !21
  br i1 %2, label %5, label %3, !dbg !21

3:                                                ; preds = %0
  %4 = icmp sgt i32 %1, 1000, !dbg !21
  br i1 %4, label %5, label %6, !dbg !21

5:                                                ; preds = %3, %0
  br label %20, !dbg !21

6:                                                ; preds = %3
  %7 = call i32 @nondet_i32(), !dbg !23
    #dbg_value(i32 %7, !24, !DIExpression(), !20)
  %8 = icmp slt i32 %7, -1000, !dbg !25
  br i1 %8, label %11, label %9, !dbg !25

9:                                                ; preds = %6
  %10 = icmp sgt i32 %7, 1000, !dbg !25
  br i1 %10, label %11, label %12, !dbg !25

11:                                               ; preds = %9, %6
  br label %20, !dbg !25

12:                                               ; preds = %9
  store i32 %1, ptr @o, align 4, !dbg !27
  %13 = load i32, ptr @o, align 4, !dbg !28
    #dbg_value(i32 %13, !29, !DIExpression(), !20)
  store i32 %7, ptr @o, align 4, !dbg !30
  %14 = icmp slt i32 %13, 5, !dbg !31
  br i1 %14, label %15, label %19, !dbg !31

15:                                               ; preds = %12
  %16 = load i32, ptr @o, align 4, !dbg !33
    #dbg_value(i32 %16, !35, !DIExpression(), !36)
  %17 = icmp slt i32 %16, 5, !dbg !37
  call void @svf_assert(i1 noundef zeroext %17), !dbg !38
  %18 = icmp sle i32 %16, 1000, !dbg !39
  call void @svf_assert(i1 noundef zeroext %18), !dbg !40
  br label %19, !dbg !41

19:                                               ; preds = %15, %12
  br label %20, !dbg !42

20:                                               ; preds = %19, %11, %5
  ret i32 0, !dbg !43
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 5, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "R10_stale_branch_refinement_false_safe.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/supervisor-sparse-fixtures/review-probes", checksumkind: CSK_MD5, checksum: "6f2970d89f4f559807cf8983d3a958bc")
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
!14 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !15, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !17)
!15 = !DISubroutineType(types: !16)
!16 = !{!5}
!17 = !{}
!18 = !DILocation(line: 7, column: 3, scope: !14)
!19 = !DILocalVariable(name: "x", scope: !14, file: !3, line: 7, type: !5)
!20 = !DILocation(line: 0, scope: !14)
!21 = !DILocation(line: 7, column: 3, scope: !22)
!22 = distinct !DILexicalBlock(scope: !14, file: !3, line: 7, column: 3)
!23 = !DILocation(line: 8, column: 3, scope: !14)
!24 = !DILocalVariable(name: "v", scope: !14, file: !3, line: 8, type: !5)
!25 = !DILocation(line: 8, column: 3, scope: !26)
!26 = distinct !DILexicalBlock(scope: !14, file: !3, line: 8, column: 3)
!27 = !DILocation(line: 9, column: 5, scope: !14)
!28 = !DILocation(line: 10, column: 11, scope: !14)
!29 = !DILocalVariable(name: "t", scope: !14, file: !3, line: 10, type: !5)
!30 = !DILocation(line: 11, column: 5, scope: !14)
!31 = !DILocation(line: 12, column: 9, scope: !32)
!32 = distinct !DILexicalBlock(scope: !14, file: !3, line: 12, column: 7)
!33 = !DILocation(line: 13, column: 13, scope: !34)
!34 = distinct !DILexicalBlock(scope: !32, file: !3, line: 12, column: 14)
!35 = !DILocalVariable(name: "y", scope: !34, file: !3, line: 13, type: !5)
!36 = !DILocation(line: 0, scope: !34)
!37 = !DILocation(line: 14, column: 18, scope: !34)
!38 = !DILocation(line: 14, column: 5, scope: !34)
!39 = !DILocation(line: 15, column: 18, scope: !34)
!40 = !DILocation(line: 15, column: 5, scope: !34)
!41 = !DILocation(line: 16, column: 3, scope: !34)
!42 = !DILocation(line: 17, column: 3, scope: !14)
!43 = !DILocation(line: 18, column: 1, scope: !14)
