#include "enet/enet.h"
#include "engine/engine.h"
#include "engine/blob.h"
#include "engine/iallocator.h"
#include "engine/iplugin.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/property_register.h"


#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "wininet.lib")


namespace Lumix
{


typedef int ConnectionHandle;


struct NetPluginImpl : IPlugin 
{
	enum class Channel : int
	{
		RPC = 0,
		LUA_STRING = 1,
		USER = 2,

		COUNT
	};
		
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


	static int remoteCall(lua_State* L)
	{
		NetPluginImpl* that = LuaWrapper::checkArg<NetPluginImpl*>(L, 1);
		ConnectionHandle connection = LuaWrapper::checkArg<ConnectionHandle>(L, 2);
		bool is_connection_valid = connection >= 0 && connection < that->m_connections.size();
		if (!is_connection_valid) luaL_argerror(L, 2, "invalid connection");
		const char* func_name = LuaWrapper::checkArg<const char*>(L, 3);
		int func_arg_count = lua_gettop(L) - 3;

		char buf[1024];
		// TODO handle overflow in blob
		OutputBlob blob(buf, sizeof(buf));
		blob.writeString(func_name);
		blob.write(func_arg_count);

		for (int i = 0; i < func_arg_count; ++i)
		{
			int type = lua_type(L, i + 4);
			switch (type)
			{
				case LUA_TSTRING:
					blob.write(type);
					blob.writeString(lua_tostring(L, i + 4));
					break;
				case LUA_TNUMBER:
					blob.write(type);
					blob.write(lua_tonumber(L, i + 4));
					break;
				case LUA_TBOOLEAN:
					blob.write(type);
					blob.write(lua_toboolean(L, i + 4) != 0);
					break;
				default:
					g_log_error.log("Network") << "Can not RPC " << func_name << ", #" << i + 1 << "argument's type is not supported";
					return 0;
			}
		}

		that->send(connection, (int)Channel::RPC, buf, blob.getPos(), true);
		return 0;
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
			LuaWrapper::createSystemFunction(L, "Network", "call", &NetPluginImpl::remoteCall);
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


	void RPC(InputBlob* blob)
	{
		char func_name[128];
		blob->readString(func_name, lengthOf(func_name));
		lua_State* L = m_engine.getState();
		lua_getglobal(L, "Network"); // [Network]
		lua_getfield(L, -1, "RPCFunctions"); // [Network, Network.RPCFunctions] 
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 2); // []
			g_log_error.log("Network") << "Unknown RPC function" << func_name;
			return;
		}
		lua_getfield(L, -1, func_name);  // [Network, Network.RPCFunctions, func] 
		if(!lua_isfunction(L, -1))
		{ 
			lua_pop(L, 3); // []
			g_log_error.log("Network") << "Unknown RPC function" << func_name;
			return;
		}

		int func_arg_count = blob->read<int>();
		for (int i = 0; i < func_arg_count; ++i)
		{
			int type = blob->read<int>();
			switch(type)
			{
				case LUA_TSTRING:
				{
					char tmp[1024];
					blob->readString(tmp, lengthOf(tmp));
					lua_pushstring(L, tmp); // [Network, Network.RPCFunctions, func, args...] 
					break;
				}
				case LUA_TNUMBER:
				{
					lua_Number n = blob->read<lua_Number>();
					lua_pushnumber(L, n);
					break;
				}
				case LUA_TBOOLEAN:
				{
					bool b = blob->read<bool>();
					lua_pushboolean(L, b ? 1 : 0);
					break;
				}
				default:
					lua_pop(L, 3 + i);
					g_log_error.log("Network") << func_name << ": type of argument #" << i + 1 << " not supported by RPC";
					return;
			}
		}
		if (lua_pcall(L, func_arg_count, 0, 0) != LUA_OK)// [Network, Network.RPCFunctions] 
		{
			g_log_error.log("Network") << lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		lua_pop(L, 2); // []
	}


	void handleEvent(const ENetEvent& event)
	{
		switch (event.type)
		{
			case ENET_EVENT_TYPE_CONNECT:
				{
					int idx = allocConnection();
					Connection& conn = m_connections[idx];
					conn.is_server = true;
					conn.peer = event.peer;
					callLuaCallback(event, getConnection(event.peer));
				}
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				{
					int idx = getConnection(event.peer);
					if (idx < 0) return;
					m_connections[idx].peer = nullptr;
					callLuaCallback(event, getConnection(event.peer));
				}
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				switch ((Channel)event.channelID)
				{
					case Channel::RPC:
						{
							InputBlob blob(event.packet->data, (int)event.packet->dataLength);
							RPC(&blob);
						}
						break;
					case Channel::LUA_STRING:
						callLuaCallback(event, getConnection(event.peer));
						break;
				}
				break;
		}
	}


	void update(float time_delta) override
	{
		ENetEvent event;

		if (m_server_host)
		{
			while (enet_host_service(m_server_host, &event, 0))
			{
				handleEvent(event);
			}
		}

		if (m_client_host)
		{
			while (enet_host_service(m_client_host, &event, 0))
			{
				handleEvent(event);
			}
		}
	}


	bool createServer(u16 port, int max_clients)
	{
		ENetAddress address;
		address.port = port;
		address.host = ENET_HOST_ANY;

		m_server_host = enet_host_create(&address, max_clients, (int)Channel::COUNT, 0, 0);
		if (!m_server_host) return false;

		return true;
	}


	bool sendString(ConnectionHandle connection, const char* message, bool reliable)
	{
		return send(connection, (int)Channel::LUA_STRING, message, stringLength(message) + 1, reliable);
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


	ConnectionHandle connect(const char* host_name, u16 port)
	{
		if(!m_client_host)
		{
			m_client_host = enet_host_create(nullptr, 64, (int)Channel::COUNT, 0, 0);
			if (!m_client_host) return -1;
		}

		int idx = allocConnection();
		Connection& conn = m_connections[idx];

		ENetAddress address;
		enet_address_set_host(&address, host_name);
		address.port = port;
		conn.peer = enet_host_connect(m_client_host, &address, (int)Channel::COUNT, 0);
		conn.is_server = false;

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