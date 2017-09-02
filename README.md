# Network plugin for [Lumix Engine](https://github.com/nem0/lumixengine). 
------

[![Gitter](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/nem0/LumixEngine?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)
[![License](http://img.shields.io/:license-mit-blue.svg)](http://doge.mit-license.org)
[![Twitter](https://img.shields.io/twitter/url/http/shields.io.svg?style=social)](https://twitter.com/mikulasflorek)

Very early in development.

[Getting started](https://www.youtube.com/watch?v=NAUASTmoulQ)

[Documentation](https://github.com/nem0/lumixengine_net/wiki)

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
