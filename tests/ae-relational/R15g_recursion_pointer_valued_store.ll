; ModuleID = 'r15g.raw.ll'
source_filename = "r15g.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@a = global i32 1, align 4, !dbg !0
@b = global i32 2, align 4, !dbg !5
@gp = global ptr null, align 8, !dbg !8

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i32 noundef %0) #0 !dbg !19 {
    #dbg_value(i32 %0, !23, !DIExpression(), !24)
  %2 = icmp sgt i32 %0, 0, !dbg !25
  br i1 %2, label %3, label %5, !dbg !25

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !27
  call void @rec(i32 noundef %4), !dbg !28
  br label %6, !dbg !28

5:                                                ; preds = %1
  store ptr @b, ptr @gp, align 8, !dbg !29
  br label %6

6:                                                ; preds = %5, %3
  ret void, !dbg !30
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !31 {
  store ptr @a, ptr @gp, align 8, !dbg !34
  call void @rec(i32 noundef 1), !dbg !35
  %1 = load ptr, ptr @gp, align 8, !dbg !36
  %2 = load i32, ptr %1, align 4, !dbg !37
    #dbg_value(i32 %2, !38, !DIExpression(), !39)
  %3 = icmp eq i32 %2, 1, !dbg !40
  call void @svf_assert(i1 noundef zeroext %3), !dbg !41
  ret i32 0, !dbg !42
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!11, !12, !13, !14, !15, !16, !17}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!18}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "a", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "r15g.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r15", checksumkind: CSK_MD5, checksum: "f46f53ad54d6d5b4fe8832d146b6c59d")
!4 = !{!0, !5, !8}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "b", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !DIGlobalVariableExpression(var: !9, expr: !DIExpression())
!9 = distinct !DIGlobalVariable(name: "gp", scope: !2, file: !3, line: 5, type: !10, isLocal: false, isDefinition: true)
!10 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!11 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!12 = !{i32 7, !"Dwarf Version", i32 5}
!13 = !{i32 2, !"Debug Info Version", i32 3}
!14 = !{i32 1, !"wchar_size", i32 4}
!15 = !{i32 8, !"PIC Level", i32 2}
!16 = !{i32 7, !"uwtable", i32 1}
!17 = !{i32 7, !"frame-pointer", i32 1}
!18 = !{!"Homebrew clang version 21.1.8"}
!19 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 6, type: !20, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !22)
!20 = !DISubroutineType(types: !21)
!21 = !{null, !7}
!22 = !{}
!23 = !DILocalVariable(name: "n", arg: 1, scope: !19, file: !3, line: 6, type: !7)
!24 = !DILocation(line: 0, scope: !19)
!25 = !DILocation(line: 6, column: 51, scope: !26)
!26 = distinct !DILexicalBlock(scope: !19, file: !3, line: 6, column: 49)
!27 = !DILocation(line: 6, column: 62, scope: !26)
!28 = !DILocation(line: 6, column: 56, scope: !26)
!29 = !DILocation(line: 6, column: 76, scope: !26)
!30 = !DILocation(line: 6, column: 82, scope: !19)
!31 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 7, type: !32, scopeLine: 7, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !22)
!32 = !DISubroutineType(types: !33)
!33 = !{!7}
!34 = !DILocation(line: 8, column: 6, scope: !31)
!35 = !DILocation(line: 9, column: 3, scope: !31)
!36 = !DILocation(line: 10, column: 12, scope: !31)
!37 = !DILocation(line: 10, column: 11, scope: !31)
!38 = !DILocalVariable(name: "v", scope: !31, file: !3, line: 10, type: !7)
!39 = !DILocation(line: 0, scope: !31)
!40 = !DILocation(line: 11, column: 16, scope: !31)
!41 = !DILocation(line: 11, column: 3, scope: !31)
!42 = !DILocation(line: 12, column: 3, scope: !31)
