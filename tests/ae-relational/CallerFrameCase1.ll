; ModuleID = 'build/CallerFrameCase1.raw.ll'
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
define void @f1() #0 !dbg !24 {
  %1 = alloca i32, align 4
  %2 = call i32 @nondet_i32(), !dbg !27
    #dbg_value(i32 %2, !28, !DIExpression(), !29)
    #dbg_declare(ptr %1, !30, !DIExpression(), !31)
  %3 = icmp slt i32 %2, -1000, !dbg !32
  br i1 %3, label %6, label %4, !dbg !34

4:                                                ; preds = %0
  %5 = icmp sgt i32 %2, 1000, !dbg !35
  br i1 %5, label %6, label %7, !dbg !34

6:                                                ; preds = %4, %0
  br label %10, !dbg !36

7:                                                ; preds = %4
  store i32 %2, ptr @g, align 4, !dbg !37
  call void @shared(ptr noundef %1), !dbg !38
  %8 = load i32, ptr @g, align 4, !dbg !39
  %9 = icmp eq i32 %8, %2, !dbg !40
  call void @svf_assert(i1 noundef zeroext %9), !dbg !41
  br label %10, !dbg !42

10:                                               ; preds = %7, %6
  ret void, !dbg !42
}

declare void @svf_assert(i1 noundef zeroext) #1

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @f2() #0 !dbg !43 {
  %1 = call i32 @nondet_i32(), !dbg !44
    #dbg_value(i32 %1, !45, !DIExpression(), !46)
  %2 = icmp slt i32 %1, -1000, !dbg !47
  br i1 %2, label %5, label %3, !dbg !49

3:                                                ; preds = %0
  %4 = icmp sgt i32 %1, 1000, !dbg !50
  br i1 %4, label %5, label %6, !dbg !49

5:                                                ; preds = %3, %0
  br label %9, !dbg !51

6:                                                ; preds = %3
  store i32 %1, ptr @g, align 4, !dbg !52
  call void @shared(ptr noundef @g), !dbg !53
  %7 = load i32, ptr @g, align 4, !dbg !54
  %8 = icmp eq i32 %7, %1, !dbg !55
  call void @svf_assert(i1 noundef zeroext %8), !dbg !56
  br label %9, !dbg !57

9:                                                ; preds = %6, %5
  ret void, !dbg !57
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !58 {
  call void @f1(), !dbg !61
  call void @f2(), !dbg !62
  ret i32 0, !dbg !63
}

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
!24 = distinct !DISubprogram(name: "f1", scope: !3, file: !3, line: 10, type: !25, scopeLine: 10, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!25 = !DISubroutineType(types: !26)
!26 = !{null}
!27 = !DILocation(line: 11, column: 11, scope: !24)
!28 = !DILocalVariable(name: "a", scope: !24, file: !3, line: 11, type: !5)
!29 = !DILocation(line: 0, scope: !24)
!30 = !DILocalVariable(name: "local", scope: !24, file: !3, line: 11, type: !5)
!31 = !DILocation(line: 11, column: 25, scope: !24)
!32 = !DILocation(line: 12, column: 9, scope: !33)
!33 = distinct !DILexicalBlock(scope: !24, file: !3, line: 12, column: 7)
!34 = !DILocation(line: 12, column: 17, scope: !33)
!35 = !DILocation(line: 12, column: 22, scope: !33)
!36 = !DILocation(line: 12, column: 30, scope: !33)
!37 = !DILocation(line: 13, column: 5, scope: !24)
!38 = !DILocation(line: 13, column: 10, scope: !24)
!39 = !DILocation(line: 13, column: 37, scope: !24)
!40 = !DILocation(line: 13, column: 39, scope: !24)
!41 = !DILocation(line: 13, column: 26, scope: !24)
!42 = !DILocation(line: 14, column: 1, scope: !24)
!43 = distinct !DISubprogram(name: "f2", scope: !3, file: !3, line: 15, type: !25, scopeLine: 15, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!44 = !DILocation(line: 16, column: 11, scope: !43)
!45 = !DILocalVariable(name: "a", scope: !43, file: !3, line: 16, type: !5)
!46 = !DILocation(line: 0, scope: !43)
!47 = !DILocation(line: 17, column: 9, scope: !48)
!48 = distinct !DILexicalBlock(scope: !43, file: !3, line: 17, column: 7)
!49 = !DILocation(line: 17, column: 17, scope: !48)
!50 = !DILocation(line: 17, column: 22, scope: !48)
!51 = !DILocation(line: 17, column: 30, scope: !48)
!52 = !DILocation(line: 18, column: 5, scope: !43)
!53 = !DILocation(line: 18, column: 10, scope: !43)
!54 = !DILocation(line: 18, column: 33, scope: !43)
!55 = !DILocation(line: 18, column: 35, scope: !43)
!56 = !DILocation(line: 18, column: 22, scope: !43)
!57 = !DILocation(line: 19, column: 1, scope: !43)
!58 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 20, type: !59, scopeLine: 20, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!59 = !DISubroutineType(types: !60)
!60 = !{!5}
!61 = !DILocation(line: 20, column: 18, scope: !58)
!62 = !DILocation(line: 20, column: 24, scope: !58)
!63 = !DILocation(line: 20, column: 30, scope: !58)
