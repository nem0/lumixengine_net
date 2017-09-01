project "lumixengine_net"
	libType()
	files { 
		"external/**.cpp",
		"external/**.c",
		"external/**.h",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	includedirs { "../lumixengine_net/src", "external/enet/include", "../lumixengine/external/lua/include" }
	defines { "BUILDING_NET", "_WINSOCK_DEPRECATED_NO_WARNINGS" }
	links { "engine" }
	useLua()
	defaultConfigurations()
