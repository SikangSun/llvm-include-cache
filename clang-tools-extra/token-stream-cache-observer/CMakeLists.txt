add_subdirectory(tool)

set(LLVM_EXPORTED_SYMBOL_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/TokenStreamCacheObserverPlugin.exports)

add_llvm_library(TokenStreamCacheObserverPlugin MODULE
  Plugin.cpp

  PLUGIN_TOOL clang
  )

# clangCacheObserver and clangToolingSyntax are not part of a stock clang
# binary's own link closure (unlike clangAST/clangBasic/clangFrontend/
# clangLex, which clang itself already depends on), so -- unlike those --
# they must be linked into the plugin unconditionally, not just on
# WIN32/CYGWIN, or symbol resolution at -Xclang -load time fails.
clang_target_link_libraries(TokenStreamCacheObserverPlugin PRIVATE
  clangCacheObserver
  clangToolingSyntax
  )

if(WIN32 OR CYGWIN)
  set(LLVM_LINK_COMPONENTS
    Support
    )
  clang_target_link_libraries(TokenStreamCacheObserverPlugin PRIVATE
    clangAST
    clangBasic
    clangFrontend
    clangLex
    )
endif()
