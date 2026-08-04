add_llvm_executable(ae ae.cpp)
add_llvm_executable(relational-domain-eval relational-domain-eval.cpp)

if(SVF_ENABLE_APRON)
  add_llvm_executable(fp-backend-precision-eval fp-backend-precision-eval.cpp)
  target_link_libraries(fp-backend-precision-eval PRIVATE z3::libz3 Apron::Apron)
endif()
