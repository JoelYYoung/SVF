; ModuleID = 'build/RecursiveModCase4.raw.ll'
source_filename = "tests/ae-relational/RecursiveModCases.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 1, align 4, !dbg !0
@h = global i32 0, align 4, !dbg !11
@other = global i32 2, align 4, !dbg !13

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i64 noundef %0, i32 noundef %1) #0 !dbg !23 {
    #dbg_value(i64 %0, !27, !DIExpression(), !28)
    #dbg_value(i32 %1, !29, !DIExpression(), !28)
  %3 = icmp ne i32 %1, 0, !dbg !30
  br i1 %3, label %4, label %6, !dbg !30

4:                                                ; preds = %2
  %5 = sub nsw i32 %1, 1, !dbg !32
  call void @rec(i64 noundef %0, i32 noundef %5), !dbg !33
  br label %8, !dbg !33

6:                                                ; preds = %2
  %7 = inttoptr i64 %0 to ptr, !dbg !34
  store i32 99, ptr %7, align 4, !dbg !35
  br label %8

8:                                                ; preds = %6, %4
  ret void, !dbg !36
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !37 {
  call void @rec(i64 noundef ptrtoint (ptr @g to i64), i32 noundef 1), !dbg !40
  %1 = load i32, ptr @g, align 4, !dbg !41
  %2 = icmp eq i32 %1, 1, !dbg !42
  call void @svf_assert(i1 noundef zeroext %2), !dbg !43
  ret i32 0, !dbg !44
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!15, !16, !17, !18, !19, !20, !21}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!22}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 4, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !4, globals: !10, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "tests/ae-relational/RecursiveModCases.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-recursive-mod", checksumkind: CSK_MD5, checksum: "d5281a7fc31afe84382c9122de707412")
!4 = !{!5, !7}
!5 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !6, size: 64)
!6 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!7 = !DIDerivedType(tag: DW_TAG_typedef, name: "uintptr_t", file: !8, line: 34, baseType: !9)
!8 = !DIFile(filename: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk/usr/include/sys/_types/_uintptr_t.h", directory: "", checksumkind: CSK_MD5, checksum: "e70ae655dd1b9d4ae0b1dcc073f5b7e4")
!9 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!10 = !{!0, !11, !13}
!11 = !DIGlobalVariableExpression(var: !12, expr: !DIExpression())
!12 = distinct !DIGlobalVariable(name: "h", scope: !2, file: !3, line: 4, type: !6, isLocal: false, isDefinition: true)
!13 = !DIGlobalVariableExpression(var: !14, expr: !DIExpression())
!14 = distinct !DIGlobalVariable(name: "other", scope: !2, file: !3, line: 4, type: !6, isLocal: false, isDefinition: true)
!15 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!16 = !{i32 7, !"Dwarf Version", i32 5}
!17 = !{i32 2, !"Debug Info Version", i32 3}
!18 = !{i32 1, !"wchar_size", i32 4}
!19 = !{i32 8, !"PIC Level", i32 2}
!20 = !{i32 7, !"uwtable", i32 1}
!21 = !{i32 7, !"frame-pointer", i32 1}
!22 = !{!"Homebrew clang version 21.1.8"}
!23 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 16, type: !24, scopeLine: 16, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !26)
!24 = !DISubroutineType(types: !25)
!25 = !{null, !7, !6}
!26 = !{}
!27 = !DILocalVariable(name: "p", arg: 1, scope: !23, file: !3, line: 16, type: !7)
!28 = !DILocation(line: 0, scope: !23)
!29 = !DILocalVariable(name: "n", arg: 2, scope: !23, file: !3, line: 16, type: !6)
!30 = !DILocation(line: 17, column: 7, scope: !31)
!31 = distinct !DILexicalBlock(scope: !23, file: !3, line: 17, column: 7)
!32 = !DILocation(line: 17, column: 19, scope: !31)
!33 = !DILocation(line: 17, column: 10, scope: !31)
!34 = !DILocation(line: 17, column: 31, scope: !31)
!35 = !DILocation(line: 17, column: 40, scope: !31)
!36 = !DILocation(line: 18, column: 1, scope: !23)
!37 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 19, type: !38, scopeLine: 19, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!38 = !DISubroutineType(types: !39)
!39 = !{!6}
!40 = !DILocation(line: 19, column: 18, scope: !37)
!41 = !DILocation(line: 19, column: 52, scope: !37)
!42 = !DILocation(line: 19, column: 54, scope: !37)
!43 = !DILocation(line: 19, column: 41, scope: !37)
!44 = !DILocation(line: 19, column: 61, scope: !37)
