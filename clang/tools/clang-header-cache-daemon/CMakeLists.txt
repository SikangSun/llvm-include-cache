set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_tool(clang-header-cache-daemon
  ClangHeaderCacheDaemon.cpp
  )

clang_target_link_libraries(clang-header-cache-daemon
  PRIVATE
  clangASTCache
  clangBasic
  )
