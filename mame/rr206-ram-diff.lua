-- Capture RR206 68000 work RAM at each completed emulated frame.
-- Output is an initial 64 KiB baseline plus compact changed-byte records.

local RAM_START = 0xff0000
local RAM_END = 0xffffff
local MAGIC = "RR2DIFF1"

local prefix = os.getenv("RR2_CAPTURE_PREFIX")
if not prefix or prefix == "" then
    error("RR2_CAPTURE_PREFIX is not set")
end

local cpu = manager.machine.devices[":maincpu"]
if not cpu then
    error("Genesis main CPU device not found")
end

local space = cpu.spaces["program"]
if not space then
    error("Genesis main CPU program space not found")
end

local baseline_path = prefix .. ".baseline.bin"
local diff_path = prefix .. ".diffs.bin"
local metadata_path = prefix .. ".meta.txt"

local baseline_file = assert(io.open(baseline_path, "wb"))
local diff_file = assert(io.open(diff_path, "wb"))
local metadata_file = assert(io.open(metadata_path, "w"))

local previous = space:read_range(RAM_START, RAM_END, 8)
if #previous ~= 0x10000 then
    error(string.format("expected 65536 RAM bytes, received %d", #previous))
end

baseline_file:write(previous)
baseline_file:close()
diff_file:write(MAGIC)

local frame_number = 0
local total_changes = 0
local closed = false

metadata_file:write("format=rr2-ram-diff-v1\n")
metadata_file:write("mame_version=" .. emu.app_version() .. "\n")
metadata_file:write("machine=genesis\n")
metadata_file:write("cpu=:maincpu\n")
metadata_file:write(string.format("ram_start=0x%06X\n", RAM_START))
metadata_file:write(string.format("ram_end=0x%06X\n", RAM_END))
metadata_file:write("byte_order=address-order\n")
metadata_file:write("diff_encoding=RR2DIFF1 then repeated big-endian u32 frame, u32 count, count*(u16 offset,u8 value)\n")
metadata_file:flush()

local function capture_frame()
    local current = space:read_range(RAM_START, RAM_END, 8)
    local changes = {}

    for index = 1, #current do
        local value = string.byte(current, index)
        if value ~= string.byte(previous, index) then
            changes[#changes + 1] = string.pack(">I2I1", index - 1, value)
        end
    end

    frame_number = frame_number + 1
    total_changes = total_changes + #changes
    diff_file:write(string.pack(">I4I4", frame_number, #changes))
    if #changes > 0 then
        diff_file:write(table.concat(changes))
    end
    if frame_number % 60 == 0 then
        diff_file:flush()
    end
    previous = current
end

local function close_capture()
    if closed then
        return
    end
    closed = true
    diff_file:flush()
    diff_file:close()
    metadata_file:write(string.format("frames=%d\n", frame_number))
    metadata_file:write(string.format("changed_bytes=%d\n", total_changes))
    metadata_file:close()
    emu.print_info(string.format("RR2 RAM capture complete: %d frames, %d changed bytes", frame_number, total_changes))
end

local frame_subscription = emu.add_machine_frame_notifier(capture_frame)
local stop_subscription = emu.add_machine_stop_notifier(close_capture)

-- Keep notifier subscriptions reachable for the lifetime of the script.
_G.rr2_ram_capture = {
    frame_subscription = frame_subscription,
    stop_subscription = stop_subscription,
}

emu.print_info("RR2 per-frame RAM differential capture started: " .. prefix)
