-- hotd_shots.lua: MAME's The House of the Dead driven by one of our input scripts (scripts/inputs/*.txt, the
-- format tools/common/input_script.h reads), saving the screen at chosen frames: the original game's frames, to
-- compare with ours (scripts/compare_mame.py runs both). Settings by environment variable:
--   HOTD_INPUTS=path      the input script (optional: none is the attract mode)
--   HOTD_SHOTS=f1,f2,...  the frames to save (MAME's snapshot directory, shot_<frame>.png)
--   HOTD_END=frame        exit then (default: the last shot)
--   HOTD_POKE=addr=v,...  bytes of the game's memory written before every frame (hex: 51f729=1,51f72d=0 are the
--                         settings our runs have, USA and red blood; MAME's defaults are Japan and green)
--   HOTD_OFFSET=n         the script's frame n is MAME's frame n + this (our frames start later: about 235)
-- mame hotd -autoboot_script tools/mame-plugins/hotd_shots.lua -video none -sound none -nothrottle -snapshot_directory DIR

local lines = {}
local path = os.getenv("HOTD_INPUTS")
if path and path ~= "" then
	for l in io.lines(path) do
		local a, b, name, value = l:match("^(%d+)%-(%d+)%s+([%w_]+)=(%S+)")
		if not a then
			a, name, value = l:match("^(%d+)%s+([%w_]+)=(%S+)")
			b = a
		end
		if a and not name:match("^poke") then
			lines[#lines + 1] = { from = tonumber(a), to = tonumber(b), name = name, value = tonumber(value) }
		end
	end
end

local shots, want = {}, {}
for f in (os.getenv("HOTD_SHOTS") or ""):gmatch("%d+") do shots[#shots + 1] = tonumber(f); want[tonumber(f)] = true end
local last = 0
for _, f in ipairs(shots) do if f > last then last = f end end
local stop = tonumber(os.getenv("HOTD_END") or "") or last

local ports = manager.machine.ioport.ports
local function field(port, name) return ports[port] and ports[port].fields[name] end
-- our names: the script's (input_script.h), MAME's fields
local digital = {
	coin = field(":IN0", "Coin 1"), start = field(":IN0", "1 Player Start"), start2 = field(":IN0", "2 Players Start"),
	trigger = field(":IN1", "P1 Trigger"), trigger2 = field(":IN1", "P2 Trigger"), service = field(":IN0", "Service 1"),
	test = field(":IN0", "Service Mode"),
}
local analog = {
	gunx = field(":P1_X", "Lightgun X"), guny = field(":P1_Y", "Lightgun Y"),
	gun2x = field(":P2_X", "Lightgun X 2"), gun2y = field(":P2_Y", "Lightgun Y 2"),
}

local offset = tonumber(os.getenv("HOTD_OFFSET") or "") or 0
local pokes = {}
for a, v in (os.getenv("HOTD_POKE") or ""):gmatch("(%x+)=(%x+)") do pokes[#pokes + 1] = { tonumber(a, 16), tonumber(v, 16) } end
local program = manager.machine.devices[":maincpu"].spaces["program"]
local screen = manager.machine.screens[":screen"]
local function apply(frame)
	for _, p in ipairs(pokes) do program:write_u8(p[1], p[2]) end
	frame = frame - offset
	local values = {}
	for _, l in ipairs(lines) do
		if frame >= l.from and frame <= l.to then values[l.name] = l.value end
	end
	for name, f in pairs(digital) do f:set_value(values[name] or 0) end
	for name, f in pairs(analog) do if values[name] then f:set_value(values[name]) end end
end

apply(0)
emu.register_frame_done(function()
	local frame = screen:frame_number()
	if want[frame] then
		want[frame] = nil
		manager.machine.video:snapshot()
		print(string.format("hotd_shots: frame %d saved", frame))
	end
	if frame >= stop then manager.machine:exit() end
	apply(frame + 1)
end)
