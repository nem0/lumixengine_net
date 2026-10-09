-- Runs inside the plugin project. Paths are relative to this file.
files { "external/**.cpp", "external/**.c", "external/**.h", "src/**.cpp", "src/**.h" }
includedirs { "src", "external/enet/include", "../lua/src" }
defines { "BUILDING_NET", "_WINSOCK_DEPRECATED_NO_WARNINGS" }
if _OPTIONS["dynamic-plugins"] then
	links { "engine" }
end
filter "system:windows"
	links { "ws2_32" }
filter {}
