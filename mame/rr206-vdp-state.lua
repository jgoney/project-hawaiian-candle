-- Dump MAME's video chip memory and work RAM once the debugger has stopped at
-- the VBlank handler's first instruction.
--
-- Used with a debugger script from rr2port.mame_cmd.build_stop_cmd, which
-- stops at the Nth handler entry; counting entries from Lua proved unreliable.
-- Environment: RR2_STATE_OUT (video state file) and, optionally,
-- RR2_STATE_RAM_OUT (64 KiB of work RAM). The video state file is
-- "RR2VDPSTATE1\n", 24 register bytes, 65,536 bytes of VRAM, 64 big-endian
-- CRAM words and 40 big-endian VSRAM words, which
-- port/tools/rr_vdp_render_tool.c can render. Read-only: nothing in the
-- emulated machine is changed.

local VBLANK_HANDLER = 0x11b8

local out_path = os.getenv("RR2_STATE_OUT")
if not out_path or out_path == "" then
    error("RR2_STATE_OUT must be set")
end

local cpu = manager.machine.devices[":maincpu"]
local vdp = manager.machine.devices[":gen_vdp"]
if not cpu or not vdp then
    error("Genesis main CPU or video chip device not found")
end
local space = cpu.spaces["program"]

local function item(name)
    local id = vdp.items["0/" .. name]
    if not id then
        error("video chip state item not found: " .. name)
    end
    return emu.item(id)
end

local function words_big_endian(name, count)
    local source = item(name)
    local parts = {}
    for index = 0, count - 1 do
        local value = source:read(index)
        parts[#parts + 1] = string.char((value >> 8) & 0xff, value & 0xff)
    end
    return table.concat(parts)
end

local function dump()
    local file = assert(io.open(out_path, "wb"))
    file:write("RR2VDPSTATE1\n")
    local registers = item("m_regs")
    local parts = {}
    for index = 0, 23 do
        parts[#parts + 1] = string.char(registers:read(index) & 0xff)
    end
    file:write(table.concat(parts))
    file:write(words_big_endian("m_vram", 32768))
    file:write(words_big_endian("m_cram", 64))
    file:write(words_big_endian("m_vsram", 40))
    file:close()
    local ram_path = os.getenv("RR2_STATE_RAM_OUT")
    if ram_path and ram_path ~= "" then
        local ram = assert(io.open(ram_path, "wb"))
        ram:write(space:read_range(0xff0000, 0xffffff, 8))
        ram:close()
    end
end

local finished = false

rr2_state_poll = emu.register_periodic(function()
    if finished then
        return
    end
    local debugger = manager.machine.debugger
    if debugger and debugger.execution_state == "stop"
        and (cpu.state["CURPC"].value & 0xffffff) == VBLANK_HANDLER then
        dump()
        finished = true
        manager.machine:exit()
    end
end)
