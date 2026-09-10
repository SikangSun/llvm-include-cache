set(LLVM_LINK_COMPONENTS
  Support
  Option
  )

add_clang_tool(cache-observer-test-tool
  CacheObserverTestTool.cpp

  DEPENDS
  ClangDriverOptions
  )

clang_target_link_libraries(cache-observer-test-tool
  PRIVATE
  clangAST
  clangBasic
  clangCacheObserver
  clangFrontend
  clangLex
  clangTooling
  clangToolingSyntax
  )
