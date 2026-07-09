# Standalone converters built on the header-only ftr writer. They need lz4
# but neither SystemC nor the lwtr library.
if(TARGET lz4::lz4)
    add_executable(konata2ftr konata2ftr.cpp)
    target_link_libraries(konata2ftr PRIVATE ftr lz4::lz4)
    target_compile_features(konata2ftr PRIVATE cxx_std_17)
else()
    message(STATUS "tools: lz4 not available, skipping konata2ftr")
endif()
