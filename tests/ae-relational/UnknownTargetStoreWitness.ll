; ModuleID = 'R05_unknown_target_store.raw.ll'
source_filename = "R05_unknown_target_store.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !19 {
  store i32 1, ptr @o, align 4, !dbg !23
  %1 = call i32 @nondet_i32(), !dbg !24
  %2 = sext i32 %1 to i64, !dbg !25
  %3 = inttoptr i64 %2 to ptr, !dbg !26
    #dbg_value(ptr %3, !27, !DIExpression(), !28)
  store i32 7, ptr %3, align 4, !dbg !29
  %4 = load i32, ptr @o, align 4, !dbg !30
    #dbg_value(i32 %4, !31, !DIExpression(), !28)
  %5 = icmp eq i32 %4, 1, !dbg !32
  call void @svf_assert(i1 noundef zeroext %5), !dbg !33
  ret i32 0, !dbg !34
}

declare i32 @nondet_i32() #1

declare void @svf_assert(i1 noundef zeroext) #1

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!11, !12, !13, !14, !15, !16, !17}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!18}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 5, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !4, globals: !10, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "R05_unknown_target_store.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/supervisor-sparse-fixtures/review-probes", checksumkind: CSK_MD5, checksum: "6986c920375a78bc20838dee2001e6db")
!4 = !{!5, !7}
!5 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !6, size: 64)
!6 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!7 = !DIDerivedType(tag: DW_TAG_typedef, name: "uintptr_t", file: !8, line: 34, baseType: !9)
!8 = !DIFile(filename: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk/usr/include/sys/_types/_uintptr_t.h", directory: "", checksumkind: CSK_MD5, checksum: "e70ae655dd1b9d4ae0b1dcc073f5b7e4")
!9 = !DIBasicType(name: "unsigned long", size: 64, encoding: DW_ATE_unsigned)
!10 = !{!0}
!11 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!12 = !{i32 7, !"Dwarf Version", i32 5}
!13 = !{i32 2, !"Debug Info Version", i32 3}
!14 = !{i32 1, !"wchar_size", i32 4}
!15 = !{i32 8, !"PIC Level", i32 2}
!16 = !{i32 7, !"uwtable", i32 1}
!17 = !{i32 7, !"frame-pointer", i32 1}
!18 = !{!"Homebrew clang version 21.1.8"}
!19 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !20, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !22)
!20 = !DISubroutineType(types: !21)
!21 = !{!6}
!22 = !{}
!23 = !DILocation(line: 7, column: 5, scope: !19)
!24 = !DILocation(line: 8, column: 30, scope: !19)
!25 = !DILocation(line: 8, column: 19, scope: !19)
!26 = !DILocation(line: 8, column: 12, scope: !19)
!27 = !DILocalVariable(name: "p", scope: !19, file: !3, line: 8, type: !5)
!28 = !DILocation(line: 0, scope: !19)
!29 = !DILocation(line: 9, column: 6, scope: !19)
!30 = !DILocation(line: 10, column: 11, scope: !19)
!31 = !DILocalVariable(name: "y", scope: !19, file: !3, line: 10, type: !6)
!32 = !DILocation(line: 11, column: 16, scope: !19)
!33 = !DILocation(line: 11, column: 3, scope: !19)
!34 = !DILocation(line: 12, column: 3, scope: !19)
