# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Rajesh D'Monte
add_library(vox_jobs STATIC
    JobScheduler.cpp
    JobScheduler.h
)

target_link_libraries(vox_jobs PUBLIC vox_includes fmt::fmt)
vox_configure_target(vox_jobs)

if(VOX_ENABLE_JOBS)
    find_package(enkiTS CONFIG REQUIRED)
    target_link_libraries(vox_jobs PUBLIC enkiTS::enkiTS)
    target_compile_definitions(vox_jobs PUBLIC VOX_ENABLE_JOBS=1)
endif()
