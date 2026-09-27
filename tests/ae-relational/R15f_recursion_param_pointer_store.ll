; ModuleID = 'r15f.raw.ll'
source_filename = "r15f.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(ptr noundef %0, i32 noundef %1) #0 !dbg !14 {
    #dbg_value(ptr %0, !19, !DIExpression(), !20)
    #dbg_value(i32 %1, !21, !DIExpression(), !20)
  %3 = icmp sgt i32 %1, 0, !dbg !22
  br i1 %3, label %4, label %6, !dbg !22

4:                                                ; preds = %2
  %5 = sub nsw i32 %1, 1, !dbg !24
  call void @rec(ptr noundef %0, i32 noundef %5), !dbg !25
  br label %8, !dbg !25

6:                                                ; preds = %2
  %7 = add nsw i32 %1, 98, !dbg !26
  store i32 %7, ptr %0, align 4, !dbg !27
  br label %8

8:                                                ; preds = %6, %4
  ret void, !dbg !28
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !29 {
  store i32 1, ptr @g, align 4, !dbg !32
  call void @rec(ptr noundef @g, i32 noundef 1), !dbg !33
  %1 = load i32, ptr @g, align 4, !dbg !34
  %2 = icmp eq i32 %1, 1, !dbg !35
  call void @svf_assert(i1 noundef zeroext %2), !dbg !36
  ret i32 0, !dbg !37
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 3, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "r15f.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r15", checksumkind: CSK_MD5, checksum: "c77af01a915eb72ae3759c9d1aaf41d5")
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
!14 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 4, type: !15, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!15 = !DISubroutineType(types: !16)
!16 = !{null, !17, !5}
!17 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !5, size: 64)
!18 = !{}
!19 = !DILocalVariable(name: "p", arg: 1, scope: !14, file: !3, line: 4, type: !17)
!20 = !DILocation(line: 0, scope: !14)
!21 = !DILocalVariable(name: "n", arg: 2, scope: !14, file: !3, line: 4, type: !5)
!22 = !DILocation(line: 4, column: 59, scope: !23)
!23 = distinct !DILexicalBlock(scope: !14, file: !3, line: 4, column: 57)
!24 = !DILocation(line: 4, column: 73, scope: !23)
!25 = !DILocation(line: 4, column: 64, scope: !23)
!26 = !DILocation(line: 4, column: 91, scope: !23)
!27 = !DILocation(line: 4, column: 87, scope: !23)
!28 = !DILocation(line: 4, column: 97, scope: !14)
!29 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 5, type: !30, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!30 = !DISubroutineType(types: !31)
!31 = !{!5}
!32 = !DILocation(line: 6, column: 5, scope: !29)
!33 = !DILocation(line: 7, column: 3, scope: !29)
!34 = !DILocation(line: 8, column: 14, scope: !29)
!35 = !DILocation(line: 8, column: 16, scope: !29)
!36 = !DILocation(line: 8, column: 3, scope: !29)
!37 = !DILocation(line: 9, column: 3, scope: !29)
