cmake_minimum_required(VERSION 3.20)

add_executable(lwtr_example lwtr_example.cpp)
target_link_libraries (lwtr_example lwtr reporting)

add_test(NAME lwtr_example_test COMMAND lwtr_example)

add_executable(ftr_example ftr_example.cpp)
target_link_libraries (ftr_example ftr)
