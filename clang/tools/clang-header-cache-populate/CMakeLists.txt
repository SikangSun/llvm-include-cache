set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_tool(clang-header-cache-populate
  ClangHeaderCachePopulate.cpp
  )

clang_target_link_libraries(clang-header-cache-populate
  PRIVATE
  clangASTCache
  clangBasic
  )
