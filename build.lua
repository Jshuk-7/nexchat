workspace "PROJECT_NAME"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

include "core/build-core.lua"
include "app/build-app.lua"