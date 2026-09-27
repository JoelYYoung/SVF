; ModuleID = 'g9.raw.ll'
source_filename = "g.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0
@q = global i32 0, align 4, !dbg !7
@w = global i64 0, align 8, !dbg !12
@u = global i32 0, align 4, !dbg !9

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @overwrite(i32 noundef %0) #0 !dbg !23 {
    #dbg_value(i32 %0, !27, !DIExpression(), !28)
  store i32 %0, ptr @o, align 4, !dbg !29
  ret void, !dbg !30
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !31 {
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
  br label %21, !dbg !48

12:                                               ; preds = %9
  store i32 %1, ptr @o, align 4, !dbg !49
  store i32 %2, ptr @q, align 4, !dbg !50
  %13 = sext i32 %1 to i64, !dbg !51
  store i64 %13, ptr @w, align 8, !dbg !52
  %14 = load i64, ptr @w, align 8, !dbg !53
    #dbg_value(i64 %14, !54, !DIExpression(), !35)
  %15 = trunc i64 %14 to i32, !dbg !55
  %16 = icmp slt i32 %15, 5, !dbg !57
  br i1 %16, label %17, label %20, !dbg !57

17:                                               ; preds = %12
  %18 = load i64, ptr @w, align 8, !dbg !58
    #dbg_value(i64 %18, !60, !DIExpression(), !61)
  %19 = icmp slt i64 %18, 5, !dbg !62
  call void @svf_assert(i1 noundef zeroext %19), !dbg !63
  br label %20, !dbg !64

20:                                               ; preds = %17, %12
  br label %21, !dbg !65

21:                                               ; preds = %20, %11
  ret i32 0, !dbg !66
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!15, !16, !17, !18, !19, !20, !21}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!22}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 4, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !4, globals: !6, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "g.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/guards", checksumkind: CSK_MD5, checksum: "3a206e09e871b84ee61d0543fb047e4c")
!4 = !{!5}
!5 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!6 = !{!0, !7, !9, !12}
!7 = !DIGlobalVariableExpression(var: !8, expr: !DIExpression())
!8 = distinct !DIGlobalVariable(name: "q", scope: !2, file: !3, line: 4, type: !5, isLocal: false, isDefinition: true)
!9 = !DIGlobalVariableExpression(var: !10, expr: !DIExpression())
!10 = distinct !DIGlobalVariable(name: "u", scope: !2, file: !3, line: 4, type: !11, isLocal: false, isDefinition: true)
!11 = !DIBasicType(name: "unsigned int", size: 32, encoding: DW_ATE_unsigned)
!12 = !DIGlobalVariableExpression(var: !13, expr: !DIExpression())
!13 = distinct !DIGlobalVariable(name: "w", scope: !2, file: !3, line: 4, type: !14, isLocal: false, isDefinition: true)
!14 = !DIBasicType(name: "long", size: 64, encoding: DW_ATE_signed)
!15 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!16 = !{i32 7, !"Dwarf Version", i32 5}
!17 = !{i32 2, !"Debug Info Version", i32 3}
!18 = !{i32 1, !"wchar_size", i32 4}
!19 = !{i32 8, !"PIC Level", i32 2}
!20 = !{i32 7, !"uwtable", i32 1}
!21 = !{i32 7, !"frame-pointer", i32 1}
!22 = !{!"Homebrew clang version 21.1.8"}
!23 = distinct !DISubprogram(name: "overwrite", scope: !3, file: !3, line: 5, type: !24, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !26)
!24 = !DISubroutineType(types: !25)
!25 = !{null, !5}
!26 = !{}
!27 = !DILocalVariable(name: "v", arg: 1, scope: !23, file: !3, line: 5, type: !5)
!28 = !DILocation(line: 0, scope: !23)
!29 = !DILocation(line: 5, column: 52, scope: !23)
!30 = !DILocation(line: 5, column: 56, scope: !23)
!31 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !32, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !26)
!32 = !DISubroutineType(types: !4)
!33 = !DILocation(line: 7, column: 9, scope: !31)
!34 = !DILocalVariable(name: "x", scope: !31, file: !3, line: 7, type: !5)
!35 = !DILocation(line: 0, scope: !31)
!36 = !DILocation(line: 7, column: 25, scope: !31)
!37 = !DILocalVariable(name: "v", scope: !31, file: !3, line: 7, type: !5)
!38 = !DILocation(line: 7, column: 41, scope: !31)
!39 = !DILocalVariable(name: "c", scope: !31, file: !3, line: 7, type: !5)
!40 = !DILocation(line: 8, column: 9, scope: !41)
!41 = distinct !DILexicalBlock(scope: !31, file: !3, line: 8, column: 7)
!42 = !DILocation(line: 8, column: 17, scope: !41)
!43 = !DILocation(line: 8, column: 22, scope: !41)
!44 = !DILocation(line: 8, column: 29, scope: !41)
!45 = !DILocation(line: 8, column: 34, scope: !41)
!46 = !DILocation(line: 8, column: 42, scope: !41)
!47 = !DILocation(line: 8, column: 47, scope: !41)
!48 = !DILocation(line: 8, column: 55, scope: !41)
!49 = !DILocation(line: 9, column: 4, scope: !31)
!50 = !DILocation(line: 9, column: 9, scope: !31)
!51 = !DILocation(line: 39, column: 5, scope: !31)
!52 = !DILocation(line: 39, column: 4, scope: !31)
!53 = !DILocation(line: 39, column: 15, scope: !31)
!54 = !DILocalVariable(name: "t", scope: !31, file: !3, line: 39, type: !14)
!55 = !DILocation(line: 40, column: 7, scope: !56)
!56 = distinct !DILexicalBlock(scope: !31, file: !3, line: 40, column: 7)
!57 = !DILocation(line: 40, column: 13, scope: !56)
!58 = !DILocation(line: 40, column: 26, scope: !59)
!59 = distinct !DILexicalBlock(scope: !56, file: !3, line: 40, column: 17)
!60 = !DILocalVariable(name: "y", scope: !59, file: !3, line: 40, type: !14)
!61 = !DILocation(line: 0, scope: !59)
!62 = !DILocation(line: 40, column: 41, scope: !59)
!63 = !DILocation(line: 40, column: 29, scope: !59)
!64 = !DILocation(line: 40, column: 46, scope: !59)
!65 = !DILocation(line: 42, column: 3, scope: !31)
!66 = !DILocation(line: 43, column: 1, scope: !31)

