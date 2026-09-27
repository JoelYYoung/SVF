; ModuleID = 'r15h.raw.ll'
source_filename = "r15h.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(ptr noundef %0, i32 noundef %1) #0 !dbg !10 {
    #dbg_value(ptr %0, !16, !DIExpression(), !17)
    #dbg_value(i32 %1, !18, !DIExpression(), !17)
  %3 = icmp sgt i32 %1, 0, !dbg !19
  br i1 %3, label %4, label %6, !dbg !19

4:                                                ; preds = %2
  %5 = sub nsw i32 %1, 1, !dbg !21
  call void @rec(ptr noundef %0, i32 noundef %5), !dbg !22
  br label %7, !dbg !22

6:                                                ; preds = %2
  store i32 7, ptr %0, align 4, !dbg !23
  br label %7

7:                                                ; preds = %6, %4
  ret void, !dbg !24
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !25 {
  %1 = alloca i32, align 4
    #dbg_declare(ptr %1, !28, !DIExpression(), !29)
  call void @rec(ptr noundef %1, i32 noundef 1), !dbg !30
  %2 = load i32, ptr %1, align 4, !dbg !31
    #dbg_value(i32 %2, !32, !DIExpression(), !33)
  %3 = icmp eq i32 %2, 5, !dbg !34
  call void @svf_assert(i1 noundef zeroext %3), !dbg !35
  ret i32 0, !dbg !36
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}
!llvm.dbg.cu = !{!7}
!llvm.ident = !{!9}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!1 = !{i32 7, !"Dwarf Version", i32 5}
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !{i32 1, !"wchar_size", i32 4}
!4 = !{i32 8, !"PIC Level", i32 2}
!5 = !{i32 7, !"uwtable", i32 1}
!6 = !{i32 7, !"frame-pointer", i32 1}
!7 = distinct !DICompileUnit(language: DW_LANG_C11, file: !8, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!8 = !DIFile(filename: "r15h.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r15h", checksumkind: CSK_MD5, checksum: "09556803ff5091f18cd47aa11e574b20")
!9 = !{!"Homebrew clang version 21.1.8"}
!10 = distinct !DISubprogram(name: "rec", scope: !8, file: !8, line: 3, type: !11, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !15)
!11 = !DISubroutineType(types: !12)
!12 = !{null, !13, !14}
!13 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !14, size: 64)
!14 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!15 = !{}
!16 = !DILocalVariable(name: "p", arg: 1, scope: !10, file: !8, line: 3, type: !13)
!17 = !DILocation(line: 0, scope: !10)
!18 = !DILocalVariable(name: "n", arg: 2, scope: !10, file: !8, line: 3, type: !14)
!19 = !DILocation(line: 3, column: 59, scope: !20)
!20 = distinct !DILexicalBlock(scope: !10, file: !8, line: 3, column: 57)
!21 = !DILocation(line: 3, column: 73, scope: !20)
!22 = !DILocation(line: 3, column: 64, scope: !20)
!23 = !DILocation(line: 3, column: 87, scope: !20)
!24 = !DILocation(line: 3, column: 92, scope: !10)
!25 = distinct !DISubprogram(name: "main", scope: !8, file: !8, line: 4, type: !26, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !15)
!26 = !DISubroutineType(types: !27)
!27 = !{!14}
!28 = !DILocalVariable(name: "x", scope: !25, file: !8, line: 5, type: !14)
!29 = !DILocation(line: 5, column: 7, scope: !25)
!30 = !DILocation(line: 6, column: 3, scope: !25)
!31 = !DILocation(line: 7, column: 11, scope: !25)
!32 = !DILocalVariable(name: "y", scope: !25, file: !8, line: 7, type: !14)
!33 = !DILocation(line: 0, scope: !25)
!34 = !DILocation(line: 8, column: 16, scope: !25)
!35 = !DILocation(line: 8, column: 3, scope: !25)
!36 = !DILocation(line: 9, column: 3, scope: !25)
