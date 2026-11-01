-- faketty: the ESP32-S3's end of the link (PROTOCOL.md) in Mesen 2, serial only.
-- Run: `make run`. Bytes appended to $ESP32_S3_IN (default /tmp/ttyEMU.in) go to
-- the Famicom; `make bridge` feeds that file from the PTY /tmp/ttyEMU.
-- Needs --debug.scriptWindow.allowIoOsAccess=true. The UART and the link core
-- are tools/esp-link.lua.
local dir = debug.getinfo(1, "S").source:match("^@(.*/)") or "./"
local esp = dofile(dir .. "esp-link.lua").new()

local IN_PATH = os.getenv("ESP32_S3_IN") or "/tmp/ttyEMU.in"
local infile

local function poll_input()
  if not io then return end               -- I/O access disabled
  if not infile then
    infile = io.open(IN_PATH, "rb")
    if not infile then return end
    infile:seek("end")                    -- skip stale data
  end
  infile:seek("cur", 0)                   -- clear EOF so new data is seen
  local data = infile:read(4096)
  if data and #data > 0 then esp:push(data) end
end

emu.addEventCallback(poll_input, emu.eventType.endFrame)
