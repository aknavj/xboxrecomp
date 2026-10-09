cmake_minimum_required(VERSION 3.20)
project(apu_mixdown_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_mixdown_test test_main.c)
target_include_directories(apu_mixdown_test PRIVATE ../../src ../../src/apu)
target_link_libraries(apu_mixdown_test PRIVATE xbox_apu)
enable_testing()
add_executable(apu_voice_restart_test test_voice_restart.c)
target_include_directories(apu_voice_restart_test PRIVATE ../../src ../../src/apu)
target_link_libraries(apu_voice_restart_test PRIVATE xbox_apu)
add_test(NAME apu_voice_restart COMMAND apu_voice_restart_test)
set_tests_properties(apu_voice_restart PROPERTIES ENVIRONMENT "RECOMP_APU_DIAG=0")
add_test(NAME apu_voice_restart_diagnostics COMMAND apu_voice_restart_test)
set_tests_properties(apu_voice_restart_diagnostics PROPERTIES ENVIRONMENT "RECOMP_APU_DIAG=1")
add_test(NAME apu_dsp_dma COMMAND apu_voice_restart_test --dsp-dma)
set_tests_properties(apu_dsp_dma PROPERTIES
    ENVIRONMENT "RECOMP_APU_DIAG=0;RECOMP_APU_DSP_ACK=gp:0x810,ep:0x1810")
add_test(NAME apu_diagnostics_off COMMAND apu_voice_restart_test --diagnostics 0)
set_tests_properties(apu_diagnostics_off PROPERTIES ENVIRONMENT "RECOMP_APU_DIAG=0")
add_test(NAME apu_diagnostics_on COMMAND apu_voice_restart_test --diagnostics 1)
set_tests_properties(apu_diagnostics_on PROPERTIES ENVIRONMENT "RECOMP_APU_DIAG=1")
add_test(NAME apu_diagnostics_unset COMMAND ${CMAKE_COMMAND} -E env
    --unset=RECOMP_APU_DIAG $<TARGET_FILE:apu_voice_restart_test> --diagnostics 0)
# Registered TWICE, on purpose. mcpx_apu_mixdown_all caches its getenv in a
# static, so one process can only ever exercise one arm -- and an arm that
# cannot fail is not a test. The off arm is the negative control: it must show
# the effect bins really are discarded without this change, or the on arm
# proves nothing.
add_test(NAME apu_mixdown_all COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_all PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN_ALL=1;APU_MIXDOWN_EXPECT=1")
add_test(NAME apu_mixdown_two_bins COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_two_bins PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN_ALL=0;APU_MIXDOWN_EXPECT=0")
