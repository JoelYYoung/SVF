; ModuleID = '/tmp/RecursiveModCase7.raw.ll'
source_filename = "tests/ae-relational/RecursiveModCases.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 1, align 4, !dbg !0
@h = global i32 0, align 4, !dbg !5
@other = global i32 2, align 4, !dbg !8
@fp = global ptr null, align 8, !dbg !10

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i32 noundef %0) #0 !dbg !23 {
    #dbg_value(i32 %0, !27, !DIExpression(), !28)
  %2 = icmp ne i32 %0, 0, !dbg !29
  br i1 %2, label %3, label %5, !dbg !29

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !31
  call void @rec(i32 noundef %4), !dbg !32
  br label %7, !dbg !32

5:                                                ; preds = %1
  %6 = load ptr, ptr @fp, align 8, !dbg !33
  call void %6(), !dbg !33
  br label %7

7:                                                ; preds = %5, %3
  ret void, !dbg !34
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !35 {
  %1 = call ptr @unknown(), !dbg !38
  store ptr %1, ptr @fp, align 8, !dbg !39
  call void @rec(i32 noundef 1), !dbg !40
  %2 = load i32, ptr @g, align 4, !dbg !41
  %3 = icmp eq i32 %2, 1, !dbg !42
  call void @svf_assert(i1 noundef zeroext %3), !dbg !43
  ret i32 0, !dbg !44
}

declare ptr @unknown() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!15, !16, !17, !18, !19, !20, !21}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!22}

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
!11 = distinct !DIGlobalVariable(name: "fp", scope: !2, file: !3, line: 32, type: !12, isLocal: false, isDefinition: true)
!12 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !13, size: 64)
!13 = !DISubroutineType(types: !14)
!14 = !{null}
!15 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!16 = !{i32 7, !"Dwarf Version", i32 5}
!17 = !{i32 2, !"Debug Info Version", i32 3}
!18 = !{i32 1, !"wchar_size", i32 4}
!19 = !{i32 8, !"PIC Level", i32 2}
!20 = !{i32 7, !"uwtable", i32 1}
!21 = !{i32 7, !"frame-pointer", i32 1}
!22 = !{!"Homebrew clang version 21.1.8"}
!23 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 33, type: !24, scopeLine: 33, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !26)
!24 = !DISubroutineType(types: !25)
!25 = !{null, !7}
!26 = !{}
!27 = !DILocalVariable(name: "n", arg: 1, scope: !23, file: !3, line: 33, type: !7)
!28 = !DILocation(line: 0, scope: !23)
!29 = !DILocation(line: 33, column: 49, scope: !30)
!30 = distinct !DILexicalBlock(scope: !23, file: !3, line: 33, column: 49)
!31 = !DILocation(line: 33, column: 58, scope: !30)
!32 = !DILocation(line: 33, column: 52, scope: !30)
!33 = !DILocation(line: 33, column: 69, scope: !30)
!34 = !DILocation(line: 33, column: 75, scope: !23)
!35 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 34, type: !36, scopeLine: 34, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!36 = !DISubroutineType(types: !37)
!37 = !{!7}
!38 = !DILocation(line: 34, column: 23, scope: !35)
!39 = !DILocation(line: 34, column: 21, scope: !35)
!40 = !DILocation(line: 34, column: 34, scope: !35)
!41 = !DILocation(line: 34, column: 53, scope: !35)
!42 = !DILocation(line: 34, column: 55, scope: !35)
!43 = !DILocation(line: 34, column: 42, scope: !35)
!44 = !DILocation(line: 34, column: 62, scope: !35)
