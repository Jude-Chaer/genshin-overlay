metadata = { name = "registers in init" }

function Init()
    RegisterKeybind(Keys.F8, function() end, KeybindModifiers.Alt)
    SetInterval(function() end, 1000)
end

function Shutdown()
end
