-- Quire acceptance (Phase 3): load a stock CLAP instrument in REAPER, play a
-- phrase, render it, open the editor. Driven by tools/reaper/load-test.sh.
local out = (os.getenv("QUIRE_REAPER_OUT") or "/tmp/quire-reaper") .. "/"
local fxName = os.getenv("QUIRE_REAPER_FX") or "Orrery"
local log = io.open(out .. "result.txt", "w")
local function say(s) log:write(s .. "\n"); log:flush() end

reaper.Main_OnCommand(40023, 0) -- File: New project
reaper.InsertTrackAtIndex(0, true)
local tr = reaper.GetTrack(0, 0)
local fx = reaper.TrackFX_AddByName(tr, "CLAP:" .. fxName, false, -1)
say("fx_index=" .. fx)
if fx < 0 then say("FAIL: " .. fxName .. " not found"); log:close(); return end
local _, name = reaper.TrackFX_GetFXName(tr, fx, "")
say("fx_name=" .. name)
say("params=" .. reaper.TrackFX_GetNumParams(tr, fx))
local _, pname = reaper.TrackFX_GetParamName(tr, fx, 0, "")
say("param0=" .. pname)

-- A two-bar phrase: C3 E3 G3 C4 then a chord.
local item = reaper.CreateNewMIDIItemInProj(tr, 0, 4, false)
local take = reaper.GetActiveTake(item)
local ppq = function(sec) return reaper.MIDI_GetPPQPosFromProjTime(take, sec) end
local notes = {{0, 0.45, 48}, {0.5, 0.95, 52}, {1.0, 1.45, 55}, {1.5, 1.95, 60},
               {2.0, 3.5, 48}, {2.0, 3.5, 55}, {2.0, 3.5, 64}}
for _, n in ipairs(notes) do
  reaper.MIDI_InsertNote(take, false, false, ppq(n[1]), ppq(n[2]), 0, n[3], 100, true)
end
reaper.MIDI_Sort(take)

-- Render the project to a WAV with the most recent settings path overridden.
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", out, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "render", true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 5, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)  -- custom time bounds
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)  -- WAV
reaper.Main_OnCommand(42230, 0) -- Render project, most recent settings, auto-close
say("render_done=1")

-- Open the editor (floating) for the screenshot, then keep REAPER alive briefly.
reaper.TrackFX_Show(tr, fx, 3)
say("editor_open=" .. tostring(reaper.TrackFX_GetFloatingWindow(tr, fx) ~= nil))
log:close()
