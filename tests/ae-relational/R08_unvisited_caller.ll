; ModuleID = 'R08.raw.ll'
source_filename = "R08_unvisited_caller.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @f(i32 noundef %0) #0 !dbg !14 {
    #dbg_value(i32 %0, !18, !DIExpression(), !19)
  store i32 %0, ptr @o, align 4, !dbg !20
  %2 = load i32, ptr @o, align 4, !dbg !21
    #dbg_value(i32 %2, !22, !DIExpression(), !19)
  ret i32 %2, !dbg !23
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @g() #0 !dbg !24 {
  %1 = call i32 @f(i32 noundef 1000), !dbg !27
  ret i32 %1, !dbg !28
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !29 {
  %1 = call i32 @nondet_i32(), !dbg !30
    #dbg_value(i32 %1, !31, !DIExpression(), !32)
  %2 = icmp slt i32 %1, -1000, !dbg !33
  br i1 %2, label %5, label %3, !dbg !33

3:                                                ; preds = %0
  %4 = icmp sgt i32 %1, 1000, !dbg !33
  br i1 %4, label %5, label %6, !dbg !33

5:                                                ; preds = %3, %0
  br label %11, !dbg !33

6:                                                ; preds = %3
  %7 = call i32 @f(i32 noundef %1), !dbg !35
    #dbg_value(i32 %7, !36, !DIExpression(), !32)
  %8 = icmp eq i32 %7, %1, !dbg !37
  call void @svf_assert(i1 noundef zeroext %8), !dbg !38
  %9 = add nsw i32 %1, 1, !dbg !39
  %10 = icmp eq i32 %7, %9, !dbg !40
  call void @svf_assert(i1 noundef zeroext %10), !dbg !41
  br label %11, !dbg !42

11:                                               ; preds = %6, %5
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
!3 = !DIFile(filename: "R08_unvisited_caller.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/supervisor-sparse-fixtures/review-probes", checksumkind: CSK_MD5, checksum: "5c4f4e2bb9a0a990569cd0d6997c1f35")
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
!14 = distinct !DISubprogram(name: "f", scope: !3, file: !3, line: 6, type: !15, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !17)
!15 = !DISubroutineType(types: !16)
!16 = !{!5, !5}
!17 = !{}
!18 = !DILocalVariable(name: "v", arg: 1, scope: !14, file: !3, line: 6, type: !5)
!19 = !DILocation(line: 0, scope: !14)
!20 = !DILocation(line: 7, column: 5, scope: !14)
!21 = !DILocation(line: 8, column: 11, scope: !14)
!22 = !DILocalVariable(name: "t", scope: !14, file: !3, line: 8, type: !5)
!23 = !DILocation(line: 9, column: 3, scope: !14)
!24 = distinct !DISubprogram(name: "g", scope: !3, file: !3, line: 11, type: !25, scopeLine: 11, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!25 = !DISubroutineType(types: !26)
!26 = !{!5}
!27 = !DILocation(line: 11, column: 22, scope: !24)
!28 = !DILocation(line: 11, column: 15, scope: !24)
!29 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 12, type: !25, scopeLine: 12, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !17)
!30 = !DILocation(line: 13, column: 3, scope: !29)
!31 = !DILocalVariable(name: "x", scope: !29, file: !3, line: 13, type: !5)
!32 = !DILocation(line: 0, scope: !29)
!33 = !DILocation(line: 13, column: 3, scope: !34)
!34 = distinct !DILexicalBlock(scope: !29, file: !3, line: 13, column: 3)
!35 = !DILocation(line: 14, column: 11, scope: !29)
!36 = !DILocalVariable(name: "r", scope: !29, file: !3, line: 14, type: !5)
!37 = !DILocation(line: 15, column: 16, scope: !29)
!38 = !DILocation(line: 15, column: 3, scope: !29)
!39 = !DILocation(line: 16, column: 21, scope: !29)
!40 = !DILocation(line: 16, column: 16, scope: !29)
!41 = !DILocation(line: 16, column: 3, scope: !29)
!42 = !DILocation(line: 17, column: 3, scope: !29)
!43 = !DILocation(line: 18, column: 1, scope: !29)
