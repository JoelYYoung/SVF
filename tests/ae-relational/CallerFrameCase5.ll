; ModuleID = 'build/CallerFrameCase5.raw.ll'
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
define void @indirect_shared() #0 !dbg !24 {
  %1 = call ptr @get_callback(), !dbg !27
  call void %1(), !dbg !27
  ret void, !dbg !28
}

declare ptr @get_callback() #1

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !29 {
  call void @indirect_shared(), !dbg !32
  call void @indirect_shared(), !dbg !33
  ret i32 0, !dbg !34
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
!24 = distinct !DISubprogram(name: "indirect_shared", scope: !3, file: !3, line: 50, type: !25, scopeLine: 50, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!25 = !DISubroutineType(types: !26)
!26 = !{null}
!27 = !DILocation(line: 50, column: 56, scope: !24)
!28 = !DILocation(line: 50, column: 74, scope: !24)
!29 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 51, type: !30, scopeLine: 51, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2)
!30 = !DISubroutineType(types: !31)
!31 = !{!5}
!32 = !DILocation(line: 51, column: 18, scope: !29)
!33 = !DILocation(line: 51, column: 37, scope: !29)
!34 = !DILocation(line: 51, column: 56, scope: !29)
