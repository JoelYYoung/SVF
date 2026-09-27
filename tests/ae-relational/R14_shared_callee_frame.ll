; ModuleID = 'r14.raw.ll'
source_filename = "r14.c"
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
define i32 @main() #0 !dbg !24 {
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = call i32 @nondet_i32(), !dbg !27
    #dbg_value(i32 %3, !28, !DIExpression(), !29)
  %4 = icmp slt i32 %3, -1000, !dbg !30
  br i1 %4, label %7, label %5, !dbg !32

5:                                                ; preds = %0
  %6 = icmp sgt i32 %3, 1000, !dbg !33
  br i1 %6, label %7, label %8, !dbg !32

7:                                                ; preds = %5, %0
  br label %13, !dbg !34

8:                                                ; preds = %5
    #dbg_declare(ptr %1, !35, !DIExpression(), !36)
    #dbg_declare(ptr %2, !37, !DIExpression(), !38)
  store i32 %3, ptr @g, align 4, !dbg !39
  call void @shared(ptr noundef %2), !dbg !40
  %9 = load i32, ptr @g, align 4, !dbg !41
  %10 = icmp eq i32 %9, %3, !dbg !42
  call void @svf_assert(i1 noundef zeroext %10), !dbg !43
  call void @shared(ptr noundef %1), !dbg !44
  %11 = load i32, ptr @g, align 4, !dbg !45
  %12 = icmp eq i32 %11, %3, !dbg !46
  call void @svf_assert(i1 noundef zeroext %12), !dbg !47
  br label %13, !dbg !48

13:                                               ; preds = %8, %7
  ret i32 0, !dbg !49
}

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "g", scope: !2, file: !3, line: 4, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "r14.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r14", checksumkind: CSK_MD5, checksum: "e58a1b01fd9db70c2834bfa4c4eadd3d")
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
!14 = distinct !DISubprogram(name: "shared", scope: !3, file: !3, line: 5, type: !15, scopeLine: 5, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!15 = !DISubroutineType(types: !16)
!16 = !{null, !17}
!17 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !5, size: 64)
!18 = !{}
!19 = !DILocalVariable(name: "q", arg: 1, scope: !14, file: !3, line: 5, type: !17)
!20 = !DILocation(line: 0, scope: !14)
!21 = !DILocation(line: 5, column: 54, scope: !14)
!22 = !DILocation(line: 5, column: 52, scope: !14)
!23 = !DILocation(line: 5, column: 68, scope: !14)
!24 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !25, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!25 = !DISubroutineType(types: !26)
!26 = !{!5}
!27 = !DILocation(line: 7, column: 11, scope: !24)
!28 = !DILocalVariable(name: "a", scope: !24, file: !3, line: 7, type: !5)
!29 = !DILocation(line: 0, scope: !24)
!30 = !DILocation(line: 8, column: 9, scope: !31)
!31 = distinct !DILexicalBlock(scope: !24, file: !3, line: 8, column: 7)
!32 = !DILocation(line: 8, column: 17, scope: !31)
!33 = !DILocation(line: 8, column: 22, scope: !31)
!34 = !DILocation(line: 8, column: 30, scope: !31)
!35 = !DILocalVariable(name: "cell", scope: !24, file: !3, line: 9, type: !5)
!36 = !DILocation(line: 9, column: 7, scope: !24)
!37 = !DILocalVariable(name: "other", scope: !24, file: !3, line: 9, type: !5)
!38 = !DILocation(line: 9, column: 13, scope: !24)
!39 = !DILocation(line: 10, column: 5, scope: !24)
!40 = !DILocation(line: 11, column: 3, scope: !24)
!41 = !DILocation(line: 12, column: 14, scope: !24)
!42 = !DILocation(line: 12, column: 16, scope: !24)
!43 = !DILocation(line: 12, column: 3, scope: !24)
!44 = !DILocation(line: 13, column: 3, scope: !24)
!45 = !DILocation(line: 14, column: 14, scope: !24)
!46 = !DILocation(line: 14, column: 16, scope: !24)
!47 = !DILocation(line: 14, column: 3, scope: !24)
!48 = !DILocation(line: 15, column: 3, scope: !24)
!49 = !DILocation(line: 16, column: 1, scope: !24)

