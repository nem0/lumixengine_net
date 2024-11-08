project "net"
	libType()
	files { 
		"external/**.cpp",
		"external/**.c",
		"external/**.h",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	includedirs { "src", "external/enet/include", "../../external/luau/include" }
	defines { "BUILDING_NET", "_WINSOCK_DEPRECATED_NO_WARNINGS" }
	links { "engine" }
	defaultConfigurations()

linkPlugin("net")