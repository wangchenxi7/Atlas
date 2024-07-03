add_library(paris SHARED bks_ctx.cc runtime.cc pointer.cc)
target_compile_options(paris PRIVATE -mrtm)

target_link_libraries(paris PRIVATE jemalloc)