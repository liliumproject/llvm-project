set( LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  Core
  LineEditor
  Option
  OrcJIT
  OrcShared
  Support
  TargetParser
  )

add_clang_tool(clang-repl
  ClangRepl.cpp

  EXPORT_SYMBOLS
  )

if(MSVC)
  set_target_properties(clang-repl PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS 1)

  # RTTI/C++ symbols
  set(clang_repl_exports ${clang_repl_exports} ??_7type_info@@6B@,DATA
    ?__type_info_root_node@@3U__type_info_node@@A,DATA
    ?nothrow@std@@3Unothrow_t@1@B,DATA
  )

  # Compiler added symbols for static variables. NOT for VStudio < 2015
  set(clang_repl_exports ${clang_repl_exports} _Init_thread_abort _Init_thread_epoch,DATA
    _Init_thread_footer _Init_thread_header _tls_index,DATA
  )

  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # new/delete variants needed when linking to static msvc runtime (esp. Debug)
    set(clang_repl_exports ${clang_repl_exports}
      ??2@YAPEAX_K@Z
      ??3@YAXPEAX@Z
      ??_U@YAPEAX_K@Z
      ??_V@YAXPEAX@Z
      ??3@YAXPEAX_K@Z
    )
  else()
    set(clang_repl_exports ${clang_repl_exports}
      ??2@YAPAXI@Z
      ??3@YAXPAX@Z
      ??3@YAXPAXI@Z
      ??_U@YAPAXI@Z
      ??_V@YAXPAX@Z
      ??_V@YAXPAXI@Z
    )
  endif()

  # List to '/EXPORT:sym0 /EXPORT:sym1 /EXPORT:sym2 ...'
  # The 'SHELL' prefix tells CMake to use a space instead of comma as the
  # separator between the driver and linker options, which we need since MSVC's
  # linker uses `,DATA` as a suffix to indicate that data is being exported.
  list(TRANSFORM clang_repl_exports PREPEND "LINKER:SHELL:/EXPORT:")

  set_property(TARGET clang-repl APPEND PROPERTY LINK_OPTIONS ${clang_repl_exports})

endif(MSVC)

clang_target_link_libraries(clang-repl PRIVATE
  clangAST
  clangBasic
  clangFrontend
  clangInterpreter
  )

# The clang-repl binary can get huge with static linking in debug mode.
# Some 32-bit targets use PLT slots with limited branch range by default and we
# start to exceed this limit, e.g. when linking for arm-linux-gnueabihf with
# gold. This flag tells the linker to build a PLT for the full address range.
# Linkers without this flag are assumed to support proper PLTs by default.
set(flag_long_plt "LINKER:--long-plt")
check_linker_flag(CXX ${flag_long_plt} HAVE_LINKER_FLAG_LONG_PLT)
if(HAVE_LINKER_FLAG_LONG_PLT)
  target_link_options(clang-repl PRIVATE ${flag_long_plt})
endif()
