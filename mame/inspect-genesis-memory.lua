-- Inventory memory objects exposed by MAME's Genesis driver for asset capture.

local done = false

local function inspect_memory()
    if done then
        return
    end
    done = true

    for tag, share in pairs(manager.machine.memory.shares) do
        emu.print_info(string.format(
            "RR2MEM,share,%s,size=%d,width=%d,endian=%s",
            tag,
            share.size,
            share.bitwidth,
            share.endianness
        ))
    end
    for tag, region in pairs(manager.machine.memory.regions) do
        emu.print_info(string.format(
            "RR2MEM,region,%s,size=%d,width=%d,endian=%s",
            tag,
            region.size,
            region.bitwidth,
            region.endianness
        ))
    end
    for tag, device in pairs(manager.machine.devices) do
        for name, space in pairs(device.spaces) do
            emu.print_info(string.format(
                "RR2MEM,space,%s,%s,width=%d,endian=%s,shift=%d,mask=%X",
                tag,
                name,
                space.data_width,
                space.endianness,
                space.shift,
                space.address_mask
            ))
            if space.map then
                for index, entry in ipairs(space.map.entries) do
                    emu.print_info(string.format(
                        "RR2MEM,map,%s,%s,%d,%X-%X,read=%s,write=%s,share=%s",
                        tag,
                        name,
                        index,
                        entry.address_start,
                        entry.address_end,
                        entry.read.handlertype,
                        entry.write.handlertype,
                        tostring(entry.share)
                    ))
                end
            end
        end
    end
    manager.machine:exit()
end

_G.rr2_memory_inventory_subscription = emu.add_machine_frame_notifier(inspect_memory)
