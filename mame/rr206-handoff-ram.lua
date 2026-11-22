-- Save work RAM each time MAME's debugger stops at the game-loop handoff.
--
-- Used with the debugger script from rr2port.mame_cmd.build_handoff_stop_cmd,
-- which stops at every execution of 0xDF48. This script appends the RAM as
-- changes from the previous handoff (format: rr2port/handoffs.py), writes the
-- timestep about to be stored (low word of D1) to a text file, and resumes.
-- Environment: RR2_HANDOFF_OUT (the .bin file; ".timesteps" is added for the
-- text file). Read-only: nothing in the emulated machine is changed.

local HANDOFF = 0xdf48
local RAM_SIZE = 0x10000

local out_path = os.getenv("RR2_HANDOFF_OUT")
if not out_path or out_path == "" then
    error("RR2_HANDOFF_OUT must be set")
end

local cpu = manager.machine.devices[":maincpu"]
if not cpu then
    error("Genesis main CPU device not found")
end
local space = cpu.spaces["program"]

local out = assert(io.open(out_path, "wb"))
out:write("RR2HAND1")
local timesteps = assert(io.open(out_path .. ".timesteps", "w"))

local previous = string.rep("\0", RAM_SIZE)
local index = 0
local last_time = -1.0

local function save_handoff()
    local ram = space:read_range(0xff0000, 0xffffff, 8)
    if #ram ~= RAM_SIZE then
        error(string.format("expected %d RAM bytes, received %d", RAM_SIZE, #ram))
    end
    local changes = {}
    for offset = 1, RAM_SIZE do
        local value = ram:byte(offset)
        if value ~= previous:byte(offset) then
            changes[#changes + 1] = string.pack(">I2B", offset - 1, value)
        end
    end
    out:write(string.pack(">I4I4", index, #changes))
    out:write(table.concat(changes))
    out:flush()
    timesteps:write(string.format("%d %d\n", index, cpu.state["D1"].value & 0xffff))
    timesteps:flush()
    previous = ram
    index = index + 1
end

rr2_handoff_poll = emu.register_periodic(function()
    local debugger = manager.machine.debugger
    if not debugger or debugger.execution_state ~= "stop" then
        return
    end
    if (cpu.state["CURPC"].value & 0xffffff) ~= HANDOFF then
        return
    end
    -- The callback can run again before the debugger has resumed; emulated
    -- time has not moved in that case.
    local now = manager.machine.time:as_double()
    if now == last_time then
        return
    end
    last_time = now
    save_handoff()
    debugger:command("g")
end)
