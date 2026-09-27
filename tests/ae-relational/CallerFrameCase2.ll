; ModuleID = 'build/CallerFrameCase2.raw.ll'
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
define void @again(i32 noundef %0) #0 !dbg !24 {
    #dbg_value(i32 %0, !27, !DIExpression(), !28)
  %2 = icmp ne i32 %0, 0, !dbg !29
  br i1 %2, label %3, label %5, !dbg !29

3:                                                ; preds = %1
  %4 = sub nsw i32 %0, 1, !dbg !31
  call void @f(i32 noundef %4), !dbg !32
  br label %6, !dbg !32

5:                                                ; preds = %1
  store i32 99, ptr @g, align 4, !dbg !33
  br label %6

6:                                                ; preds = %5, %3
  ret void, !dbg !34
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define void @f(i32 noundef %0) #0 !dbg !35 {
    #dbg_value(i32 %0, !36, !DIExpression(), !37)
  call void @again(i32 noundef %0), !dbg !38
  call void @again(i32 noundef %0), !dbg !39
  ret void, !dbg !40
}

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !41 {
  store i32 1, ptr @g, align 4, !dbg !44
  call void @f(i32 noundef 1), !dbg !45
  %1 = load i32, ptr @g, align 4, !dbg !46
  %2 = icmp eq i32 %1, 1, !dbg !47
  call void @svf_assert(i1 noundef zeroext %2), !dbg !48
  ret i32 0, !dbg !49
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
!24 = distinct !DISubprogram(name: "again", scope: !3, file: !3, line: 25, type: !25, scopeLine: 25, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!25 = !DISubroutineType(types: !26)
!26 = !{null, !5}
!27 = !DILocalVariable(name: "n", arg: 1, scope: !24, file: !3, line: 25, type: !5)
!28 = !DILocation(line: 0, scope: !24)
!29 = !DILocation(line: 25, column: 51, scope: !30)
!30 = distinct !DILexicalBlock(scope: !24, file: !3, line: 25, column: 51)
!31 = !DILocation(line: 25, column: 58, scope: !30)
!32 = !DILocation(line: 25, column: 54, scope: !30)
!33 = !DILocation(line: 25, column: 71, scope: !30)
!34 = !DILocation(line: 25, column: 77, scope: !24)
!35 = distinct !DISubprogram(name: "f", scope: !3, file: !3, line: 26, type: !25, scopeLine: 26, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !18)
!36 = !DILocalVariable(name: "n", arg: 1, scope: !35, file: !3, line: 26, type: !5)
!37 = !DILocation(line: 0, scope: !35)
!38 = !DILocation(line: 26, column: 43, scope: !35)
!39 = !DILocation(line: 26, column: 53, scope: !35)
!40 = !DILocation(line: 26, column: 63, scope: !35)
!41 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 27, type: !42, scopeLine: 27, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!42 = !DISubroutineType(types: !43)
!43 = !{!5}
!44 = !DILocation(line: 27, column: 20, scope: !41)
!45 = !DILocation(line: 27, column: 25, scope: !41)
!46 = !DILocation(line: 27, column: 42, scope: !41)
!47 = !DILocation(line: 27, column: 44, scope: !41)
!48 = !DILocation(line: 27, column: 31, scope: !41)
!49 = !DILocation(line: 27, column: 51, scope: !41)
