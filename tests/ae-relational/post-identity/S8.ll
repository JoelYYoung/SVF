; ModuleID = 'S8.raw.ll'
source_filename = "S8_samename.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@buf = global [8 x i8] c"a,b\00\00\00\00\00", align 1, !dbg !0
@.str = private unnamed_addr constant [2 x i8] c",\00", align 1, !dbg !9

; Function Attrs: noinline nounwind ssp uwtable(sync)
define ptr @strchr(ptr noundef %0, i32 noundef %1) #0 !dbg !25 {
    #dbg_value(ptr %0, !32, !DIExpression(), !33)
    #dbg_value(i32 %1, !34, !DIExpression(), !33)
  br label %3, !dbg !35

3:                                                ; preds = %14, %2
  %.01 = phi ptr [ %0, %2 ], [ %15, %14 ]
    #dbg_value(ptr %.01, !32, !DIExpression(), !33)
  %4 = load i8, ptr %.01, align 1, !dbg !36
  %5 = icmp ne i8 %4, 0, !dbg !39
  br i1 %5, label %6, label %16, !dbg !39

6:                                                ; preds = %3
  %7 = load i8, ptr %.01, align 1, !dbg !40
  %8 = sext i8 %7 to i32, !dbg !40
  %9 = trunc i32 %1 to i8, !dbg !42
  %10 = sext i8 %9 to i32, !dbg !42
  %11 = icmp eq i32 %8, %10, !dbg !43
  br i1 %11, label %12, label %13, !dbg !43

12:                                               ; preds = %6
  br label %17, !dbg !44

13:                                               ; preds = %6
  br label %14, !dbg !45

14:                                               ; preds = %13
  %15 = getelementptr inbounds nuw i8, ptr %.01, i32 1, !dbg !46
    #dbg_value(ptr %15, !32, !DIExpression(), !33)
  br label %3, !dbg !47, !llvm.loop !48

16:                                               ; preds = %3
  br label %17, !dbg !51

17:                                               ; preds = %16, %12
  %.0 = phi ptr [ %.01, %12 ], [ null, %16 ], !dbg !33
  ret ptr %.0, !dbg !52
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !53 {
  %1 = call ptr @strchr(ptr noundef @buf, i32 noundef 44) #2, !dbg !56
    #dbg_value(ptr %1, !57, !DIExpression(), !58)
  %2 = call ptr @strtok(ptr noundef @buf, ptr noundef @.str) #2, !dbg !59
    #dbg_value(ptr %2, !60, !DIExpression(), !58)
  %3 = icmp ne ptr %1, null, !dbg !61
  %4 = zext i1 %3 to i32, !dbg !61
  %5 = icmp ne ptr %2, null, !dbg !62
  %6 = zext i1 %5 to i32, !dbg !62
  %7 = add nsw i32 %4, %6, !dbg !63
  ret i32 %7, !dbg !64
}

declare ptr @strtok(ptr noundef, ptr noundef) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #2 = { nobuiltin "no-builtins" }

!llvm.module.flags = !{!17, !18, !19, !20, !21, !22, !23}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!24}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "buf", scope: !2, file: !3, line: 8, type: !14, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !4, globals: !8, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "S8_samename.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/51ab49a9-30ea-46ef-a16d-d36be8c20fba/scratchpad/postid", checksumkind: CSK_MD5, checksum: "ea9b253b888b7056e6c4e6f1a9295c98")
!4 = !{!5, !6, !7}
!5 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !5, size: 64)
!7 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)
!8 = !{!0, !9}
!9 = !DIGlobalVariableExpression(var: !10, expr: !DIExpression())
!10 = distinct !DIGlobalVariable(scope: null, file: !3, line: 9, type: !11, isLocal: true, isDefinition: true)
!11 = !DICompositeType(tag: DW_TAG_array_type, baseType: !5, size: 16, elements: !12)
!12 = !{!13}
!13 = !DISubrange(count: 2)
!14 = !DICompositeType(tag: DW_TAG_array_type, baseType: !5, size: 64, elements: !15)
!15 = !{!16}
!16 = !DISubrange(count: 8)
!17 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!18 = !{i32 7, !"Dwarf Version", i32 5}
!19 = !{i32 2, !"Debug Info Version", i32 3}
!20 = !{i32 1, !"wchar_size", i32 4}
!21 = !{i32 8, !"PIC Level", i32 2}
!22 = !{i32 7, !"uwtable", i32 1}
!23 = !{i32 7, !"frame-pointer", i32 1}
!24 = !{!"Homebrew clang version 21.1.8"}
!25 = distinct !DISubprogram(name: "strchr", scope: !3, file: !3, line: 3, type: !26, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !31)
!26 = !DISubroutineType(types: !27)
!27 = !{!6, !28, !30}
!28 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !29, size: 64)
!29 = !DIDerivedType(tag: DW_TAG_const_type, baseType: !5)
!30 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!31 = !{}
!32 = !DILocalVariable(name: "s", arg: 1, scope: !25, file: !3, line: 3, type: !28)
!33 = !DILocation(line: 0, scope: !25)
!34 = !DILocalVariable(name: "c", arg: 2, scope: !25, file: !3, line: 3, type: !30)
!35 = !DILocation(line: 4, column: 3, scope: !25)
!36 = !DILocation(line: 4, column: 10, scope: !37)
!37 = distinct !DILexicalBlock(scope: !38, file: !3, line: 4, column: 3)
!38 = distinct !DILexicalBlock(scope: !25, file: !3, line: 4, column: 3)
!39 = !DILocation(line: 4, column: 3, scope: !38)
!40 = !DILocation(line: 4, column: 23, scope: !41)
!41 = distinct !DILexicalBlock(scope: !37, file: !3, line: 4, column: 23)
!42 = !DILocation(line: 4, column: 29, scope: !41)
!43 = !DILocation(line: 4, column: 26, scope: !41)
!44 = !DILocation(line: 4, column: 38, scope: !41)
!45 = !DILocation(line: 4, column: 35, scope: !41)
!46 = !DILocation(line: 4, column: 14, scope: !37)
!47 = !DILocation(line: 4, column: 3, scope: !37)
!48 = distinct !{!48, !39, !49, !50}
!49 = !DILocation(line: 4, column: 53, scope: !38)
!50 = !{!"llvm.loop.mustprogress"}
!51 = !DILocation(line: 5, column: 3, scope: !25)
!52 = !DILocation(line: 6, column: 1, scope: !25)
!53 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 9, type: !54, scopeLine: 9, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !31)
!54 = !DISubroutineType(types: !55)
!55 = !{!30}
!56 = !DILocation(line: 9, column: 28, scope: !53)
!57 = !DILocalVariable(name: "p", scope: !53, file: !3, line: 9, type: !6)
!58 = !DILocation(line: 0, scope: !53)
!59 = !DILocation(line: 9, column: 56, scope: !53)
!60 = !DILocalVariable(name: "t", scope: !53, file: !3, line: 9, type: !6)
!61 = !DILocation(line: 9, column: 84, scope: !53)
!62 = !DILocation(line: 9, column: 98, scope: !53)
!63 = !DILocation(line: 9, column: 93, scope: !53)
!64 = !DILocation(line: 9, column: 74, scope: !53)
