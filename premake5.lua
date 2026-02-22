-- Setup the extension.
local ext = get_current_extension_info()
project_ext(ext)

-- Link folders that should be packaged with the extension.
repo_build.prebuild_link {
    { "data", ext.target_dir.."/data" },
    { "docs", ext.target_dir.."/docs" },
}

-- Read the aerosim-world-link library path written by build.sh / build.bat.
-- The build scripts copy the lib files into the extension's aerosim-world-link-lib/
-- subdirectory so they are accessible when Kit builds inside a Docker container
-- (docker build option), where the original AEROSIM_WORLD_LINK_LIB host path may
-- not be mounted. The path is used below for includedirs, libdirs, and post-build
-- copy commands.
local aerosim_world_link_lib_path = nil
local file = io.open("aerosim_world_link_lib_path.txt", "r")
if file then
    aerosim_world_link_lib_path = file:read("*line")
    file:close()
else
    error("Could not read aerosim_world_link_lib_path.txt. Please run build.sh script to regenerate it.")
end

-- Build the C++ plugin that will be loaded by the extension.
-- The plugin must implement the omni::ext::IExt interface to
-- be automatically loaded by the extension system at startup.
project_ext_plugin(ext, "aerosim.omniverse.extension.plugin")
    add_files("include", "include/aerosim/omniverse/extension")
    add_files("source", "plugins/aerosim.omniverse.extension")

    -- Use Kit's USD helper for proper linking
    extra_usd_libs = {
        "usdGeom",
        "usdUtils"
    }
    add_usd(extra_usd_libs)

    includedirs {
        "include",
        "plugins/aerosim.omniverse.extension",
    }
    libdirs { "%{target_deps}/usd/release/lib" }
    links { "aerosim_world_link" }
    defines { "NOMINMAX", "NDEBUG" }
    runtime "Release"
    rtti "On"

    filter { "system:linux" }
        exceptionhandling "On"
        staticruntime "Off"
        cppdialect "C++17"
        buildoptions { "-pthread -lstdc++fs -Wno-error" }
        linkoptions { "-Wl,--disable-new-dtags -Wl,-rpath,%{target_deps}/usd/release/lib:%{target_deps}/python/lib:" }
    filter { "system:windows" }
        buildoptions { "/wd4244 /wd4305 /EHsc" }
    filter {}

    -- Link aerosim_world_link library
    includedirs { aerosim_world_link_lib_path }
    libdirs { aerosim_world_link_lib_path }
    filter { "system:windows" }
        postbuildcommands{
            "{COPY} " .. aerosim_world_link_lib_path .. "/aerosim_world_link.dll " .. ext.target_dir .. "/bin"
        }
    filter { "system:linux" }
        postbuildcommands{
            "{COPY} " .. aerosim_world_link_lib_path .. "/libaerosim_world_link.so " .. ext.target_dir .. "/bin"
        }
    filter {}

-- Build Python bindings that will be loaded by the extension.
project_ext_bindings {
    ext = ext,
    project_name = "aerosim.omniverse.extension.python",
    module = "_aerosim_connector_bindings",
    src = "bindings/python/aerosim.omniverse.extension",
    target_subdir = "aerosim/omniverse/extension"
}
    includedirs { "include" }

    -- Link aerosim_world_link for publish_image_to_topic() Python binding
    includedirs { aerosim_world_link_lib_path }
    libdirs { aerosim_world_link_lib_path }
    links { "aerosim_world_link" }

    repo_build.prebuild_link {
        { "python/impl", ext.target_dir.."/aerosim/omniverse/extension/impl" },
        { "python/tests", ext.target_dir.."/aerosim/omniverse/extension/tests" },
    }
