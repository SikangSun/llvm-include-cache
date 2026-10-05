set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangASTCache
  ASTCacheDependencyTracker.cpp
  ASTCacheProducer.cpp
  CacheClient.cpp
  CacheDaemon.cpp
  CacheProtocol.cpp
  DeclFingerprint.cpp
  DependencySerialization.cpp
  HeaderCacheServer.cpp
  MacroTokens.cpp
  PartExporter.cpp
  PragmaStateCheck.cpp
  SharedRegion.cpp

  LINK_LIBS
  clangAST
  clangBasic
  clangDriver
  clangOptions
  clangFrontend
  clangLex
  clangSema
  clangSerialization
  )
