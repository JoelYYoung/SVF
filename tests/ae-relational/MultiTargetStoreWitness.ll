; ModuleID = 'R03.raw.ll'
source_filename = "R03_multi_target_store_overwrite.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0
@q = global i32 0, align 4, !dbg !5

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !16 {
  store i32 5, ptr @o, align 4, !dbg !20
  store i32 0, ptr @q, align 4, !dbg !21
  %1 = call i32 @nondet_i32(), !dbg !22
    #dbg_value(i32 %1, !23, !DIExpression(), !24)
  %2 = icmp sge i32 %1, 100, !dbg !25
  br i1 %2, label %3, label %5, !dbg !25

3:                                                ; preds = %0
  %4 = icmp sle i32 %1, 200, !dbg !25
  br i1 %4, label %6, label %5, !dbg !25

5:                                                ; preds = %3, %0
  br label %15, !dbg !25

6:                                                ; preds = %3
  %7 = call i32 @nondet_i32(), !dbg !27
  %8 = icmp ne i32 %7, 0, !dbg !27
  %9 = zext i1 %8 to i64, !dbg !27
  %10 = select i1 %8, ptr @o, ptr @q, !dbg !27
    #dbg_value(ptr %10, !28, !DIExpression(), !24)
  store i32 %1, ptr %10, align 4, !dbg !30
  %11 = load i32, ptr @o, align 4, !dbg !31
    #dbg_value(i32 %11, !32, !DIExpression(), !24)
  %12 = icmp sge i32 %11, 100, !dbg !33
  call void @svf_assert(i1 noundef zeroext %12), !dbg !34
  %13 = icmp sge i32 %11, 5, !dbg !35
  call void @svf_assert(i1 noundef zeroext %13), !dbg !36
  %14 = icmp sle i32 %11, 200, !dbg !37
  call void @svf_assert(i1 noundef zeroext %14), !dbg !38
  br label %15, !dbg !39

15:                                               ; preds = %6, %5
  ret i32 0, !dbg !40
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!8, !9, !10, !11, !12, !13, !14}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!15}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 5, type: !7, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "R03_multi_target_store_overwrite.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/supervisor-sparse-fixtures/review-probes", checksumkind: CSK_MD5, checksum: "786fd64a306a6fd99894f8eeab771905")
!4 = !{!0, !5}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "q", scope: !2, file: !3, line: 5, type: !7, isLocal: false, isDefinition: true)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!9 = !{i32 7, !"Dwarf Version", i32 5}
!10 = !{i32 2, !"Debug Info Version", i32 3}
!11 = !{i32 1, !"wchar_size", i32 4}
!12 = !{i32 8, !"PIC Level", i32 2}
!13 = !{i32 7, !"uwtable", i32 1}
!14 = !{i32 7, !"frame-pointer", i32 1}
!15 = !{!"Homebrew clang version 21.1.8"}
!16 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !17, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !19)
!17 = !DISubroutineType(types: !18)
!18 = !{!7}
!19 = !{}
!20 = !DILocation(line: 7, column: 5, scope: !16)
!21 = !DILocation(line: 7, column: 12, scope: !16)
!22 = !DILocation(line: 8, column: 11, scope: !16)
!23 = !DILocalVariable(name: "v", scope: !16, file: !3, line: 8, type: !7)
!24 = !DILocation(line: 0, scope: !16)
!25 = !DILocation(line: 9, column: 3, scope: !26)
!26 = distinct !DILexicalBlock(scope: !16, file: !3, line: 9, column: 3)
!27 = !DILocation(line: 10, column: 12, scope: !16)
!28 = !DILocalVariable(name: "p", scope: !16, file: !3, line: 10, type: !29)
!29 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!30 = !DILocation(line: 11, column: 6, scope: !16)
!31 = !DILocation(line: 12, column: 11, scope: !16)
!32 = !DILocalVariable(name: "y", scope: !16, file: !3, line: 12, type: !7)
!33 = !DILocation(line: 13, column: 16, scope: !16)
!34 = !DILocation(line: 13, column: 3, scope: !16)
!35 = !DILocation(line: 14, column: 16, scope: !16)
!36 = !DILocation(line: 14, column: 3, scope: !16)
!37 = !DILocation(line: 15, column: 16, scope: !16)
!38 = !DILocation(line: 15, column: 3, scope: !16)
!39 = !DILocation(line: 16, column: 3, scope: !16)
!40 = !DILocation(line: 17, column: 1, scope: !16)
