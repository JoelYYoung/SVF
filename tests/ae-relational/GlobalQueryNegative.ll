; ModuleID = 'r21.raw.ll'
source_filename = "r21.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

%struct.S = type { i32, i32 }

@__const.main.arr = private unnamed_addr constant [4 x i32] [i32 0, i32 1, i32 2, i32 3], align 4

; Function Attrs: noinline nounwind ssp uwtable(sync)
define i32 @main() #0 !dbg !17 {
  %1 = alloca [4 x i32], align 4
    #dbg_declare(ptr %1, !21, !DIExpression(), !25)
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 %1, ptr align 4 @__const.main.arr, i64 16, i1 false), !dbg !25
  %2 = call i32 @nondet_i32(), !dbg !26
    #dbg_value(i32 %2, !27, !DIExpression(), !28)
  %3 = icmp ne i32 %2, -1, !dbg !29
  br i1 %3, label %4, label %5, !dbg !29

4:                                                ; preds = %0
  br label %16, !dbg !31

5:                                                ; preds = %0
  %6 = sext i32 %2 to i64, !dbg !32
  %7 = getelementptr inbounds [4 x i32], ptr %1, i64 0, i64 %6, !dbg !32
  %8 = load i32, ptr %7, align 4, !dbg !32
    #dbg_value(i32 %8, !33, !DIExpression(), !28)
  %9 = call i32 @nondet_i32(), !dbg !34
  %10 = icmp ne i32 %9, 0, !dbg !34
  %11 = zext i1 %10 to i64, !dbg !34
  %12 = select i1 %10, ptr null, ptr null, !dbg !34
    #dbg_value(ptr %12, !35, !DIExpression(), !28)
  %13 = getelementptr inbounds nuw %struct.S, ptr %12, i32 0, i32 1, !dbg !36
  %14 = load i32, ptr %13, align 4, !dbg !36
    #dbg_value(i32 %14, !37, !DIExpression(), !28)
  %15 = add nsw i32 %8, %14, !dbg !38
  br label %16, !dbg !39

16:                                               ; preds = %5, %4
  %.0 = phi i32 [ 0, %4 ], [ %15, %5 ], !dbg !28
  ret i32 %.0, !dbg !40
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: readwrite)
declare void @llvm.memcpy.p0.p0.i64(ptr noalias writeonly captures(none), ptr noalias readonly captures(none), i64, i1 immarg) #1

declare i32 @nondet_i32() #2

attributes #0 = { noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { nocallback nofree nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6}
!llvm.dbg.cu = !{!7}
!llvm.ident = !{!16}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!1 = !{i32 7, !"Dwarf Version", i32 5}
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = !{i32 1, !"wchar_size", i32 4}
!4 = !{i32 8, !"PIC Level", i32 2}
!5 = !{i32 7, !"uwtable", i32 1}
!6 = !{i32 7, !"frame-pointer", i32 1}
!7 = distinct !DICompileUnit(language: DW_LANG_C11, file: !8, producer: "Homebrew clang version 21.1.8", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, retainedTypes: !9, splitDebugInlining: false, nameTableKind: Apple, sysroot: "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk", sdk: "MacOSX26.sdk")
!8 = !DIFile(filename: "r21.c", directory: "/private/tmp/claude-501/-Users-xavier-Research-agentic-progressive-analysis/36c4c7d0-6412-4a23-8e11-4ead50b22e34/scratchpad/r21", checksumkind: CSK_MD5, checksum: "0bedd6c824d6c4705dca557842045a85")
!9 = !{!10}
!10 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !11, size: 64)
!11 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "S", file: !8, line: 5, size: 64, elements: !12)
!12 = !{!13, !15}
!13 = !DIDerivedType(tag: DW_TAG_member, name: "a", scope: !11, file: !8, line: 5, baseType: !14, size: 32)
!14 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!15 = !DIDerivedType(tag: DW_TAG_member, name: "b", scope: !11, file: !8, line: 5, baseType: !14, size: 32, offset: 32)
!16 = !{!"Homebrew clang version 21.1.8"}
!17 = distinct !DISubprogram(name: "main", scope: !8, file: !8, line: 6, type: !18, scopeLine: 6, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !7, retainedNodes: !20)
!18 = !DISubroutineType(types: !19)
!19 = !{!14}
!20 = !{}
!21 = !DILocalVariable(name: "arr", scope: !17, file: !8, line: 7, type: !22)
!22 = !DICompositeType(tag: DW_TAG_array_type, baseType: !14, size: 128, elements: !23)
!23 = !{!24}
!24 = !DISubrange(count: 4)
!25 = !DILocation(line: 7, column: 7, scope: !17)
!26 = !DILocation(line: 8, column: 11, scope: !17)
!27 = !DILocalVariable(name: "i", scope: !17, file: !8, line: 8, type: !14)
!28 = !DILocation(line: 0, scope: !17)
!29 = !DILocation(line: 9, column: 9, scope: !30)
!30 = distinct !DILexicalBlock(scope: !17, file: !8, line: 9, column: 7)
!31 = !DILocation(line: 9, column: 16, scope: !30)
!32 = !DILocation(line: 10, column: 11, scope: !17)
!33 = !DILocalVariable(name: "x", scope: !17, file: !8, line: 10, type: !14)
!34 = !DILocation(line: 11, column: 17, scope: !17)
!35 = !DILocalVariable(name: "p", scope: !17, file: !8, line: 11, type: !10)
!36 = !DILocation(line: 12, column: 14, scope: !17)
!37 = !DILocalVariable(name: "y", scope: !17, file: !8, line: 12, type: !14)
!38 = !DILocation(line: 13, column: 12, scope: !17)
!39 = !DILocation(line: 13, column: 3, scope: !17)
!40 = !DILocation(line: 14, column: 1, scope: !17)
