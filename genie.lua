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
	includedirs { "src", "external/enet/include", "../../external/lua/include" }
	defines { "BUILDING_NET", "_WINSOCK_DEPRECATED_NO_WARNINGS" }
	links { "engine" }
	useLua()
	defaultConfigurations()

linkPlugin("net")