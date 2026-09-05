# Each consumer must build its PCH with its own compile definitions and options.
# Sharing one binary PCH across targets can change dependency ABI or macro state.
add_library(use_pch INTERFACE)

# fmt/format.h is included in the PCH.
target_link_libraries(use_pch INTERFACE fmt::fmt)
target_precompile_headers(use_pch INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/pch.h>"
)
