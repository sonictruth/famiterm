-- Run via: Mesen --testRunner rom.nes mesen-shot.lua  (see `make shot`)
local frames = 0
emu.addEventCallback(function()
  frames = frames + 1
  if frames == 30 then
    local f = assert(io.open(os.getenv("SHOT_OUT") or "build/mesen.png", "wb"))
    f:write(emu.takeScreenshot())
    f:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)
