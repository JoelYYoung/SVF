; ModuleID = 'build/CallerFrameCase3.raw.ll'
source_filename = "tests/ae-relational/CallerFrameCases.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@g = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @shared(ptr noundef %0) #0 !dbg !14 {
    #dbg_value(ptr %0, !19, !DIExpression(), !20)
  %2 = call i32 @nondet_i32(), !dbg !21
  store i32 %2, ptr %0, align 4, !dbg !22
  ret void, !dbg !23
}

declare i32 @nondet_i32() #1

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @other(ptr noundef %0) #0 !dbg !24 {
    #dbg_value(ptr %0, !25, !DIExpression(), !26)
  %2 = call i32 @nondet_i32(), !dbg !27
  store i32 %2, ptr %0, align 4, !dbg !28
  ret void, !dbg !29
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @dispatch(ptr noundef %0, i1 noundef zeroext %1) #0 !dbg !30 {
    #dbg_value(ptr %0, !34, !DIExpression(), !35)
  %3 = zext i1 %1 to i8
    #dbg_value(i8 %3, !36, !DIExpression(), !35)
  %4 = trunc i8 %3 to i1, !dbg !37
  %5 = zext i1 %4 to i64, !dbg !37
  %6 = select i1 %4, ptr @shared, ptr @other, !dbg !37
    #dbg_value(ptr %6, !38, !DIExpression(), !35)
  call void %6(ptr noundef %0), !dbg !40
  ret void, !dbg !41
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !42 {
  %1 = alloca i32, align 4
  %2 = call i32 @nondet_i32(), !dbg !45
    #dbg_value(i32 %2, !46, !DIExpression(), !47)
    #dbg_declare(ptr %1, !48, !DIExpression(), !49)
  %3 = icmp slt i32 %2, -1000, !dbg !50
  br i1 %3, label %6, label %4, !dbg !52

4:                                                ; preds = %0
  %5 = icmp sgt i32 %2, 1000, !dbg !53
  br i1 %5, label %6, label %7, !dbg !52

6:                                                ; preds = %4, %0
  br label %12, !dbg !54

7:                                                ; preds = %4
  store i32 %2, ptr @g, align 4, !dbg !55
  call void @dispatch(ptr noundef %1, i1 noundef zeroext true), !dbg !56
  %8 = load i32, ptr @g, align 4, !dbg !57
  %9 = icmp eq i32 %8, %2, !dbg !58
  call void @svf_assert(i1 noundef zeroext %9), !dbg !59
  call void @dispatch(ptr noundef @g, i1 noundef zeroext false), !dbg !60
  %10 = load i32, ptr @g, align 4, !dbg !61
  %11 = icmp eq i32 %10, %2, !dbg !62
  call void @svf_assert(i1 noundef zeroext %11), !dbg !63
  br label %12, !dbg !64

12:                                               ; preds = %7, %6
  ret i32 0, !dbg !65
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 7, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "tests/ae-relational/CallerFrameCases.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-caller-frame", checksumkind: CSK_MD5, checksum: "6f5d662442735a6bf102567d8348067b")
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
!14 = distinct !DISubprogram(name: "shared", scope: !3, file: !3, line: 8, type: !15, scopeLine: 8, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!15 = !DISubroutineType(types: !16)
!16 = !{null, !17}
!17 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !5, size: 64)
!18 = !{}
!19 = !DILocalVariable(name: "q", arg: 1, scope: !14, file: !3, line: 8, type: !17)
!20 = !DILocation(line: 0, scope: !14)
!21 = !DILocation(line: 8, column: 54, scope: !14)
!22 = !DILocation(line: 8, column: 52, scope: !14)
!23 = !DILocation(line: 8, column: 68, scope: !14)
!24 = distinct !DISubprogram(name: "other", scope: !3, file: !3, line: 30, type: !15, scopeLine: 30, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!25 = !DILocalVariable(name: "p", arg: 1, scope: !24, file: !3, line: 30, type: !17)
!26 = !DILocation(line: 0, scope: !24)
!27 = !DILocation(line: 30, column: 53, scope: !24)
!28 = !DILocation(line: 30, column: 51, scope: !24)
!29 = !DILocation(line: 30, column: 67, scope: !24)
!30 = distinct !DISubprogram(name: "dispatch", scope: !3, file: !3, line: 31, type: !31, scopeLine: 31, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!31 = !DISubroutineType(types: !32)
!32 = !{null, !17, !33}
!33 = !DIBasicType(name: "_Bool", size: 8, encoding: DW_ATE_boolean)
!34 = !DILocalVariable(name: "p", arg: 1, scope: !30, file: !3, line: 31, type: !17)
!35 = !DILocation(line: 0, scope: !30)
!36 = !DILocalVariable(name: "c", arg: 2, scope: !30, file: !3, line: 31, type: !33)
!37 = !DILocation(line: 32, column: 23, scope: !30)
!38 = !DILocalVariable(name: "fn", scope: !30, file: !3, line: 32, type: !39)
!39 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !15, size: 64)
!40 = !DILocation(line: 32, column: 43, scope: !30)
!41 = !DILocation(line: 33, column: 1, scope: !30)
!42 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 34, type: !43, scopeLine: 34, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!43 = !DISubroutineType(types: !44)
!44 = !{!5}
!45 = !DILocation(line: 35, column: 11, scope: !42)
!46 = !DILocalVariable(name: "a", scope: !42, file: !3, line: 35, type: !5)
!47 = !DILocation(line: 0, scope: !42)
!48 = !DILocalVariable(name: "local", scope: !42, file: !3, line: 35, type: !5)
!49 = !DILocation(line: 35, column: 25, scope: !42)
!50 = !DILocation(line: 36, column: 9, scope: !51)
!51 = distinct !DILexicalBlock(scope: !42, file: !3, line: 36, column: 7)
!52 = !DILocation(line: 36, column: 17, scope: !51)
!53 = !DILocation(line: 36, column: 22, scope: !51)
!54 = !DILocation(line: 36, column: 30, scope: !51)
!55 = !DILocation(line: 37, column: 5, scope: !42)
!56 = !DILocation(line: 37, column: 10, scope: !42)
!57 = !DILocation(line: 37, column: 45, scope: !42)
!58 = !DILocation(line: 37, column: 47, scope: !42)
!59 = !DILocation(line: 37, column: 34, scope: !42)
!60 = !DILocation(line: 38, column: 3, scope: !42)
!61 = !DILocation(line: 38, column: 35, scope: !42)
!62 = !DILocation(line: 38, column: 37, scope: !42)
!63 = !DILocation(line: 38, column: 24, scope: !42)
!64 = !DILocation(line: 39, column: 3, scope: !42)
!65 = !DILocation(line: 40, column: 1, scope: !42)
