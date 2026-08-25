-- Frontend witness for the PPSSPP package: run the core inside Chimera for a
-- fixed number of frames with nothing pressed, then dump a slice of user RAM
-- (the PSP's RAM domain is 64MB; the slice at 0x800000 covers where the
-- executable loads). The driver compares that dump byte-for-byte against the
-- native reference (run-native --ram-slice).
--
-- Job description comes from the file named by the MINIHAWK_JOB env var:
--   frames=<how many frames to advance>
--   out=<path to write the RAM slice (binary)>
--   meta=<path to write result metadata (text)>
--   shot=<optional path to write a screenshot>

local SLICE_OFF = 0x800000
local SLICE_LEN = 0x10000

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local meta = {}
local function finish(status, detail)
	local lines = {
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. (meta.frames or 0),
		"lag=" .. (meta.lag or 0),
		"ramsize=" .. (meta.ramsize or 0),
	}
	if meta.metaPath then
		writeAll(meta.metaPath, table.concat(lines, "\n") .. "\n")
	end
	client.exit()
end

local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB env var not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end
meta.metaPath = job.meta

if emu.getsystemid() ~= "PSP" then
	finish("ERROR", "wrong system id: " .. tostring(emu.getsystemid()))
end
if emu.getcorename() ~= "PPSSPP" then
	finish("ERROR", "wrong core: " .. tostring(emu.getcorename()))
end

pcall(function() client.speedmode(6400) end)
pcall(function() client.invisibleemulation(true) end)

local frames = tonumber(job.frames) or 120
for _ = 1, frames do
	emu.frameadvance()
end

meta.frames = emu.framecount()
meta.lag = emu.lagcount()
-- the RAM domain's size is machine-shaping state (32MB PSP-1000 vs 64MB
-- Slim), which is how the settings check sees that a sync setting arrived
pcall(function()
	memory.usememorydomain("RAM")
	meta.ramsize = memory.getcurrentmemorydomainsize()
end)

if job.shot ~= nil and job.shot ~= "" then
	client.screenshot(job.shot)
end

local ram = memory.read_bytes_as_array(SLICE_OFF, SLICE_LEN, "RAM")
local chunks = {}
for i = 1, #ram do
	chunks[i] = string.char(ram[i])
end
writeAll(job.out, table.concat(chunks))

finish("OK", "")
