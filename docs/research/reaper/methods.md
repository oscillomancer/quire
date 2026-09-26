# Local experiment record

Date: 2026-09-26. Host REAPER: `/usr/lib/REAPER/reaper`, version `7.80/linux-x86_64` as written in the saved project. Research used only REAPER's command line, ReaScript API, generated RPP files, `reaper -h`, the bundled *REAPER Quick Start* PDF, and read-only user config. No REAPER binary, theme, icon or proprietary source was inspected.

## Temporary configuration and runs

- Copied `~/.config/REAPER/reaper.ini` to `/tmp/reaper-ux-cfg/reaper.ini`; REAPER made other resource files only inside that temporary config directory. Nate's `~/.config/REAPER` was not written.
- `reaper -h` printed `-cfgfile`, `-renderproject`, `-saveas`, `-newinst`, `-nosplash`, `-close`, plus batch-converter `NORMALIZE` modes 1 peak, 2 true peak, 3 LUFS-I, 4 LUFS-S, 5 LUFS-M, 6 RMS and `BRICKWALL` modes peak/true peak.
- Ran `timeout 15s reaper -newinst -nosplash -cfgfile /tmp/reaper-ux-cfg/reaper.ini -renderproject <hand-authored.RPP absolute path>`: exit 0; generated a 529,890-byte WAV.
- Ran REAPER with the hand-authored project and `-saveas <normalized.RPP>`: file appeared, but the GUI instance did not exit within eight seconds and was terminated by `timeout` (exit 124). [normalized.RPP](normalized.RPP) records the result. It is a saved normalization example, not a clean process-exit result. It was saved before a later Lua smoke test altered the hand-authored working copy; [hand-authored.RPP](hand-authored.RPP) restores the original source.
- Ran REAPER with the normalized project and a temporary Lua script, using the same `-newinst -nosplash -cfgfile` flags. The script called `Main_SaveProject(0,false)` and `Main_OnCommand(40004,0)`; exit 0 and the project contained eight tracks. The REAPER-saved result became [generated.RPP](generated.RPP), with only the audio `FILE` path changed to `tone.wav` and the temporary `RENDER_FILE` cleared for portability.
- Ran `-renderproject` on the generated project with those two paths temporarily restored to absolute `/tmp`/workspace paths in an unretained temporary copy: exit 0, output `/tmp/reaper-ux-check.wav`, 529,890 bytes, and no surviving REAPER process. The render log had two LV2 warnings and `error creating client`, but offline render completed.

## ReaScript construction calls

The temporary Lua script used these public ReaScript functions and parameters. These calls explain the observable fields in `generated.RPP`; the script was not retained because it contained a workstation-specific absolute media path.

| Intent | Exact call shape |
|---|---|
| Eight ordered tracks | `InsertTrackAtIndex(i-1,true)`, `GetTrack(0,i-1)`, `GetSetMediaTrackInfo_String(track,'P_NAME',name,true)`; seven tracks were inserted into the one-track normalized seed. |
| Two folders | `SetMediaTrackInfo_Value(track,'I_FOLDERDEPTH',1)` on Drum bus and Music bus; `-1` on Snare and Pads. |
| Four-channel destination | `SetMediaTrackInfo_Value(sidechainReturn,'I_NCHAN',4)` and `SetMediaTrackInfo_Value(sidechainReturn,'B_MAINSEND',0)`. |
| Three sends | `CreateTrackSend(source,destination)`, then `SetTrackSendInfo_Value(source,0,sendIndex,'I_SENDMODE',mode)` with modes `0`, `1`, `3`; the Kick send also used key `'I_DSTCHAN'` with value `2` to target channels 3/4. |
| Markers and tempo | `AddProjectMarker2(0,false,0.5,0,'Intro',-1,0)`; region `AddProjectMarker2(0,true,1,5,'Verse',-1,0)`; `SetTempoTimeSigMarker(0,-1,time,-1,-1,bpm,num,denom,ramp)` at `(0,120,4,4,false)`, `(4,132,4,4,true)`, `(8,132,3,4,false)`. |
| Two-take audio item | `AddMediaItemToTrack`, `AddTakeToMediaItem` twice, `PCM_Source_CreateFromFile` for both, `SetMediaItemTake_Source`, `SetActiveTake(take2)`; item position `0`, length `2`, fade-in `0.1`, fade-out `0.25`, shape fields `1` and `2`; take rates `1.25`/`0.8`, `B_PPITCH` `1`/`0`; `SetTakeStretchMarker(take2,-1,0.5,0.4)`. |
| MIDI | `CreateNewMIDIItemInProj(track,0,2,false)`, `MIDI_InsertNote(take,false,false,0,960,0,48,100,false)`, `MIDI_InsertCC(take,false,false,0,176,0,1,64)`, `MIDI_Sort`. |
| FX | `TrackFX_AddByName(track,'ReaComp (Cockos)',false,-1)` and `TakeFX_AddByName(take2,'ReaEQ (Cockos)',-1)`. |

## Read-only local counts

- `rg -c '^KEY ' ~/.config/REAPER/KeyMaps/*.ReaperKeyMap`: DK 21, German 23. No `~/.config/REAPER/reaper-kb.ini` existed. These are keymap entry counts, not installed-action counts.
- `rg -c '^MM_CTX_' ~/.config/REAPER/reaper-mouse.ini`: 22 context entries.
- `~/.config/REAPER/reaper.ini` contained `autonbworkerthreads=0` and `render_pattern_0=$project`; no explicit `anticipativefx` key was observed.
- The local VST and CLAP cache files were read for line shape only. No interpretation of scanner numbers as isolation flags was made.

## Unrun experiments

The sandbox run did not produce item/take envelopes, comp areas, FX pin maps, hardware outputs, automation items, FX containers, input/monitor FX, custom action exports, or plug-in bridge crash behavior. The main note marks claims about these as `[from knowledge]` and lists follow-up experiments.
