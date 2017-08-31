# lumixengine_net
Network plugin for Lumix Engine. Work in progress.

# Low Level API

Create server

```lua
local port = 1234
local max_clients = 1
local channel_num = 2
Network.createServer(g_network, port, max_clients, channel_num)
```

Set callback

```lua
Network.setCallback(g_network, function(type, id, event)
	Engine.logError(Network.eventPacketToString(g_network, event))
end)
```

Connect to server

```lua
local port = 1234
local channel_num = 2
connection = Network.connect(g_network, "localhost", port, channel_num)
```

Send string message

```lua
local reliable = true
local channel = 0
Network.sendString(g_network, connection, channel, "test", reliable)
```
