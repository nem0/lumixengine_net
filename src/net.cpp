#include "engine/engine.h"
#include "enet/enet.h"
#include "engine/iallocator.h"
#include "engine/iplugin.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"


#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "wininet.lib")


namespace Lumix
{


typedef int ConnectionHandle;


struct NetPluginImpl : IPlugin 
{
	NetPluginImpl(Engine& engine)
		: m_engine(engine)
		, m_allocator(engine.getAllocator())
		, m_is_initialized(false)
		, m_connections(m_allocator)
	{
		if (enet_initialize() < 0)
		{
			g_log_error.log("Network") << "Failed to initialize network.";
			return;
		}
		m_is_initialized = true;
		registerLuaAPI(engine.getState());
	}


	static int setCallback(lua_State* L)
	{
		NetPluginImpl* that = LuaWrapper::checkArg<NetPluginImpl*>(L, 1);
		if (!lua_isfunction(L, 2)) LuaWrapper::argError(L, 2, "function");
		
		if (that->m_lua_callback_ref != -1)
		{
			luaL_unref(that->m_lua_callback_state, LUA_REGISTRYINDEX, that->m_lua_callback_ref);
		}

		lua_pushvalue(L, 2);
		that->m_lua_callback_state = L;
		that->m_lua_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
		lua_pop(L, 1);
		return 0;
	}


	void registerLuaAPI(lua_State* L)
	{
		lua_pushlightuserdata(L, this);
		lua_setglobal(L, "g_network");
		#define REGISTER_FUNCTION(name) \
			do {\
				auto f = &LuaWrapper::wrapMethod<NetPluginImpl, decltype(&NetPluginImpl::name), &NetPluginImpl::name>; \
				LuaWrapper::createSystemFunction(L, "Network", #name, f); \
			} while(false) \

			LuaWrapper::createSystemFunction(L, "Network", "setCallback", &NetPluginImpl::setCallback);
			REGISTER_FUNCTION(createServer);
			REGISTER_FUNCTION(connect);
			REGISTER_FUNCTION(sendString);
			REGISTER_FUNCTION(eventPacketToString);
			REGISTER_FUNCTION(disconnect); 

		#undef REGISTER_FUNCTION
	}

	
	~NetPluginImpl() override
	{
		if (!m_is_initialized) return;

		for(Connection& c : m_connections)
		{
			if (!c.peer) continue;
			enet_peer_reset(c.peer);
		}

		if (m_server_host) enet_host_destroy(m_server_host);
		if (m_client_host) enet_host_destroy(m_client_host);

		enet_deinitialize();
	}


	const char* eventPacketToString(ENetEvent* event)
	{
		if (!event->packet) return "";
		if (event->packet->dataLength == 0) return "";
		if (event->packet->data[event->packet->dataLength - 1] != '\0') return "";
		return (const char*)event->packet->data;
	}


	void callLuaCallback(const ENetEvent& event, ConnectionHandle connection)
	{
		if (m_lua_callback_ref == -1) return;

		if (lua_rawgeti(m_lua_callback_state, LUA_REGISTRYINDEX, m_lua_callback_ref) != LUA_TFUNCTION)
		{
			ASSERT(false);
		}

		LuaWrapper::push(m_lua_callback_state, (int)event.type);
		LuaWrapper::push(m_lua_callback_state, connection);
		LuaWrapper::push(m_lua_callback_state, &event);
		if (lua_pcall(m_lua_callback_state, 3, 0, 0) != LUA_OK)
		{
			g_log_error.log("Network") << lua_tostring(m_lua_callback_state, -1);
			lua_pop(m_lua_callback_state, 1);
		}
	}


	int getConnection(const ENetPeer* peer) const
	{
		if (!peer) return -1;
		for (int i = 0, c = m_connections.size(); i < c; ++i)
		{
			const Connection& conn = m_connections[i];
			if (conn.peer == peer) return i;
		}
		return -1;
	}


	void disconnectPeer(const ENetPeer* peer)
	{
		int idx = getConnection(peer);
		if (idx < 0) return;
		m_connections[idx].peer = nullptr;
	}


	void update(float time_delta) override
	{
		ENetEvent event;

		if (m_server_host)
		{
			while (enet_host_service(m_server_host, &event, 0))
			{
				callLuaCallback(event, getConnection(event.peer));
				switch (event.type)
				{
					case ENET_EVENT_TYPE_CONNECT:
						{
							int idx = allocConnection();
							Connection& conn = m_connections[idx];
							conn.is_server = true;
							conn.peer = event.peer;
						}
						break;
					case ENET_EVENT_TYPE_DISCONNECT:
						disconnectPeer(event.peer);
						break;
					case ENET_EVENT_TYPE_RECEIVE:
						break;
				}
			}
		}

		if (m_client_host)
		{
			while (enet_host_service(m_client_host, &event, 0))
			{
				callLuaCallback(event, getConnection(event.peer));
				switch (event.type)
				{
					case ENET_EVENT_TYPE_CONNECT:
						break;
					case ENET_EVENT_TYPE_DISCONNECT:
						disconnectPeer(event.peer);
						break;
					case ENET_EVENT_TYPE_RECEIVE:
						break;
				}
			}
		}
	}


	bool createServer(u16 port, int max_clients, int channels)
	{
		ENetAddress address;
		address.port = port;
		address.host = ENET_HOST_ANY;

		m_server_host = enet_host_create(&address, max_clients, channels, 0, 0);
		if (!m_server_host) return false;

		return true;
	}


	bool sendString(ConnectionHandle connection, int channel, const char* message, bool reliable)
	{
		return send(connection, channel, message, stringLength(message) + 1, reliable);
	}


	bool send(ConnectionHandle connection, int channel, const void* mem, int size, bool reliable)
	{
		if (connection >= m_connections.size() || !m_connections[connection].peer)
		{
			g_log_error.log("Network") << "Trying to send data through invalid connection.";
			return false;
		}
		ENetPacket * packet = enet_packet_create(mem, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
		Connection& c = m_connections[connection];
		return enet_peer_send(c.peer, channel, packet) == 0;
	}


	int allocConnection()
	{
		for (int i = 0; i < m_connections.size(); ++i)
		{
			Connection& c = m_connections[i];
			if (!c.peer) return i;
		}

		m_connections.emplace();
		return m_connections.size() - 1;
	}


	ConnectionHandle connect(const char* host_name, u16 port, int channels)
	{
		if(!m_client_host)
		{
			m_client_host = enet_host_create(nullptr, 64, channels, 0, 0);
			if (!m_client_host) return -1;
		}

		int idx = allocConnection();
		Connection& conn = m_connections[idx];

		ENetAddress address;
		enet_address_set_host(&address, host_name);
		address.port = port;
		conn.peer = enet_host_connect(m_client_host, &address, channels, 0);

		return conn.peer ? idx : -1;
	}


	void disconnect(ConnectionHandle idx)
	{
		if (idx >= m_connections.size() || !m_connections[idx].peer)
		{
			g_log_error.log("Network") << "Trying to close invalid connection.";
			return;
		}
		enet_peer_disconnect(m_connections[idx].peer, 0);
	}


	const char* getName() const override { return "network"; }


	Engine& m_engine;
	IAllocator& m_allocator;
	ENetHost* m_server_host = nullptr;
	ENetHost* m_client_host = nullptr;

	struct Connection
	{
		ENetPeer* peer = nullptr;
		bool is_server = false;
	};

	Array<Connection> m_connections;
	bool m_is_initialized = false;
	int m_lua_callback_ref = -1;
	lua_State* m_lua_callback_state = nullptr;
};


LUMIX_PLUGIN_ENTRY(lumixengine_net)
{
	IAllocator& allocator = engine.getAllocator();
	NetPluginImpl* plugin = LUMIX_NEW(allocator, NetPluginImpl)(engine);
	if (!plugin->m_is_initialized)
	{
		LUMIX_DELETE(allocator, plugin);
		plugin = nullptr;
	}
	return plugin;
}


}