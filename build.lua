workspace "nexchat"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }

OutputDir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

include "libcommon/build-libcommon.lua"

include "server/build-server.lua"
include "client/build-client.lua"