-- esp-link.lua: the ESP32-S3's end of the link (PROTOCOL.md), in Mesen 2.
--
--   local esp = dofile(dir .. "esp-link.lua").new{ ...options }
--
-- A UART at the level of CPU cycles on the two wires the real one uses: the
-- ROM's OUT1 ($4016 bit 1, 1 = space) decoded as 8N1, and replies sent as 8N2
-- on P2 D1 ($4017 bit 1, which the console inverts: space reads 1). Behind it,
-- the same link core as firmware/link-core.cpp: frames found by marker and
-- CRC, SYS HELLO and ECHO, TERM READ and WRITE, replies held for a repeat.
--
-- Options (all optional):
--   turnaround  cycles from a request's last stop bit to the reply (179 = 100 us)
--   jitter      up to this many cycles more, at random
--   skew        the ESP32-S3's clock against the console's, e.g. 0.02 = 2% fast
--   drop_every  leave every Nth reply unsent, as if lost
--   corrupt_every  flip one bit of every Nth reply sent
--   source      function() -> a string of more terminal output, when the queue is low
--
-- esp:push(s)      queue terminal output for the console
-- esp:restart()    forget the hello, as a power cycle would (the stream is kept)
-- esp.typed        bytes the console sent with TERM WRITE
-- esp.stats        counts, and esp:report() a line of them
-- esp.drained()    true when everything pushed has been taken by the console
local M = {}

local function script_dir()
  local src = debug.getinfo(1, "S").source
  return src:match("^@(.*/)") or "./"
end

-- link-proto.h is the one source of the numbers.
local function read_proto()
  local proto = {}
  local f = io.open(script_dir() .. "../firmware/link-proto.h", "r")
  if not f then error("esp-link.lua: cannot read firmware/link-proto.h") end
  for line in f:lines() do
    local name, value = line:match("^%s*#define%s+(LINK_[%w_]+)%s+(0x%x+)")
    if not name then name, value = line:match("^%s*#define%s+(LINK_[%w_]+)%s+(%d+)") end
    if name then proto[name] = tonumber(value) end
  end
  f:close()
  return proto
end

local function crc16(bytes, first, last)
  local crc = 0xFFFF
  for i = first, last do
    crc = crc ~ (bytes[i] << 8)
    for _ = 1, 8 do
      if crc & 0x8000 ~= 0 then crc = ((crc << 1) ~ 0x1021) & 0xFFFF
      else crc = (crc << 1) & 0xFFFF end
    end
  end
  return crc
end
M.crc16 = crc16

function M.new(opts)
  opts = opts or {}
  local L = read_proto()
  local P = L.LINK_PERIOD
  local esp = {
    typed = {},
    stats = { requests = 0, replies = 0, repeats = 0, noise = 0, framing = 0,
              hellos = 0, dropped = 0, corrupted = 0, tx_dev = 0, taken = 0, lost = 0 },
    proto = L,
  }
  local st = esp.stats
  local turnaround = opts.turnaround or 179
  local jitter = opts.jitter or 0
  local skew = opts.skew or 0

  -- ---- the link core (firmware/link-core.cpp) ----------------------------
  local helloed, reply_max = false, L.LINK_REPLY_MAX_DEFAULT
  local queue, qhead = {}, 1         -- terminal output not yet taken into `held`
  local held, base = {}, 0           -- sent or about to be, not yet acknowledged
  local last = {}                    -- per service: the reply held for a repeat
  local rxbuf = {}

  function esp:push(s)
    if type(s) == "string" then
      for i = 1, #s do queue[#queue + 1] = s:byte(i) end
    else
      for _, b in ipairs(s) do queue[#queue + 1] = b end
    end
  end

  -- What a power cycle loses: the hello, and the bytes sent and not yet
  -- acknowledged (RAM). What the host had not yet sent is kept, as a telnet
  -- server would keep it.
  function esp:restart()
    helloed, reply_max, last, rxbuf = false, L.LINK_REPLY_MAX_DEFAULT, {}, {}
    st.lost = st.lost + #held
    held = {}
  end

  function esp.drained() return qhead > #queue and #held == 0 end
  function esp.helloed() return helloed end

  local function reply(svc, status, seq, payload)
    local f = { L.LINK_REPLY, svc, status, seq, #payload }
    for _, b in ipairs(payload) do f[#f + 1] = b end
    local crc = crc16(f, 2, #f)
    f[#f + 1] = crc >> 8
    f[#f + 1] = crc & 0xFF
    return f
  end

  local function term_read(payload)
    if #payload ~= 2 then return L.LINK_BAD_ARG, {} end
    local pos, room = payload[1], payload[2]
    local d = (pos - base) & 0xFF
    if d <= #held then
      for _ = 1, d do table.remove(held, 1) end
      st.taken = st.taken + d
    end                               -- else: the console knows better; keep it all
    base = pos
    local n = math.min(room, reply_max)
    while #held < n do
      if qhead > #queue and opts.source then
        queue, qhead = {}, 1
        esp:push(opts.source())
      end
      if qhead > #queue then break end
      held[#held + 1] = queue[qhead]
      queue[qhead] = nil
      qhead = qhead + 1
    end
    if qhead > #queue then queue, qhead = {}, 1 end
    local out = {}
    for i = 1, math.min(n, #held) do out[i] = held[i] end
    return L.LINK_OK, out
  end

  -- returns status, payload, hold
  local function serve(svc, op, payload)
    if svc == L.LINK_SVC_SYS then
      if op == L.LINK_SYS_HELLO then
        if #payload < 7 then return L.LINK_BAD_ARG, {}, false end
        local answer = { L.LINK_PROTOCOL, L.LINK_FW_MAJOR, L.LINK_FW_MINOR, L.LINK_FW_PATCH,
                         L.LINK_PAYLOAD_MAX }
        if payload[1] ~= L.LINK_PROTOCOL then return L.LINK_VERSION, answer, false end
        reply_max = math.max(L.LINK_REPLY_MAX_DEFAULT, math.min(payload[7], L.LINK_PAYLOAD_MAX))
        helloed, last, base = true, {}, 0
        st.hellos = st.hellos + 1
        return L.LINK_OK, answer, false
      elseif op == L.LINK_SYS_ECHO then
        local out = {}
        for i = 1, math.min(#payload, reply_max) do out[i] = payload[i] end
        return L.LINK_OK, out, true
      end
      return L.LINK_NO_OP, {}, true
    elseif svc == L.LINK_SVC_TERM then
      if not helloed then return L.LINK_INACTIVE, {}, false end
      if op == L.LINK_TERM_READ then
        local status, out = term_read(payload)
        return status, out, false
      elseif op == L.LINK_TERM_WRITE then
        for _, b in ipairs(payload) do esp.typed[#esp.typed + 1] = b end
        return L.LINK_OK, { #payload }, true
      end
      return L.LINK_NO_OP, {}, true
    end
    return L.LINK_NO_SERVICE, {}, true
  end

  -- One byte off the line. Returns a reply frame, or nil.
  local function feed(b)
    rxbuf[#rxbuf + 1] = b
    while #rxbuf > 0 do
      if rxbuf[1] ~= L.LINK_REQUEST then
        table.remove(rxbuf, 1); st.noise = st.noise + 1
      elseif #rxbuf < L.LINK_HEADER then
        return nil
      elseif rxbuf[5] > L.LINK_PAYLOAD_MAX then
        table.remove(rxbuf, 1); st.noise = st.noise + 1
      elseif #rxbuf < L.LINK_OVERHEAD + rxbuf[5] then
        return nil
      else
        local len = rxbuf[5]
        local crc = crc16(rxbuf, 2, 5 + len)
        if rxbuf[6 + len] ~= crc >> 8 or rxbuf[7 + len] ~= crc & 0xFF then
          table.remove(rxbuf, 1); st.noise = st.noise + 1
        else
          local svc, op, seq = rxbuf[2], rxbuf[3], rxbuf[4]
          local payload = {}
          for i = 1, len do payload[i] = rxbuf[5 + i] end
          rxbuf = {}
          st.requests = st.requests + 1
          local h = last[svc]
          if h and h.op == op and h.seq == seq and h.len == len and h.crc == crc then
            st.repeats = st.repeats + 1
            return h.frame
          end
          local status, out, hold = serve(svc, op, payload)
          local frame = reply(svc, status, seq, out)
          if hold and status ~= L.LINK_BUSY then
            last[svc] = { op = op, seq = seq, len = len, crc = crc, frame = frame }
          else
            last[svc] = nil
          end
          return frame
        end
      end
    end
  end

  -- ---- the UART -------------------------------------------------------------
  -- To the ESP32-S3: OUT1's changes, with their cycles. 1 = space.
  local trans, base_level, line = {}, 0, 0
  local rx_start = nil
  -- To the console: one reply at a time, 8N2 from tx_start.
  local tx_start, tx_bytes, tx_end = nil, nil, 0
  local nreplies = 0

  local function level_at(t)
    local lvl = base_level
    for i = 1, #trans do
      if trans[i].t <= t then lvl = trans[i].level else break end
    end
    return lvl
  end

  local function send(frame, at)
    nreplies = nreplies + 1
    if opts.drop_every and nreplies % opts.drop_every == 0 then
      st.dropped = st.dropped + 1
      return
    end
    if opts.corrupt_every and nreplies % opts.corrupt_every == 0 then
      local copy = {}
      for i, b in ipairs(frame) do copy[i] = b end
      local i = math.random(1, #copy)
      copy[i] = copy[i] ~ (1 << math.random(0, 7))
      frame = copy
      st.corrupted = st.corrupted + 1
    end
    if jitter > 0 then at = at + math.random(0, jitter) end
    tx_start = math.max(at, tx_end)
    tx_bytes = frame
    tx_end = tx_start + math.ceil(#frame * L.LINK_TO_NES_BITS * P / (1 + skew))
    st.replies = st.replies + 1
  end

  local function rx_advance(now)
    while true do
      if not rx_start then
        local idx
        for i = 1, #trans do
          if trans[i].level == 1 then idx = i; break end
        end
        if not idx then
          trans, base_level = {}, line
          return
        end
        local keep = {}
        for i = idx, #trans do keep[#keep + 1] = trans[i] end
        trans, base_level = keep, 0
        rx_start = trans[1].t
      end
      -- the ESP32-S3's clock: its bit is P / (1 + skew) of the console's cycles
      local bit = P / (1 + skew)
      local stop_at = rx_start + 9.5 * bit
      if now < stop_at then return end
      local b = 0
      for k = 0, 7 do
        if level_at(rx_start + (1.5 + k) * bit) == 0 then b = b | (1 << k) end
      end
      local stop_ok = level_at(stop_at) == 0
      local keep = {}
      for _, tr in ipairs(trans) do
        if tr.t > stop_at then keep[#keep + 1] = tr
        elseif tr.t > rx_start then
          -- the ROM's own timing: every change a whole number of bits in
          local dev = (tr.t - rx_start) % P
          dev = math.min(dev, P - dev)
          if dev > st.tx_dev then st.tx_dev = dev end
        end
      end
      base_level = level_at(stop_at)
      trans = keep
      rx_start = nil
      if stop_ok then
        local frame = feed(b)
        if frame then send(frame, stop_at + turnaround) end
      else
        st.framing = st.framing + 1
      end
    end
  end

  local function tx_level(t)
    if not tx_start or t < tx_start then return 0 end
    local i = math.floor((t - tx_start) * (1 + skew) / P)
    local n = i // L.LINK_TO_NES_BITS + 1
    local bit = i % L.LINK_TO_NES_BITS
    if n > #tx_bytes then
      tx_start = nil
      return 0
    end
    if bit == 0 then return 1 end
    if bit <= 8 then return ((tx_bytes[n] >> (bit - 1)) & 1) == 0 and 1 or 0 end
    return 0
  end

  local function now() return emu.getState()["cpu.cycleCount"] end

  emu.addMemoryCallback(function(_, value)
    local t = now()
    rx_advance(t)
    local lvl = (value >> 1) & 1
    if lvl ~= line then
      trans[#trans + 1] = { t = t, level = lvl }
      line = lvl
    end
  end, emu.callbackType.write, 0x4016, 0x4016)

  emu.addMemoryCallback(function(_, value)
    -- The ROM's hunt reads this every 6 cycles; with nothing on either wire
    -- the line is at mark, and asking Mesen the time would only be slow.
    if not tx_start and not rx_start and #trans == 0 then return value & ~0x02 end
    local t = now()
    rx_advance(t)
    return (value & ~0x02) | (tx_level(t) << 1)
  end, emu.callbackType.read, 0x4017, 0x4017)

  function esp:report()
    local s = st
    return string.format("requests=%d replies=%d repeats=%d hellos=%d noise=%d framing=%d "
      .. "dropped=%d corrupted=%d taken=%d lost=%d rom-tx-deviation=%d cycles",
      s.requests, s.replies, s.repeats, s.hellos, s.noise, s.framing,
      s.dropped, s.corrupted, s.taken, s.lost, s.tx_dev)
  end

  return esp
end

M.script_dir = script_dir
return M
