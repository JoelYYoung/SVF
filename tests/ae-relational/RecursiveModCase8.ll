; ModuleID = '/tmp/RecursiveModCase8.raw.ll'
source_filename = "tests/ae-relational/RecursiveModCases.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 1, align 4, !dbg !0
@h = global i32 0, align 4, !dbg !5
@other = global i32 2, align 4, !dbg !8
@gp = global ptr @g, align 8, !dbg !10

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i32 noundef %0) #0 !dbg !21 {
    #dbg_value(i32 %0, !25, !DIExpression(), !26)
  %2 = icmp ne i32 %0, 0, !dbg !27
  br i1 %2, label %3, label %5, !dbg !27

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !29
  call void @rec(i32 noundef %4), !dbg !30
  br label %6, !dbg !30

5:                                                ; preds = %1
  call void @external_write(), !dbg !31
  br label %6

6:                                                ; preds = %5, %3
  ret void, !dbg !32
}

declare void @external_write() #1

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !33 {
  call void @rec(i32 noundef 1), !dbg !36
  %1 = load ptr, ptr @gp, align 8, !dbg !37
  %2 = load i32, ptr %1, align 4, !dbg !38
  %3 = icmp eq i32 %2, 1, !dbg !39
  call void @svf_assert(i1 noundef zeroext %3), !dbg !40
  ret i32 0, !dbg !41
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!13, !14, !15, !16, !17, !18, !19}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!20}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "tests/ae-relational/RecursiveModCases.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-recursive-mod-gates", checksumkind: CSK_MD5, checksum: "d8ab4d0843e1b0ee1a9d5eb918fb78ef")
!4 = !{!0, !5, !8, !10}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "h", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !DIGlobalVariableExpression(var: !9, expr: !DIExpression())
!9 = distinct !DIGlobalVariable(name: "other", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!10 = !DIGlobalVariableExpression(var: !11, expr: !DIExpression())
!11 = distinct !DIGlobalVariable(name: "gp", scope: !2, file: !3, line: 37, type: !12, isLocal: false, isDefinition: true)
!12 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!13 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!14 = !{i32 7, !"Dwarf Version", i32 5}
!15 = !{i32 2, !"Debug Info Version", i32 3}
!16 = !{i32 1, !"wchar_size", i32 4}
!17 = !{i32 8, !"PIC Level", i32 2}
!18 = !{i32 7, !"uwtable", i32 1}
!19 = !{i32 7, !"frame-pointer", i32 1}
!20 = !{!"Homebrew clang version 21.1.8"}
!21 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 38, type: !22, scopeLine: 38, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !24)
!22 = !DISubroutineType(types: !23)
!23 = !{null, !7}
!24 = !{}
!25 = !DILocalVariable(name: "n", arg: 1, scope: !21, file: !3, line: 38, type: !7)
!26 = !DILocation(line: 0, scope: !21)
!27 = !DILocation(line: 38, column: 49, scope: !28)
!28 = distinct !DILexicalBlock(scope: !21, file: !3, line: 38, column: 49)
!29 = !DILocation(line: 38, column: 58, scope: !28)
!30 = !DILocation(line: 38, column: 52, scope: !28)
!31 = !DILocation(line: 38, column: 69, scope: !28)
!32 = !DILocation(line: 38, column: 87, scope: !21)
!33 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 39, type: !34, scopeLine: 39, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!34 = !DISubroutineType(types: !35)
!35 = !{!7}
!36 = !DILocation(line: 39, column: 18, scope: !33)
!37 = !DILocation(line: 39, column: 38, scope: !33)
!38 = !DILocation(line: 39, column: 37, scope: !33)
!39 = !DILocation(line: 39, column: 41, scope: !33)
!40 = !DILocation(line: 39, column: 26, scope: !33)
!41 = !DILocation(line: 39, column: 48, scope: !33)
