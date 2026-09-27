; ModuleID = 'build/RecursiveModCase5.raw.ll'
source_filename = "tests/ae-relational/RecursiveModCases.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 1, align 4, !dbg !0
@h = global i32 0, align 4, !dbg !5
@other = global i32 2, align 4, !dbg !8

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i32 noundef %0) #0 !dbg !18 {
    #dbg_value(i32 %0, !22, !DIExpression(), !23)
  %2 = icmp ne i32 %0, 0, !dbg !24
  br i1 %2, label %3, label %5, !dbg !24

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !26
  call void @rec(i32 noundef %4), !dbg !27
  br label %6, !dbg !27

5:                                                ; preds = %1
  store i32 99, ptr @g, align 4, !dbg !28
  br label %6

6:                                                ; preds = %5, %3
  ret void, !dbg !29
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !30 {
  %1 = load i32, ptr @g, align 4, !dbg !33
    #dbg_value(i32 %1, !34, !DIExpression(), !35)
  call void @rec(i32 noundef 1), !dbg !36
  %2 = icmp eq i32 %1, 1, !dbg !37
  call void @svf_assert(i1 noundef zeroext %2), !dbg !38
  ret i32 0, !dbg !39
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!10, !11, !12, !13, !14, !15, !16}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!17}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "tests/ae-relational/RecursiveModCases.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-recursive-mod", checksumkind: CSK_MD5, checksum: "c9be1a8bd6cc284bd34601e1b698fc4d")
!4 = !{!0, !5, !8}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "h", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !DIGlobalVariableExpression(var: !9, expr: !DIExpression())
!9 = distinct !DIGlobalVariable(name: "other", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!10 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!11 = !{i32 7, !"Dwarf Version", i32 5}
!12 = !{i32 2, !"Debug Info Version", i32 3}
!13 = !{i32 1, !"wchar_size", i32 4}
!14 = !{i32 8, !"PIC Level", i32 2}
!15 = !{i32 7, !"uwtable", i32 1}
!16 = !{i32 7, !"frame-pointer", i32 1}
!17 = !{!"Homebrew clang version 21.1.8"}
!18 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 21, type: !19, scopeLine: 21, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !21)
!19 = !DISubroutineType(types: !20)
!20 = !{null, !7}
!21 = !{}
!22 = !DILocalVariable(name: "n", arg: 1, scope: !18, file: !3, line: 21, type: !7)
!23 = !DILocation(line: 0, scope: !18)
!24 = !DILocation(line: 21, column: 49, scope: !25)
!25 = distinct !DILexicalBlock(scope: !18, file: !3, line: 21, column: 49)
!26 = !DILocation(line: 21, column: 58, scope: !25)
!27 = !DILocation(line: 21, column: 52, scope: !25)
!28 = !DILocation(line: 21, column: 71, scope: !25)
!29 = !DILocation(line: 21, column: 77, scope: !18)
!30 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 22, type: !31, scopeLine: 22, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !21)
!31 = !DISubroutineType(types: !32)
!32 = !{!7}
!33 = !DILocation(line: 22, column: 26, scope: !30)
!34 = !DILocalVariable(name: "a", scope: !30, file: !3, line: 22, type: !7)
!35 = !DILocation(line: 0, scope: !30)
!36 = !DILocation(line: 22, column: 29, scope: !30)
!37 = !DILocation(line: 22, column: 50, scope: !30)
!38 = !DILocation(line: 22, column: 37, scope: !30)
!39 = !DILocation(line: 22, column: 57, scope: !30)
