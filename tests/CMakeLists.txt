list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
include(Catch)

set(DOLL_TEST_LIBS doll_contracts Catch2::Catch2WithMain)
set(DOLL_WHITEBOX_INCLUDES
    ${CMAKE_SOURCE_DIR}/src/01-dispatcher
    ${CMAKE_SOURCE_DIR}/src/04-timer)

add_executable(test_with unit/test_with.cc)
target_link_libraries(test_with PRIVATE ${DOLL_TEST_LIBS})
catch_discover_tests(test_with)

add_executable(test_contracts unit/test_contracts.cc)
target_link_libraries(test_contracts PRIVATE ${DOLL_TEST_LIBS})
catch_discover_tests(test_contracts)

add_executable(test_disp_pure unit/test_disp_pure.cc)
target_link_libraries(test_disp_pure PRIVATE ${DOLL_TEST_LIBS} disp)
target_include_directories(test_disp_pure PRIVATE ${DOLL_WHITEBOX_INCLUDES})
catch_discover_tests(test_disp_pure)

add_executable(test_disp_mech mech/test_disp_mech.cc)
target_link_libraries(test_disp_mech PRIVATE ${DOLL_TEST_LIBS} disp)
target_include_directories(test_disp_mech PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}                # common/fixture.h
    ${DOLL_WHITEBOX_INCLUDES})
catch_discover_tests(test_disp_mech)

add_executable(test_tmr_pure unit/test_tmr_pure.cc)
target_link_libraries(test_tmr_pure PRIVATE ${DOLL_TEST_LIBS} tmr)
target_include_directories(test_tmr_pure PRIVATE ${DOLL_WHITEBOX_INCLUDES})
catch_discover_tests(test_tmr_pure)

add_executable(test_tmr_mech mech/test_tmr_mech.cc)
target_link_libraries(test_tmr_mech PRIVATE ${DOLL_TEST_LIBS} tmr)
target_include_directories(test_tmr_mech PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${DOLL_WHITEBOX_INCLUDES})
catch_discover_tests(test_tmr_mech)
