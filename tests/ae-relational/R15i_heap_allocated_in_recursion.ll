; ModuleID = 'r15i.raw.ll'
source_filename = "r15i.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@h = global ptr null, align 8, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @rec(i32 noundef %0) #0 !dbg !15 {
    #dbg_value(i32 %0, !19, !DIExpression(), !20)
  %2 = icmp sgt i32 %0, 0, !dbg !21
  br i1 %2, label %3, label %5, !dbg !21

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !23
  call void @rec(i32 noundef %4), !dbg !24
  br label %8, !dbg !24

5:                                                ; preds = %1
  %6 = call ptr @malloc(i64 noundef 4) #3, !dbg !25
  store ptr %6, ptr @h, align 8, !dbg !27
  %7 = load ptr, ptr @h, align 8, !dbg !28
  store i32 7, ptr %7, align 4, !dbg !29
  br label %8

8:                                                ; preds = %5, %3
  ret void, !dbg !30
}

; Function Attrs: allocsize(0)
declare ptr @malloc(i64 noundef) #1

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !31 {
  store ptr null, ptr @h, align 8, !dbg !34
  call void @rec(i32 noundef 1), !dbg !35
  %1 = load ptr, ptr @h, align 8, !dbg !36
  %2 = load i32, ptr %1, align 4, !dbg !37
    #dbg_value(i32 %2, !38, !DIExpression(), !39)
  %3 = icmp eq i32 %2, 5, !dbg !40
  call void @svf_assert(i1 noundef zeroext %3), !dbg !41
  ret i32 0, !dbg !42
}

declare void @svf_assert(i1 noundef zeroext) #2

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { allocsize(0) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #2 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #3 = { allocsize(0) }

!llvm.module.flags = !{!7, !8, !9, !10, !11, !12, !13}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!14}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "h", scope: !2, file: !3, line: 4, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "r15i.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r15h", checksumkind: CSK_MD5, checksum: "008ffb142731360b2fdfae51a0dbf561")
!4 = !{!0}
!5 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !6, size: 64)
!6 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!7 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!8 = !{i32 7, !"Dwarf Version", i32 5}
!9 = !{i32 2, !"Debug Info Version", i32 3}
!10 = !{i32 1, !"wchar_size", i32 4}
!11 = !{i32 8, !"PIC Level", i32 2}
!12 = !{i32 7, !"uwtable", i32 1}
!13 = !{i32 7, !"frame-pointer", i32 1}
!14 = !{!"Homebrew clang version 21.1.8"}
!15 = distinct !DISubprogram(name: "rec", scope: !3, file: !3, line: 5, type: !16, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!16 = !DISubroutineType(types: !17)
!17 = !{null, !6}
!18 = !{}
!19 = !DILocalVariable(name: "n", arg: 1, scope: !15, file: !3, line: 5, type: !6)
!20 = !DILocation(line: 0, scope: !15)
!21 = !DILocation(line: 5, column: 51, scope: !22)
!22 = distinct !DILexicalBlock(scope: !15, file: !3, line: 5, column: 49)
!23 = !DILocation(line: 5, column: 62, scope: !22)
!24 = !DILocation(line: 5, column: 56, scope: !22)
!25 = !DILocation(line: 5, column: 79, scope: !26)
!26 = distinct !DILexicalBlock(scope: !22, file: !3, line: 5, column: 73)
!27 = !DILocation(line: 5, column: 77, scope: !26)
!28 = !DILocation(line: 5, column: 101, scope: !26)
!29 = !DILocation(line: 5, column: 103, scope: !26)
!30 = !DILocation(line: 5, column: 110, scope: !15)
!31 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !32, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!32 = !DISubroutineType(types: !33)
!33 = !{!6}
!34 = !DILocation(line: 7, column: 5, scope: !31)
!35 = !DILocation(line: 8, column: 3, scope: !31)
!36 = !DILocation(line: 9, column: 12, scope: !31)
!37 = !DILocation(line: 9, column: 11, scope: !31)
!38 = !DILocalVariable(name: "y", scope: !31, file: !3, line: 9, type: !6)
!39 = !DILocation(line: 0, scope: !31)
!40 = !DILocation(line: 10, column: 16, scope: !31)
!41 = !DILocation(line: 10, column: 3, scope: !31)
!42 = !DILocation(line: 11, column: 3, scope: !31)
