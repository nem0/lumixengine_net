#include "core/allocator.h"
#include "core/array.h"
#include "core/delegate.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/stream.h"
#include "core/string.h"
#include "engine/engine.h"
#include "engine/plugin.h"
#include "enet/enet.h"
#include "lua/lua_wrapper.h"
#include "lua/lua_script_system.h"
#include "net.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "wininet.lib")


namespace Lumix
{


struct NetSystemImpl : NetSystem 
{
	enum class Channel : int
	{
		RPC = 0,
		LUA_STRING = 1,
		USER = 2,

		COUNT
	};
		
	NetSystemImpl(Engine& engine)
		: m_engine(engine)
		, m_allocator(engine.getAllocator())
		, m_is_initialized(false)
		, m_connections(m_allocator)
	{
		if (enet_initialize() < 0)
		{
			logError("Failed to initialize network.");
			return;
		}
		m_is_initialized = true;
	}

	void initBegin() override {
		registerLuaAPI(getLuaState());
	}

	void serialize(OutputMemoryStream& serializer) const override {}
	bool deserialize(i32 version, InputMemoryStream& serializer) override { return version == 0; }

	static int remoteCall(lua_State* L) {
		NetSystemImpl* that = LuaWrapper::toType<NetSystemImpl*>(L, lua_upvalueindex(1));
		
		ConnectionHandle connection = LuaWrapper::checkArg<ConnectionHandle>(L, 1);
		bool is_connection_valid = connection >= 0 && connection < that->m_connections.size();
		if (!is_connection_valid) luaL_argerror(L, 1, "invalid connection");
		const char* func_name = LuaWrapper::checkArg<const char*>(L, 2);
		int func_arg_count = lua_gettop(L) - 2;

		char buf[1024];
		// TODO handle overflow in blob
		OutputMemoryStream blob(buf, sizeof(buf));
		blob.writeString(func_name);
		blob.write(func_arg_count);

		for (int i = 0; i < func_arg_count; ++i) {
			int type = lua_type(L, i + 3);
			switch (type) {
				case LUA_TSTRING:
					blob.write(type);
					blob.writeString(lua_tostring(L, i + 3));
					break;
				case LUA_TNUMBER:
					blob.write(type);
					blob.write(lua_tonumber(L, i + 3));
					break;
				case LUA_TBOOLEAN:
					blob.write(type);
					blob.write(lua_toboolean(L, i + 3) != 0);
					break;
				default: logError("Can not RPC ", func_name, ", #", i + 1, "argument's type is not supported"); return 0;
			}
		}

		that->send(connection, (int)Channel::RPC, buf, (u32)blob.size(), true);
		return 0;
	}


	static int setCallback(lua_State* L) {
		NetSystemImpl* that = LuaWrapper::toType<NetSystemImpl*>(L, lua_upvalueindex(1));

		if (!lua_isfunction(L, 1)) LuaWrapper::argError(L, 1, "function");

		if (that->m_lua_callback_ref != -1) {
			LuaWrapper::releaseRef(that->m_lua_callback_state, that->m_lua_callback_ref);
		}

		that->m_lua_callback_state = L;
		lua_pushvalue(L, 1);
		that->m_lua_callback_ref = LuaWrapper::createRef(L);
		lua_pop(L, 1);
		return 0;
	}


	void registerLuaAPI(lua_State* L)
	{
		#define REGISTER_FUNCTION(name) \
			do {\
				auto f = &LuaWrapper::wrapMethodClosure<&NetSystemImpl::name>; \
				LuaWrapper::createSystemClosure(L, "Network", this, #name, f); \
			} while(false) \

			LuaWrapper::createSystemClosure(L, "Network", this, "setCallback", &NetSystemImpl::setCallback);
			LuaWrapper::createSystemClosure(L, "Network", this, "call", &NetSystemImpl::remoteCall);
			REGISTER_FUNCTION(createServer);
			REGISTER_FUNCTION(connect);
			REGISTER_FUNCTION(sendString);
			REGISTER_FUNCTION(eventPacketToString);
			REGISTER_FUNCTION(disconnect); 

		#undef REGISTER_FUNCTION
	}

	
	~NetSystemImpl() override
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

		lua_rawgeti(m_lua_callback_state, LUA_REGISTRYINDEX, m_lua_callback_ref);

		LuaWrapper::push(m_lua_callback_state, (int)event.type);
		LuaWrapper::push(m_lua_callback_state, connection);
		LuaWrapper::push(m_lua_callback_state, &event);
		if (lua_pcall(m_lua_callback_state, 3, 0, 0) != LUA_OK)
		{
			logError(lua_tostring(m_lua_callback_state, -1));
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

	lua_State* getLuaState() {
		auto* lua_system = (LuaScriptSystem*)m_engine.getSystemManager().getSystem("lua_script");
		return lua_system->getState();
	}

	void RPC(InputMemoryStream* blob)
	{
		const char* func_name = blob->readString();
		lua_State* L = getLuaState();
		lua_getglobal(L, "Network"); // [Network]
		lua_getfield(L, -1, "RPCFunctions"); // [Network, Network.RPCFunctions] 
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 2); // []
			logError("Unknown RPC function", func_name);
			return;
		}
		lua_getfield(L, -1, func_name);  // [Network, Network.RPCFunctions, func] 
		if(!lua_isfunction(L, -1))
		{ 
			lua_pop(L, 3); // []
			logError("Unknown RPC function", func_name);
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
					const char* tmp = blob->readString();
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
					logError(func_name, ": type of argument #", i + 1, " not supported by RPC");
					return;
			}
		}
		if (lua_pcall(L, func_arg_count, 0, 0) != LUA_OK)// [Network, Network.RPCFunctions] 
		{
			logError(lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		lua_pop(L, 2); // []
	}


	void handleEvent(const ENetEvent& event)
	{
		switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT: {
				ConnectionHandle idx = allocConnection();
				Connection& conn = m_connections[idx];
				conn.is_server = true;
				conn.peer = event.peer;
				callLuaCallback(event, getConnection(event.peer));
				if (m_connect_callback.isValid()) {
					m_connect_callback.invoke(idx);
				}
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT: {
				ConnectionHandle idx = getConnection(event.peer);
				if (idx < 0) return;
				m_connections[idx].peer = nullptr;
				callLuaCallback(event, getConnection(event.peer));
				if (m_disconnect_callback.isValid()) {
					m_disconnect_callback.invoke(idx);
				}
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
				switch ((Channel)event.channelID) {
					case Channel::RPC: {
						InputMemoryStream blob(event.packet->data, (int)event.packet->dataLength);
						RPC(&blob);
						break;
					}
					case Channel::LUA_STRING:
						callLuaCallback(event, getConnection(event.peer));
						break;
					case Channel::USER:
						if (m_receive_callback.isValid()) {
							ConnectionHandle conn = getConnection(event.peer);
							Span<const u8> data(event.packet->data, event.packet->dataLength);
							m_receive_callback.invoke(conn, data);
						}
						break;
					case Channel::COUNT:
						ASSERT(false);
						break;
				}
				break;
			case ENET_EVENT_TYPE_NONE: break;
		}
	}

	Delegate<void (ConnectionHandle, Span<const u8>)>& onDataReceived() override { return m_receive_callback; }
	Delegate<void(ConnectionHandle)>& onConnect() override { return m_connect_callback; }
	Delegate<void(ConnectionHandle)>& onDisconnect() override { return m_disconnect_callback; }

	void update(float time_delta) override {
		static u32 bw_in_counter = profiler::createCounter("Net in (B/s)", 0);
		static u32 bw_out_counter = profiler::createCounter("Net out (B/s)", 0);
		static u32 total_send_counter = profiler::createCounter("Net total send (KB)", 0);
		static u32 total_recv_counter = profiler::createCounter("Net total recv (KB)", 0);
		ENetEvent event;

		static float timer = 0;
		timer += time_delta;
		if (timer > 1) {
			ENetHost* host = m_server_host ? m_server_host : m_client_host;
			if (host) {
				static u32 total_send = host->totalSentData;
				static u32 total_recv = host->totalReceivedData;
				float out = (host->totalSentData - total_send) / timer;
				float in = (host->totalReceivedData - total_recv) / timer;
				profiler::pushCounter(bw_in_counter, out);
				profiler::pushCounter(bw_out_counter, in);
				total_send = host->totalSentData;
				total_recv = host->totalReceivedData;
			}
			timer = 0;
		}


		if (m_server_host) {
			profiler::pushCounter(total_send_counter, m_server_host->totalSentData / 1024.f);
			profiler::pushCounter(total_recv_counter, m_server_host->totalReceivedData / 1024.f);

			while (enet_host_service(m_server_host, &event, 0)) {
				handleEvent(event);
			}
		}

		if (m_client_host) {
			profiler::pushCounter(total_send_counter, m_client_host->totalSentData / 1024.f);
			profiler::pushCounter(total_recv_counter, m_client_host->totalReceivedData / 1024.f);

			while (enet_host_service(m_client_host, &event, 0)) {
				handleEvent(event);
			}
		}
	}

	void destroyServer() override {
		if (m_server_host) enet_host_destroy(m_server_host);
		m_server_host = nullptr;
	}

	bool createServer(u16 port, u32 max_clients) override {
		ENetAddress address;
		address.port = port;
		address.host = ENET_HOST_ANY;

		m_server_host = enet_host_create(&address, max_clients, (int)Channel::COUNT, 0, 0);
		if (!m_server_host) return false;

		return true;
	}

	bool send(ConnectionHandle connection, Span<const u8> data, bool reliable) override {
		return send(connection, (i32)Channel::USER, data.begin(), data.length(), reliable);
	}

	bool sendString(ConnectionHandle connection, const char* message, bool reliable)
	{
		return send(connection, (int)Channel::LUA_STRING, message, stringLength(message) + 1, reliable);
	}


	bool send(ConnectionHandle connection, int channel, const void* mem, u32 size, bool reliable)
	{
		if (connection >= m_connections.size() || !m_connections[connection].peer)
		{
			logError("Trying to send data through invalid connection.");
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


	ConnectionHandle connect(const char* host_name, u16 port) override {
		if (!m_client_host) {
			m_client_host = enet_host_create(nullptr, 64, (int)Channel::COUNT, 0, 0);
			if (!m_client_host) return INVALID_CONNECTION;
		}

		int idx = allocConnection();
		Connection& conn = m_connections[idx];

		ENetAddress address;
		enet_address_set_host(&address, host_name);
		address.port = port;
		conn.peer = enet_host_connect(m_client_host, &address, (int)Channel::COUNT, 0);
		conn.is_server = false;

		return conn.peer ? idx : INVALID_CONNECTION;
	}

	void disconnect(ConnectionHandle idx) override {
		if (idx >= m_connections.size() || !m_connections[idx].peer) {
			logError("Trying to close invalid connection.");
			return;
		}
		enet_peer_disconnect(m_connections[idx].peer, 0);
	}

	const char* getName() const override { return "network"; }

	Engine& m_engine;
	IAllocator& m_allocator;
	ENetHost* m_server_host = nullptr;
	ENetHost* m_client_host = nullptr;

	struct Connection {
		ENetPeer* peer = nullptr;
		bool is_server = false;
	};

	Array<Connection> m_connections;
	bool m_is_initialized = false;
	int m_lua_callback_ref = -1;
	lua_State* m_lua_callback_state = nullptr;
	Delegate<void (ConnectionHandle, Span<const u8>)> m_receive_callback;
	Delegate<void (ConnectionHandle)> m_connect_callback;
	Delegate<void (ConnectionHandle)> m_disconnect_callback;
};


LUMIX_PLUGIN_ENTRY(net)
{
	IAllocator& allocator = engine.getAllocator();
	NetSystemImpl* plugin = LUMIX_NEW(allocator, NetSystemImpl)(engine);
	if (!plugin->m_is_initialized)
	{
		LUMIX_DELETE(allocator, plugin);
		plugin = nullptr;
	}
	return plugin;
}


}