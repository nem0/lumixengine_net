#pragma once

#include "engine/plugin.h"

namespace Lumix {

template <typename T> struct Delegate;

struct NetSystem : ISystem {
	using ConnectionHandle = i32;
	static constexpr inline ConnectionHandle INVALID_CONNECTION = -1;

	virtual bool createServer(u16 port, u32 max_clients) = 0;
	virtual void destroyServer() = 0;
	virtual ConnectionHandle connect(const char* host_name, u16 port) = 0;
	virtual Delegate<void(ConnectionHandle, Span<const u8>)>& onDataReceived() = 0;
	virtual Delegate<void(ConnectionHandle)>& onConnect() = 0;
	virtual Delegate<void(ConnectionHandle)>& onDisconnect() = 0;
	virtual bool send(ConnectionHandle connection, Span<const u8> data, bool reliable) = 0;
	virtual void disconnect(ConnectionHandle idx) = 0;
};

} // namespace Lumix