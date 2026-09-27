; ModuleID = 'g3.raw.ll'
source_filename = "g.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0
@q = global i32 0, align 4, !dbg !5
@u = global i32 0, align 4, !dbg !8
@w = global i64 0, align 8, !dbg !11

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @overwrite(i32 noundef %0) #0 !dbg !22 {
    #dbg_value(i32 %0, !26, !DIExpression(), !27)
  store i32 %0, ptr @o, align 4, !dbg !28
  ret void, !dbg !29
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !30 {
  %1 = call i32 @nondet_i32(), !dbg !33
    #dbg_value(i32 %1, !34, !DIExpression(), !35)
  %2 = call i32 @nondet_i32(), !dbg !36
    #dbg_value(i32 %2, !37, !DIExpression(), !35)
  %3 = call i32 @nondet_i32(), !dbg !38
    #dbg_value(i32 %3, !39, !DIExpression(), !35)
  %4 = icmp slt i32 %1, -1000, !dbg !40
  br i1 %4, label %11, label %5, !dbg !42

5:                                                ; preds = %0
  %6 = icmp sgt i32 %1, 1000, !dbg !43
  br i1 %6, label %11, label %7, !dbg !44

7:                                                ; preds = %5
  %8 = icmp slt i32 %2, -1000, !dbg !45
  br i1 %8, label %11, label %9, !dbg !46

9:                                                ; preds = %7
  %10 = icmp sgt i32 %2, 1000, !dbg !47
  br i1 %10, label %11, label %12, !dbg !46

11:                                               ; preds = %9, %7, %5, %0
  br label %19, !dbg !48

12:                                               ; preds = %9
  store i32 %1, ptr @o, align 4, !dbg !49
  store i32 %2, ptr @q, align 4, !dbg !50
  %13 = load i32, ptr @o, align 4, !dbg !51
    #dbg_value(i32 %13, !52, !DIExpression(), !35)
  store i32 %2, ptr @o, align 4, !dbg !53
  switch i32 %13, label %17 [
    i32 0, label %14
  ], !dbg !54

14:                                               ; preds = %12
  %15 = load i32, ptr @o, align 4, !dbg !55
    #dbg_value(i32 %15, !58, !DIExpression(), !59)
  %16 = icmp eq i32 %15, 0, !dbg !60
  call void @svf_assert(i1 noundef zeroext %16), !dbg !61
  br label %18, !dbg !62

17:                                               ; preds = %12
  br label %18, !dbg !63

18:                                               ; preds = %17, %14
  br label %19, !dbg !64

19:                                               ; preds = %18, %11
  ret i32 0, !dbg !65
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!14, !15, !16, !17, !18, !19, !20}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!21}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "g.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/guards", checksumkind: CSK_MD5, checksum: "7df97cfff1f3bcfbf740689c7ffcbe69")
!4 = !{!0, !5, !8, !11}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "q", scope: !2, file: !3, line: 4, type: !7, isLocal: false, isDefinition: true)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !DIGlobalVariableExpression(var: !9, expr: !DIExpression())
!9 = distinct !DIGlobalVariable(name: "u", scope: !2, file: !3, line: 4, type: !10, isLocal: false, isDefinition: true)
!10 = !DIBasicType(name: "unsigned int", size: 32, encoding: DW_ATE_unsigned)
!11 = !DIGlobalVariableExpression(var: !12, expr: !DIExpression())
!12 = distinct !DIGlobalVariable(name: "w", scope: !2, file: !3, line: 4, type: !13, isLocal: false, isDefinition: true)
!13 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!14 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!15 = !{i32 7, !"Dwarf Version", i32 5}
!16 = !{i32 2, !"Debug Info Version", i32 3}
!17 = !{i32 1, !"wchar_size", i32 4}
!18 = !{i32 8, !"PIC Level", i32 2}
!19 = !{i32 7, !"uwtable", i32 1}
!20 = !{i32 7, !"frame-pointer", i32 1}
!21 = !{!"Homebrew clang version 21.1.8"}
!22 = distinct !DISubprogram(name: "overwrite", scope: !3, file: !3, line: 5, type: !23, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !25)
!23 = !DISubroutineType(types: !24)
!24 = !{null, !7}
!25 = !{}
!26 = !DILocalVariable(name: "v", arg: 1, scope: !22, file: !3, line: 5, type: !7)
!27 = !DILocation(line: 0, scope: !22)
!28 = !DILocation(line: 5, column: 52, scope: !22)
!29 = !DILocation(line: 5, column: 56, scope: !22)
!30 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !31, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !25)
!31 = !DISubroutineType(types: !32)
!32 = !{!7}
!33 = !DILocation(line: 7, column: 9, scope: !30)
!34 = !DILocalVariable(name: "x", scope: !30, file: !3, line: 7, type: !7)
!35 = !DILocation(line: 0, scope: !30)
!36 = !DILocation(line: 7, column: 25, scope: !30)
!37 = !DILocalVariable(name: "v", scope: !30, file: !3, line: 7, type: !7)
!38 = !DILocation(line: 7, column: 41, scope: !30)
!39 = !DILocalVariable(name: "c", scope: !30, file: !3, line: 7, type: !7)
!40 = !DILocation(line: 8, column: 9, scope: !41)
!41 = distinct !DILexicalBlock(scope: !30, file: !3, line: 8, column: 7)
!42 = !DILocation(line: 8, column: 17, scope: !41)
!43 = !DILocation(line: 8, column: 22, scope: !41)
!44 = !DILocation(line: 8, column: 29, scope: !41)
!45 = !DILocation(line: 8, column: 34, scope: !41)
!46 = !DILocation(line: 8, column: 42, scope: !41)
!47 = !DILocation(line: 8, column: 47, scope: !41)
!48 = !DILocation(line: 8, column: 55, scope: !41)
!49 = !DILocation(line: 9, column: 4, scope: !30)
!50 = !DILocation(line: 9, column: 9, scope: !30)
!51 = !DILocation(line: 18, column: 9, scope: !30)
!52 = !DILocalVariable(name: "t", scope: !30, file: !3, line: 18, type: !7)
!53 = !DILocation(line: 18, column: 13, scope: !30)
!54 = !DILocation(line: 19, column: 3, scope: !30)
!55 = !DILocation(line: 19, column: 32, scope: !56)
!56 = distinct !DILexicalBlock(scope: !57, file: !3, line: 19, column: 24)
!57 = distinct !DILexicalBlock(scope: !30, file: !3, line: 19, column: 14)
!58 = !DILocalVariable(name: "y", scope: !56, file: !3, line: 19, type: !7)
!59 = !DILocation(line: 0, scope: !56)
!60 = !DILocation(line: 19, column: 47, scope: !56)
!61 = !DILocation(line: 19, column: 35, scope: !56)
!62 = !DILocation(line: 19, column: 53, scope: !56)
!63 = !DILocation(line: 19, column: 71, scope: !57)
!64 = !DILocation(line: 31, column: 3, scope: !30)
!65 = !DILocation(line: 32, column: 1, scope: !30)

