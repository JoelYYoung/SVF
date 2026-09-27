; ModuleID = 'build/RecursiveModCase2.raw.ll'
source_filename = "tests/ae-relational/RecursiveModCases.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 1, align 4, !dbg !0
@h = global i32 0, align 4, !dbg !5
@other = global i32 2, align 4, !dbg !8

; Function Attrs: noinline nounwind ssp uwtable(sync)
define ptr @rec(i32 noundef %0) #0 !dbg !18 {
    #dbg_value(i32 %0, !23, !DIExpression(), !24)
  %2 = icmp ne i32 %0, 0, !dbg !25
  br i1 %2, label %3, label %6, !dbg !25

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !26
  %5 = call ptr @rec(i32 noundef %4), !dbg !27
  br label %7, !dbg !25

6:                                                ; preds = %1
  br label %7, !dbg !25

7:                                                ; preds = %6, %3
  %8 = phi ptr [ %5, %3 ], [ @other, %6 ], !dbg !25
  ret ptr %8, !dbg !28
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !29 {
  %1 = call ptr @rec(i32 noundef 1), !dbg !32
    #dbg_value(ptr %1, !33, !DIExpression(), !34)
  %2 = load i32, ptr %1, align 4, !dbg !35
  %3 = icmp eq i32 %2, 1, !dbg !36
  call void @svf_assert(i1 noundef zeroext %3), !dbg !37
  ret i32 0, !dbg !38
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
!3 = !DIFile(filename: "tests/ae-relational/RecursiveModCases.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-recursive-mod", checksumkind: CSK_MD5, checksum: "d5281a7fc31afe84382c9122de707412")
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
!18 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 9, type: !19, scopeLine: 9, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !22)
!19 = !DISubroutineType(types: !20)
!20 = !{!21, !7}
!21 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!22 = !{}
!23 = !DILocalVariable(name: "n", arg: 1, scope: !18, file: !3, line: 9, type: !7)
!24 = !DILocation(line: 0, scope: !18)
!25 = !DILocation(line: 9, column: 52, scope: !18)
!26 = !DILocation(line: 9, column: 62, scope: !18)
!27 = !DILocation(line: 9, column: 56, scope: !18)
!28 = !DILocation(line: 9, column: 45, scope: !18)
!29 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 10, type: !30, scopeLine: 10, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !22)
!30 = !DISubroutineType(types: !31)
!31 = !{!7}
!32 = !DILocation(line: 10, column: 27, scope: !29)
!33 = !DILocalVariable(name: "p", scope: !29, file: !3, line: 10, type: !21)
!34 = !DILocation(line: 0, scope: !29)
!35 = !DILocation(line: 10, column: 46, scope: !29)
!36 = !DILocation(line: 10, column: 49, scope: !29)
!37 = !DILocation(line: 10, column: 35, scope: !29)
!38 = !DILocation(line: 10, column: 56, scope: !29)
