set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangASTCache
  ASTCacheDependencyTracker.cpp
  ASTCacheProducer.cpp
  CacheClient.cpp
  CacheDaemon.cpp
  CacheProtocol.cpp
  DependencySerialization.cpp
  MacroTokens.cpp
  PragmaStateCheck.cpp
  SharedRegion.cpp

  LINK_LIBS
  clangBasic
  clangDriver
  clangOptions
  clangFrontend
  clangLex
  clangSema
  clangSerialization
  )
