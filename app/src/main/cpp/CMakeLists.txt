cmake_minimum_required(VERSION 3.22)
project(somniel C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)

# SDL3 is built from source so the exact version stays pinned in the tree.
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/SDL SDL3 EXCLUDE_FROM_ALL)

# GPU drivers: Vulkan is called through volk's function pointers, loaded either from the system libvulkan or from a
# libadrenotools handle that loads a custom driver (Turnip, other Qualcomm builds) into our own linker namespace.
# libadrenotools' hook libraries (libmain_hook.so, libhook_impl.so, ...) must be packaged and extracted to the
# nativeLibraryDir (app/build.gradle: useLegacyPackaging true).
set(TP ${CMAKE_CURRENT_LIST_DIR}/../../../../third_party)
add_subdirectory(${TP}/libadrenotools adrenotools)
add_library(volk STATIC ${TP}/volk/volk.c)
target_include_directories(volk PUBLIC ${TP}/volk)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
# every file that uses Vulkan includes <vulkan/vulkan.h> and then "volk.h" (main.c, nvn/vkr.h, overlay.h, gpu_driver.c)

add_library(somniel SHARED
    main.c
    overlay.c
    sidebar.c
    telemetry.c
    debug_log.c
    debug_socket.c
    lz4_block.c
    nso_loader.c
    nro_loader.c
    guest.c
    patcher.c
    svc.c
    spike.c
    kobj.c
    sync.c
    thread.c
    ipc.c
    services.c
    services_fs.c
    services_hostfs.c
    services_audio.c
    hook.c
    game_patches.c
    hooks_system.c
    hooks_time.c
    hooks_gfx.c
    nvn.c
    nvn_glue.c
    nvn/nvn_api_core.c
    nvn/nvn_api_obj.c
    nvn/nvn_api_cmd.c
    nvn/nvn_ext.c
    nvn/nvn_tex.c
    nvn/nvn_procs.c
    nvn/gfx_null.c
    nvn/vkr.c
    nvn/spv_opt.c
    gpu_driver.c
    nvn/shader_cache.c
    nvn/unity_scan.c
    nvn/tex_deswizzle.c
    hooks_hid.c
    hooks_movie.c
    hooks_swkbd.c
    guest_asm.S
)
target_include_directories(somniel PRIVATE ${CMAKE_CURRENT_LIST_DIR}/nvn)
target_compile_options(somniel PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(somniel PRIVATE SDL3::SDL3 volk adrenotools android log mediandk aaudio dl)








