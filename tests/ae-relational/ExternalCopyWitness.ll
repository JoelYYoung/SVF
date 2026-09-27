; ModuleID = 'R06_external_write.raw.ll'
source_filename = "R06_external_write.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@o = global i32 0, align 4, !dbg !0

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !14 {
  %1 = alloca i32, align 4
  store i32 1, ptr @o, align 4, !dbg !18
    #dbg_declare(ptr %1, !19, !DIExpression(), !20)
  store i32 7, ptr %1, align 4, !dbg !20
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 @o, ptr align 4 %1, i64 4, i1 false), !dbg !21
  %2 = load i32, ptr @o, align 4, !dbg !22
    #dbg_value(i32 %2, !23, !DIExpression(), !24)
  %3 = icmp eq i32 %2, 1, !dbg !25
  call void @svf_assert(i1 noundef zeroext %3), !dbg !26
  %4 = icmp eq i32 %2, 7, !dbg !27
  call void @svf_assert(i1 noundef zeroext %4), !dbg !28
  ret i32 0, !dbg !29
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #1

declare void @svf_assert(i1 noundef zeroext) #2

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!6, !7, !8, !9, !10, !11, !12}
!llvm.dbg.cu = !{!2}
!llvm.ident = !{!13}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "o", scope: !2, file: !3, line: 5, type: !5, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C11, file: !3, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, globals: !4, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!3 = !DIFile(filename: "R06_external_write.c", directory: "/Users/xavier/Projects/svf-relational-ai-20260921/supervisor-sparse-fixtures/review-probes", checksumkind: CSK_MD5, checksum: "c9c34821ce187a72e1be5b7b13ba2eae")
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
!14 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 6, type: !15, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !2, retainedNodes: !17)
!15 = !DISubroutineType(types: !16)
!16 = !{!5}
!17 = !{}
!18 = !DILocation(line: 7, column: 5, scope: !14)
!19 = !DILocalVariable(name: "v", scope: !14, file: !3, line: 8, type: !5)
!20 = !DILocation(line: 8, column: 7, scope: !14)
!21 = !DILocation(line: 9, column: 3, scope: !14)
!22 = !DILocation(line: 10, column: 11, scope: !14)
!23 = !DILocalVariable(name: "y", scope: !14, file: !3, line: 10, type: !5)
!24 = !DILocation(line: 0, scope: !14)
!25 = !DILocation(line: 11, column: 16, scope: !14)
!26 = !DILocation(line: 11, column: 3, scope: !14)
!27 = !DILocation(line: 12, column: 16, scope: !14)
!28 = !DILocation(line: 12, column: 3, scope: !14)
!29 = !DILocation(line: 13, column: 3, scope: !14)
