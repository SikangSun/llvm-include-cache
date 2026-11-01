"""Sums psum.py output (plain, link columns, one sample per 100,000
instructions) into kinds of work: the module reader (load), replaying a
hit's macros, checks, dependency tracking, the writer, memory, and the rest
(parsing). usage: cats.py psum-output... (one per project); prints G."""
import re, sys
K = [
 ('replay', r'replayMacroEffects|MacroEffectView|::replay'),
 ('check', r'matchesLiveState|NameHashView|MetadataView|ByteReader|linksToLoad|mergesHere|tryServe|lookupAtFileScope|'
           r'warningFlagsHash|HeaderCacheServer|DeclFingerprint|ast_cache::fingerprint|StmtProfiler|ODRHash|DependencyView|'
           r'DeclarationView|ast_cache::.*View|CacheClient|HeaderCacheClient'),
 ('track', r'ASTCacheDependencyTracker|SyntacticDependency|recordSyntactic|foldServed|expansionCandidate|DependencyTracker'),
 ('write', r'ASTWriter|ASTDeclWriter|ASTRecordWriter|ASTStmtWriter|ASTTypeWriter|BitstreamWriter|OnDiskChainedHashTableGenerator|'
           r'PartExporter|Exporter::|encodeMetadata|ByteWriter|serializeMacroTokens|ZSTD_|HeaderDependencySet|XXH3_hashLong|'
           r'ASTIdentifierTableTrait|ASTDeclContextNameLookupTrait|HeaderFileInfoTrait::Emit|LazyASTUnresolvedSet|TypeLocWriter|'
           r'DataStreamBasicWriter|BasicWriterBase'),
 ('load', r'ASTReader|ASTDeclReader|ASTRecordReader|ASTStmtReader|TypeLocReader|BitstreamCursor|readULEB|readAbbreviatedField|'
          r'ModuleManager|serialization::reader|OnDisk|MultiOnDiskHashTable|ModuleFile|LazyGenerationalUpdatePtr|'
          r'^bool llvm::function_ref<bool|GlobalModuleIndex|DataStreamBasicReader|BasicReaderBase|ASTSelectorLookupTrait|'
          r'NameSlot|ExternalASTMerger|ASTIdentifierLookupTrait|clang::serialization::'),
 ('memory', r'malloc|_int_free|libc_free|cfree|operator new|operator delete|memcpy|memmove|memset|realloc|basic_string|'
            r'_M_construct|_M_append|_M_dispose|_M_replace|_M_mutate|_M_create'),
]
R = [(k, re.compile(p)) for k, p in K]
order = ['load', 'replay', 'check', 'track', 'write', 'memory', 'rest']
print(f"{'project':9s} {'G instr':>8s} " + ' '.join(f'{k:>14s}' for k in order))
for path in sys.argv[1:]:
    tot = {k: [0, 0] for k in order}
    sums = None
    for line in open(path):
        if line.startswith('samples:'):
            sums = [int(x) for x in line.split()[1:]]; continue
        m = re.match(r'\s*(\d+)\s+(\d+)\s+[+-]\d+\s+(.*)', line)
        if not m: continue
        a, b, name = int(m[1]), int(m[2]), m[3]
        k = next((k for k, r in R if r.search(name)), 'rest')
        tot[k][0] += a; tot[k][1] += b
    p = path.rsplit('/', 1)[-1].split('.')[0]
    g = lambda n: n * 1e5 / 1e9
    print(f"{p:9s} {g(sums[0]):5.1f}>{g(sums[1]):5.1f} " +
          ' '.join(f'{g(tot[k][0]):6.1f}>{g(tot[k][1]):6.1f}' for k in order))
