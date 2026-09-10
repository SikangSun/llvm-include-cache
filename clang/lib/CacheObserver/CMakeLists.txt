set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangCacheObserver
  CacheWireFormat.cpp
  CacheSocketClient.cpp
  MacroStateDigest.cpp
  TokenStreamCacheObserver.cpp

  LINK_LIBS
  clangBasic
  clangLex
  clangToolingSyntax
  )
