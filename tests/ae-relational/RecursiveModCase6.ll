; ModuleID = '/tmp/RecursiveModCase6.raw.ll'
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
define void @leaf(i32 noundef %0) #0 !dbg !30 {
    #dbg_value(i32 %0, !31, !DIExpression(), !32)
  %2 = add nsw i32 %0, 1, !dbg !33
  store i32 %2, ptr @g, align 4, !dbg !34
  ret void, !dbg !35
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main(i32 noundef %0, ptr noundef %1) #0 !dbg !36 {
    #dbg_value(i32 %0, !42, !DIExpression(), !43)
    #dbg_value(ptr %1, !44, !DIExpression(), !43)
  %3 = icmp sgt i32 %0, 1, !dbg !45
  %4 = zext i1 %3 to i64, !dbg !46
  %5 = select i1 %3, ptr @rec, ptr @leaf, !dbg !46
    #dbg_value(ptr %5, !47, !DIExpression(), !43)
  call void %5(i32 noundef 1), !dbg !49
  %6 = load i32, ptr @g, align 4, !dbg !50
  %7 = icmp eq i32 %6, 1, !dbg !51
  call void @svf_assert(i1 noundef zeroext %7), !dbg !52
  ret i32 0, !dbg !53
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
!3 = !DIFile(filename: "tests/ae-relational/RecursiveModCases.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-recursive-mod-gates", checksumkind: CSK_MD5, checksum: "d8ab4d0843e1b0ee1a9d5eb918fb78ef")
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
!18 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 24, type: !19, scopeLine: 24, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !21)
!19 = !DISubroutineType(types: !20)
!20 = !{null, !7}
!21 = !{}
!22 = !DILocalVariable(name: "n", arg: 1, scope: !18, file: !3, line: 24, type: !7)
!23 = !DILocation(line: 0, scope: !18)
!24 = !DILocation(line: 24, column: 49, scope: !25)
!25 = distinct !DILexicalBlock(scope: !18, file: !3, line: 24, column: 49)
!26 = !DILocation(line: 24, column: 58, scope: !25)
!27 = !DILocation(line: 24, column: 52, scope: !25)
!28 = !DILocation(line: 24, column: 71, scope: !25)
!29 = !DILocation(line: 24, column: 77, scope: !18)
!30 = distinct !DISubprogram(name: "leaf", scope: !3, file: !3, line: 25, type: !19, scopeLine: 25, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !21)
!31 = !DILocalVariable(name: "n", arg: 1, scope: !30, file: !3, line: 25, type: !7)
!32 = !DILocation(line: 0, scope: !30)
!33 = !DILocation(line: 25, column: 52, scope: !30)
!34 = !DILocation(line: 25, column: 48, scope: !30)
!35 = !DILocation(line: 25, column: 57, scope: !30)
!36 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 26, type: !37, scopeLine: 26, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !21)
!37 = !DISubroutineType(types: !38)
!38 = !{!7, !7, !39}
!39 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !40, size: 64)
!40 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !41, size: 64)
!41 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!42 = !DILocalVariable(name: "argc", arg: 1, scope: !36, file: !3, line: 26, type: !7)
!43 = !DILocation(line: 0, scope: !36)
!44 = !DILocalVariable(name: "argv", arg: 2, scope: !36, file: !3, line: 26, type: !39)
!45 = !DILocation(line: 27, column: 25, scope: !36)
!46 = !DILocation(line: 27, column: 20, scope: !36)
!47 = !DILocalVariable(name: "f", scope: !36, file: !3, line: 27, type: !48)
!48 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !19, size: 64)
!49 = !DILocation(line: 28, column: 3, scope: !36)
!50 = !DILocation(line: 28, column: 20, scope: !36)
!51 = !DILocation(line: 28, column: 22, scope: !36)
!52 = !DILocation(line: 28, column: 9, scope: !36)
!53 = !DILocation(line: 28, column: 29, scope: !36)
