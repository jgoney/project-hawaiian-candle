-- Log every write to the FM chip (YM2612) while MAME runs the original.
--
-- One line per write: "<seconds since power-on> <port 0-3> <value, two hex
-- digits> <z or m: the Z80 or the 68000 wrote it>". The Z80's writes (its addresses 0x4000-0x4003) and any the 68000
-- makes itself (0xA04000-0xA04003) are logged alike, in the order they
-- happen. Environment: RR2_FM_LOG (the file to write). Read-only: nothing in
-- the emulated machine is changed. Compared with the port's log by
-- rr2-fm-compare.

local out_path = os.getenv("RR2_FM_LOG")
if not out_path or out_path == "" then
    error("RR2_FM_LOG must be set")
end

local z80 = manager.machine.devices[":genesis_snd_z80"]
local main = manager.machine.devices[":maincpu"]
if not z80 or not main then
    error("Genesis CPUs not found")
end

local out = assert(io.open(out_path, "w"))
local machine = manager.machine

local function note(port, data, source)
    out:write(string.format("%.9f %d %02X %s\n", machine.time:as_double(), port, data & 0xff, source))
end

-- Kept in globals so the taps are not collected.
rr2_fm_tap_z80 = z80.spaces["program"]:install_write_tap(0x4000, 0x4003, "rr2 fm z80",
    function(offset, data, mask)
        note(offset & 3, data, "z")
    end)
rr2_fm_tap_68k = main.spaces["program"]:install_write_tap(0xa04000, 0xa04003, "rr2 fm 68k",
    function(offset, data, mask)
        -- The 68000's bus is 16 bits wide: the mask says which byte was written.
        if mask & 0xff00 ~= 0 then note(offset & 2, data >> 8, "m") end
        if mask & 0x00ff ~= 0 then note((offset & 2) | 1, data, "m") end
    end)

emu.add_machine_stop_notifier(function()
    out:close()
end)
