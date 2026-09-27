; ModuleID = 'R09.raw.ll'
source_filename = "R09_load_store_branch_refinement.c"
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
  br label %14, !dbg !21

6:                                                ; preds = %3
  store i32 %1, ptr @o, align 4, !dbg !23
  %7 = load i32, ptr @o, align 4, !dbg !24
    #dbg_value(i32 %7, !25, !DIExpression(), !20)
  store i32 100, ptr @o, align 4, !dbg !26
  %8 = icmp slt i32 %7, 5, !dbg !27
  br i1 %8, label %9, label %13, !dbg !27

9:                                                ; preds = %6
  %10 = load i32, ptr @o, align 4, !dbg !29
    #dbg_value(i32 %10, !31, !DIExpression(), !32)
  %11 = icmp eq i32 %10, 100, !dbg !33
  call void @svf_assert(i1 noundef zeroext %11), !dbg !34
  %12 = icmp slt i32 %10, 5, !dbg !35
  call void @svf_assert(i1 noundef zeroext %12), !dbg !36
  br label %13, !dbg !37

13:                                               ; preds = %9, %6
  br label %14, !dbg !38

14:                                               ; preds = %13, %5
  ret i32 0, !dbg !39
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
!3 = !DIFile(filename: "R09_load_store_branch_refinement.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/supervisor-sparse-fixtures/review-probes", checksumkind: CSK_MD5, checksum: "d848ce78dbcf75131b0178a5ec8a40f8")
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
!23 = !DILocation(line: 8, column: 5, scope: !14)
!24 = !DILocation(line: 9, column: 11, scope: !14)
!25 = !DILocalVariable(name: "t", scope: !14, file: !3, line: 9, type: !5)
!26 = !DILocation(line: 10, column: 5, scope: !14)
!27 = !DILocation(line: 11, column: 9, scope: !28)
!28 = distinct !DILexicalBlock(scope: !14, file: !3, line: 11, column: 7)
!29 = !DILocation(line: 12, column: 13, scope: !30)
!30 = distinct !DILexicalBlock(scope: !28, file: !3, line: 11, column: 14)
!31 = !DILocalVariable(name: "y", scope: !30, file: !3, line: 12, type: !5)
!32 = !DILocation(line: 0, scope: !30)
!33 = !DILocation(line: 13, column: 18, scope: !30)
!34 = !DILocation(line: 13, column: 5, scope: !30)
!35 = !DILocation(line: 14, column: 18, scope: !30)
!36 = !DILocation(line: 14, column: 5, scope: !30)
!37 = !DILocation(line: 15, column: 3, scope: !30)
!38 = !DILocation(line: 16, column: 3, scope: !14)
!39 = !DILocation(line: 17, column: 1, scope: !14)
