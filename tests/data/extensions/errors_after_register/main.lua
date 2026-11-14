-- binds a key and starts an interval, then fails before it's done loading
RegisterKeybind(Keys.F7, function() end, KeybindModifiers.Alt)
SetInterval(function() end, 1000)
error("stops here on purpose")
