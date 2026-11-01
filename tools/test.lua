-- Emulates the ESP32-S3 side of PROTOCOL.md in Mesen 2 (tools/esp-link.lua).
-- Run: `make test` (needs --debug.scriptWindow.allowIoOsAccess=true for serial input)
-- Demo: waits 2s, draws a VT52 chart, then sends 40 numbered lines 0.5s apart.
-- Serial via `make bridge` (see below).
local FPS      = 60
local CHART_AT = 2 * FPS
local START    = 5 * FPS
local INTERVAL = FPS // 2
local COUNT    = 40
local DEMO     = os.getenv("ESP32_S3_DEMO") ~= "0"

local dir = debug.getinfo(1, "S").source:match("^@(.*/)") or "./"
local esp = dofile(dir .. "esp-link.lua").new()   -- the UART and link core

local function push(s) esp:push(s) end

local function yx(r, c) return "\27Y" .. string.char(32 + r, 32 + c) end

local function chart()
  local out = { "\27H\27J" }                      -- home + erase to end of screen
  -- full CP437 chart, 16x16 (NUL, BEL, BS, TAB, LF, CR, ESC are controls: blank)
  local ctl = { [0]=1, [7]=1, [8]=1, [9]=1, [10]=1, [13]=1, [27]=1 }
  for r = 0, 15 do
    out[#out + 1] = yx(r, 0)
    for c = 0, 15 do
      local b = r * 16 + c
      if ctl[b] then b = 32 end
      out[#out + 1] = string.char(b)
      if c < 15 then out[#out + 1] = " " end
    end
  end
  -- graphics set 0x5E-0x7E
  out[#out + 1] = yx(17, 0) .. "\27F"
  for b = 0x5E, 0x7E do out[#out + 1] = string.char(b) end
  out[#out + 1] = "\27G"
  -- cursor movement: ESC A/B/C/D, erase to end of line, reverse linefeed
  out[#out + 1] = yx(19, 0) .. "cursor keys:\27B\27D\27D\27D\27D\27D\27D\27D\27D\27D\27D\27D\27D\27Cdown-left"
  out[#out + 1] = yx(21, 0) .. "XXXXXXXXXXXXXXXX" .. yx(21, 4) .. "\27K" .. "<-EOL erase"
  out[#out + 1] = yx(23, 0) .. "wrap off: " .. string.rep("0123456789", 4)
  out[#out + 1] = yx(25, 0) .. "bell \7"
  push(table.concat(out))
end

-- Serial input: bytes appended to $ESP32_S3_IN (default /tmp/ttyEMU.in) are sent
-- to the Famicom. `make bridge` feeds that file from a PTY.
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
  if data and #data > 0 then push(data) end
end

local frame, sent, charted = 0, 0, false
emu.addEventCallback(function()
  frame = frame + 1
  if DEMO then
    if not charted and frame >= CHART_AT then
      charted = true
      chart()
    end
    if sent < COUNT and frame >= START + sent * INTERVAL then
      sent = sent + 1
      push(string.format("line %d\r\n", sent))
    end
  end
  poll_input()
end, emu.eventType.endFrame)
