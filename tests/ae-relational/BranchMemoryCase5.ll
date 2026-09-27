; ModuleID = '/tmp/branch-memory-case-5.raw.ll'
source_filename = "tests/ae-relational/BranchMemoryRefinement.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0
@q = global i32 0, align 4, !dbg !5

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define void @overwrite(i32 noundef %0) #0 !dbg !16 {
  %2 = alloca i32, align 4
  store i32 %0, ptr %2, align 4
    #dbg_declare(ptr %2, !20, !DIExpression(), !21)
  %3 = load i32, ptr %2, align 4, !dbg !22
  store i32 %3, ptr @o, align 4, !dbg !23
  ret void, !dbg !24
}

; Function Attrs: noinline nounwind optnone ssp uwtable(sync)
define i32 @main() #0 !dbg !25 {
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  store i32 0, ptr %1, align 4
    #dbg_declare(ptr %2, !28, !DIExpression(), !29)
  %5 = call i32 @nondet_i32(), !dbg !30
  store i32 %5, ptr %2, align 4, !dbg !29
    #dbg_declare(ptr %3, !31, !DIExpression(), !32)
  %6 = call i32 @nondet_i32(), !dbg !33
  store i32 %6, ptr %3, align 4, !dbg !32
  %7 = load i32, ptr %2, align 4, !dbg !34
  %8 = icmp slt i32 %7, -1000, !dbg !36
  br i1 %8, label %18, label %9, !dbg !37

9:                                                ; preds = %0
  %10 = load i32, ptr %2, align 4, !dbg !38
  %11 = icmp sgt i32 %10, 1000, !dbg !39
  br i1 %11, label %18, label %12, !dbg !40

12:                                               ; preds = %9
  %13 = load i32, ptr %3, align 4, !dbg !41
  %14 = icmp slt i32 %13, -1000, !dbg !42
  br i1 %14, label %18, label %15, !dbg !43

15:                                               ; preds = %12
  %16 = load i32, ptr %3, align 4, !dbg !44
  %17 = icmp sgt i32 %16, 1000, !dbg !45
  br i1 %17, label %18, label %19, !dbg !43

18:                                               ; preds = %15, %12, %9, %0
  store i32 0, ptr %1, align 4, !dbg !46
  br label %30, !dbg !46

19:                                               ; preds = %15
  %20 = load i32, ptr %2, align 4, !dbg !47
  store i32 %20, ptr @o, align 4, !dbg !48
  %21 = load i32, ptr %3, align 4, !dbg !49
  store i32 %21, ptr @q, align 4, !dbg !50
    #dbg_declare(ptr %4, !51, !DIExpression(), !52)
  %22 = load i32, ptr @o, align 4, !dbg !53
  store i32 %22, ptr %4, align 4, !dbg !52
  %23 = load i32, ptr %3, align 4, !dbg !54
  store i32 %23, ptr @o, align 4, !dbg !55
  %24 = load i32, ptr %4, align 4, !dbg !56
  switch i32 %24, label %28 [
    i32 0, label %25
  ], !dbg !57

25:                                               ; preds = %19
  %26 = load i32, ptr @o, align 4, !dbg !58
  %27 = icmp eq i32 %26, 0, !dbg !60
  call void @svf_assert(i1 noundef zeroext %27), !dbg !61
  br label %29, !dbg !62

28:                                               ; preds = %19
  br label %29, !dbg !63

29:                                               ; preds = %28, %25
  store i32 0, ptr %1, align 4, !dbg !64
  br label %30, !dbg !64

30:                                               ; preds = %29, %18
  %31 = load i32, ptr %1, align 4, !dbg !65
  ret i32 %31, !dbg !65
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind optnone ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!8, !9, !10, !11, !12, !13, !14}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!15}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 7, type: !7, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "tests/ae-relational/BranchMemoryRefinement.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/svf-branch-refinement", checksumkind: CSK_MD5, checksum: "721b943b97da93f26d7e471cce345fcd")
!4 = !{!0, !5}
!5 = !DIGlobalVariableExpression(var: !6, expr: !DIExpression())
!6 = distinct !DIGlobalVariable(name: "q", scope: !2, file: !3, line: 7, type: !7, isLocal: false, isDefinition: true)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!9 = !{i32 7, !"Dwarf Version", i32 5}
!10 = !{i32 2, !"Debug Info Version", i32 3}
!11 = !{i32 1, !"wchar_size", i32 4}
!12 = !{i32 8, !"PIC Level", i32 2}
!13 = !{i32 7, !"uwtable", i32 1}
!14 = !{i32 7, !"frame-pointer", i32 1}
!15 = !{!"Homebrew clang version 21.1.8"}
!16 = distinct !DISubprogram(name: "overwrite", scope: !3, file: !3, line: 8, type: !17, scopeLine: 8, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !19)
!17 = !DISubroutineType(types: !18)
!18 = !{null, !7}
!19 = !{}
!20 = !DILocalVariable(name: "v", arg: 1, scope: !16, file: !3, line: 8, type: !7)
!21 = !DILocation(line: 8, column: 46, scope: !16)
!22 = !DILocation(line: 8, column: 53, scope: !16)
!23 = !DILocation(line: 8, column: 52, scope: !16)
!24 = !DILocation(line: 8, column: 56, scope: !16)
!25 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 9, type: !26, scopeLine: 9, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !19)
!26 = !DISubroutineType(types: !27)
!27 = !{!7}
!28 = !DILocalVariable(name: "x", scope: !25, file: !3, line: 10, type: !7)
!29 = !DILocation(line: 10, column: 9, scope: !25)
!30 = !DILocation(line: 10, column: 11, scope: !25)
!31 = !DILocalVariable(name: "v", scope: !25, file: !3, line: 10, type: !7)
!32 = !DILocation(line: 10, column: 25, scope: !25)
!33 = !DILocation(line: 10, column: 27, scope: !25)
!34 = !DILocation(line: 11, column: 8, scope: !35)
!35 = distinct !DILexicalBlock(scope: !25, file: !3, line: 11, column: 8)
!36 = !DILocation(line: 11, column: 10, scope: !35)
!37 = !DILocation(line: 11, column: 18, scope: !35)
!38 = !DILocation(line: 11, column: 21, scope: !35)
!39 = !DILocation(line: 11, column: 23, scope: !35)
!40 = !DILocation(line: 11, column: 30, scope: !35)
!41 = !DILocation(line: 11, column: 33, scope: !35)
!42 = !DILocation(line: 11, column: 35, scope: !35)
!43 = !DILocation(line: 11, column: 43, scope: !35)
!44 = !DILocation(line: 11, column: 46, scope: !35)
!45 = !DILocation(line: 11, column: 48, scope: !35)
!46 = !DILocation(line: 11, column: 56, scope: !35)
!47 = !DILocation(line: 12, column: 7, scope: !25)
!48 = !DILocation(line: 12, column: 6, scope: !25)
!49 = !DILocation(line: 12, column: 12, scope: !25)
!50 = !DILocation(line: 12, column: 11, scope: !25)
!51 = !DILocalVariable(name: "t", scope: !25, file: !3, line: 34, type: !7)
!52 = !DILocation(line: 34, column: 9, scope: !25)
!53 = !DILocation(line: 34, column: 11, scope: !25)
!54 = !DILocation(line: 35, column: 7, scope: !25)
!55 = !DILocation(line: 35, column: 6, scope: !25)
!56 = !DILocation(line: 36, column: 12, scope: !25)
!57 = !DILocation(line: 36, column: 5, scope: !25)
!58 = !DILocation(line: 36, column: 36, scope: !59)
!59 = distinct !DILexicalBlock(scope: !25, file: !3, line: 36, column: 15)
!60 = !DILocation(line: 36, column: 37, scope: !59)
!61 = !DILocation(line: 36, column: 25, scope: !59)
!62 = !DILocation(line: 36, column: 43, scope: !59)
!63 = !DILocation(line: 36, column: 59, scope: !59)
!64 = !DILocation(line: 38, column: 5, scope: !25)
!65 = !DILocation(line: 39, column: 1, scope: !25)
