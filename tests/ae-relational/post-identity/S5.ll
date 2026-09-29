; ModuleID = 'S5_boolcond.raw.ll'
source_filename = "S5_boolcond.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@flag = global i8 0, align 1, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !14 {
  %1 = call i32 @nondet_i32(), !dbg !19
  %2 = icmp sgt i32 %1, 3, !dbg !20
  %3 = zext i1 %2 to i8, !dbg !21
  store i8 %3, ptr @flag, align 1, !dbg !21
    #dbg_value(i32 0, !22, !DIExpression(), !23)
  %4 = load i8, ptr @flag, align 1, !dbg !24
  %5 = trunc i8 %4 to i1, !dbg !24
  br i1 %5, label %6, label %7, !dbg !24

6:                                                ; preds = %0
    #dbg_value(i32 1, !22, !DIExpression(), !23)
  br label %7, !dbg !26

7:                                                ; preds = %6, %0
  %.0 = phi i32 [ 1, %6 ], [ 0, %0 ], !dbg !27
    #dbg_value(i32 %.0, !22, !DIExpression(), !23)
  ret i32 %.0, !dbg !28
}

declare i32 @nondet_i32() #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "flag", scope: !2, file: !3, line: 3, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "S5_boolcond.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/51ab49a9-30ea-46ef-a16d-d36be8c20fba/scratchpad/postid", checksumkind: CSK_MD5, checksum: "e5392bb0f1c5998d486733b7265041ed")
!4 = !{!0}
!5 = !DIBasicType(name: "_Bool", size: 8, encoding: DW_ATE_boolean)
!6 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!7 = !{i32 7, !"Dwarf Version", i32 5}
!8 = !{i32 2, !"Debug Info Version", i32 3}
!9 = !{i32 1, !"wchar_size", i32 4}
!10 = !{i32 8, !"PIC Level", i32 2}
!11 = !{i32 7, !"uwtable", i32 1}
!12 = !{i32 7, !"frame-pointer", i32 1}
!13 = !{!"Homebrew clang version 21.1.8"}
!14 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 4, type: !15, scopeLine: 4, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!15 = !DISubroutineType(types: !16)
!16 = !{!17}
!17 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!18 = !{}
!19 = !DILocation(line: 4, column: 25, scope: !14)
!20 = !DILocation(line: 4, column: 38, scope: !14)
!21 = !DILocation(line: 4, column: 23, scope: !14)
!22 = !DILocalVariable(name: "r", scope: !14, file: !3, line: 4, type: !17)
!23 = !DILocation(line: 0, scope: !14)
!24 = !DILocation(line: 4, column: 58, scope: !25)
!25 = distinct !DILexicalBlock(scope: !14, file: !3, line: 4, column: 58)
!26 = !DILocation(line: 4, column: 64, scope: !25)
!27 = !DILocation(line: 4, scope: !14)
!28 = !DILocation(line: 4, column: 71, scope: !14)
