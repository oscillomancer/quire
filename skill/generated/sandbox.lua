---@meta
-- Quire 0.1.0 scripting reference, generated from the command registry.
-- Do not edit: `<slug>-engine registry --lua` writes it, tools/gen/update-generated.sh regenerates it.
-- The `q` table of a script (libs/lua-bus, ADR-0006): every command is a
-- function, `q.run(name, args)` reaches any of them by name, `q.batch(label, body)`
-- is one transaction, and `q.on(topic, handler)` subscribes to bus events.

---@type Quire
q = nil

---@class Quire.analyze_args
---@field source string A render id (rd_1) or an audio file path.
---@field detail? "summary"|"standard"|"full" summary: key scalars; standard: + percentiles and spectrum; full: + time series. (default: "summary")
---@field note_off_s? number

---@class Quire.analyze_result
---@field report any
---@field summary string

---@class Quire.compare_args
---@field reference string A render id (rd_1) or an audio file path.
---@field test string A render id (rd_1) or an audio file path.
---@field reference_note_off_s? number The reference's note-off; defaults to a render's input end.
---@field test_note_off_s? number The test's note-off; defaults to a render's input end.

---@class Quire.compare_result
---@field notable any
---@field ltas_rms_db number
---@field summary string
---@field loss any The score the optimizer minimizes: total, per-term contributions, and the note-off_s used per side.

---@class Quire.dsp_check_args
---@field name? string
---@field source? string

---@class Quire.dsp_check_result
---@field ok boolean
---@field diagnostics table[]
---@field plugin? table

---@class Quire.dsp_export_args
---@field script string a name from dsp.list
---@field format? "clap"|"source" clap (default): a plugin file; source: a C++ project
---@field folder? string an absolute folder to write to (for clap, ~/.clap installs it for this user); for source, empty or new
---@field license? "MIT"|"GPL-3.0-or-later" source: the license of the project's own files (default MIT)

---@class Quire.dsp_export_result
---@field path string the plugin file, or the project's folder
---@field id string
---@field name string
---@field format string clap or source
---@field player? string clap: the script player it was copied from
---@field bytes? integer
---@field notices? string clap: the licenses to ship with the plugin

---@class Quire.dsp_get_args
---@field name string

---@class Quire.dsp_get_result
---@field name string
---@field path string
---@field source string

---@class Quire.dsp_list_result
---@field library string
---@field scripts table[]

---@class Quire.dsp_preview_args
---@field plugin? string
---@field script? string
---@field patch? any
---@field signal? "sine"|"sweep"|"impulse"|"noise"|"silence"|"notes"|"note"
---@field level_db? number|string Quantity strings are range-checked by the server. (default: -12.0)
---@field frequency_hz? number|string Quantity strings are range-checked by the server. (default: 1000.0)
---@field seconds? number (default: 1.0)
---@field scale? number (default: 1.0)
---@field pointer? table[]

---@class Quire.dsp_preview_result
---@field plugin string
---@field path string the PNG
---@field width integer pixels
---@field height integer pixels
---@field edits any what pointer changed, as a merge patch
---@field problems string[]
---@field log string[]

---@class Quire.dsp_test_args
---@field plugin? string
---@field script? string
---@field patch? any
---@field signals? "sine"|"sweep"|"impulse"|"noise"|"silence"|"notes"|"note"[] default: effects five; instruments three, or notes and silence with custom notes
---@field automation? table[] discrete physical-value changes on every signal
---@field sidechain? table timed generated key input
---@field notes? table[] instrument sequence replacing the notes stimulus
---@field level_db? number|string Quantity strings are range-checked by the server. (default: -12.0)
---@field frequency_hz? number|string Quantity strings are range-checked by the server. (default: 1000.0)
---@field seconds? number (default: 1.0)
---@field save? boolean (default: false)

---@class Quire.dsp_test_result
---@field plugin string
---@field latency integer samples
---@field signals table[]
---@field cpu_percent number of real time, measured
---@field problems string[]
---@field log string[]

---@class Quire.dsp_write_args
---@field name string the script's folder and file name
---@field source string the whole C++ file

---@class Quire.dsp_write_result
---@field name string
---@field path string
---@field ok boolean it compiled
---@field diagnostics table[]
---@field plugin? table
---@field listed boolean plugin.list shows it (the script player is on the plugin path)

---@class Quire.job_cancel_args
---@field job string

---@class Quire.job_get_args
---@field job string

---@class Quire.job_status_args
---@field job string

---@class Quire.job_wait_args
---@field job string
---@field timeout_s? number Seconds to block (default 30). A larger value is clamped to 30 and reported as timeout_clamped_s.

---@class Quire.masking_args
---@field masker string A render id (rd_1) or an audio file path.
---@field maskee string A render id (rd_1) or an audio file path.

---@class Quire.masking_result
---@field result any

---@class Quire.optimize_start_args
---@field instance string
---@field params? any "auto" or a list of keys / {key, min, max}. (default: "auto")
---@field target any {reference?, profile?}
---@field phrase? string
---@field seconds? number
---@field tail_s? number
---@field evaluations? integer
---@field max_seconds? number
---@field seed? integer (default: 0)

---@class Quire.optimize_start_result
---@field job string
---@field status string
---@field params string[]
---@field render string

---@class Quire.param_get_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field keys? string[]

---@class Quire.param_get_result
---@field values any

---@class Quire.param_set_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field values any Key -> value.

---@class Quire.param_set_result
---@field values any

---@class Quire.patch_get_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field detail? "changed"|"full" (default: "changed")

---@class Quire.patch_get_result
---@field patch any

---@class Quire.patch_merge_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field patch any

---@class Quire.patch_merge_result
---@field changed any
---@field description string

---@class Quire.patch_set_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field patch any

---@class Quire.patch_set_result
---@field description string

---@class Quire.plugin_describe_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field detail? "summary"|"full" (default: "summary")

---@class Quire.plugin_describe_result
---@field plugin string
---@field name string
---@field params any
---@field patch_type? string
---@field patch_schema? any
---@field description string
---@field docs? string
---@field status_available boolean The instance supplied its current problems
---@field problems? any Current plugin problems: code, message, scope, line when known, hint, silent; scope host when unreadable

---@class Quire.plugin_list_args
---@field filter? string Text to match in the id or name.
---@field rescan? boolean (default: false)

---@class Quire.plugin_list_result
---@field plugins table[]
---@field scan table

---@class Quire.plugin_load_args
---@field plugin string Plugin id or .clap path.
---@field position? integer Chain index (0 = first).

---@class Quire.plugin_load_result
---@field instance string
---@field name string
---@field semantic boolean
---@field chain string[]

---@class Quire.plugin_remove_args
---@field instance string Instance id from plugin.load, e.g. "in_1".

---@class Quire.plugin_remove_result
---@field chain string[]

---@class Quire.preset_list_args
---@field plugin? string
---@field source? "user"|"factory" (default: "user")

---@class Quire.preset_list_result
---@field presets table[]

---@class Quire.preset_load_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field name string
---@field source? "user"|"factory" (default: "user")

---@class Quire.preset_load_result
---@field description string

---@class Quire.preset_save_args
---@field instance string Instance id from plugin.load, e.g. "in_1".
---@field name string
---@field tags? string[]
---@field notes? string

---@class Quire.preset_save_result
---@field path string

---@class Quire.profile_evaluate_args
---@field source string A render id (rd_1) or an audio file path.
---@field profile any

---@class Quire.profile_evaluate_result
---@field result any

---@class Quire.registry_describe_args
---@field command string

---@class Quire.registry_list_args
---@field tag? string
---@field kind? string
---@field text? string

---@class Quire.render_args
---@field notation? string Notation text.
---@field midi_file? string
---@field audio_file? string
---@field bars? number
---@field seconds? number
---@field tempo? number|string Quantity strings are range-checked by the server.
---@field tail_s? number
---@field name? string
---@field analyze? "none"|"summary"|"standard"|"full" Also analyze the render (see the analyze command); summary is compact (≤ 400 tokens). (default: "none")

---@class Quire.render_result
---@field render string
---@field path string
---@field seconds number
---@field peak_dbfs number
---@field tail_s number
---@field tail_capped boolean
---@field latency_samples integer
---@field warnings? string[]
---@field log? string[]
---@field analysis? any

---@class Quire.script_run_args
---@field code string The Lua source. It may return a value: what it returns is converted to JSON (tables with string keys are objects, sequences are arrays).
---@field name? string The script's name: its commands are recorded as `lua:<name>` and its state (globals, subscriptions) is kept between runs of the same name. Defaults to the caller's client name.
---@field budget_ms? number Instruction budget of this run; the script is stopped with `script.budget` when it runs out. Time spent inside a command does not count.
---@field args? any Converted to the global table `args`.

---@class Quire.script_run_result
---@field result any What the script returned (null if it returned nothing).
---@field log string[] What the script printed.
---@field script string The name it ran under.
---@field instructions integer Lua instructions the run executed.

---@class Quire.session_get_result
---@field settings any
---@field chain any
---@field instances any
---@field renders any

---@class Quire.session_set_args
---@field sample_rate? number|string Quantity strings are range-checked by the server.
---@field block_size? integer
---@field tempo? number|string Quantity strings are range-checked by the server.
---@field meter? string
---@field tail_limit_s? number

---@class Quire.session_set_result
---@field settings any

---@class Quire.dsp
---@field check fun(args?: Quire.dsp_check_args): Quire.dsp_check_result Compile a DSP script without saving it; return its diagnostics.
---@field export fun(args: Quire.dsp_export_args): Quire.dsp_export_result Export a DSP script as a plugin for other DAWs: a CLAP file, or C++ source.
---@field get fun(args: Quire.dsp_get_args): Quire.dsp_get_result Read a DSP script from the library.
---@field list fun(): Quire.dsp_list_result List the DSP scripts in the library, compiled, with their plugin ids.
---@field preview fun(args?: Quire.dsp_preview_args): Quire.dsp_preview_result Draw a plugin's editor to a PNG, after a test signal plays through it.
---@field test fun(args?: Quire.dsp_test_args): Quire.dsp_test_result Play test signals or notes through a plugin and measure it: levels, THD, pitch.
---@field write fun(args: Quire.dsp_write_args): Quire.dsp_write_result Save a DSP script to the library and compile it, with diagnostics.

---@class Quire.job
---@field cancel fun(args: Quire.job_cancel_args): table Cancel a job.
---@field get fun(args: Quire.job_get_args): table Get a job's status and progress.
---@field list fun(): table List session jobs.
---@field status fun(args: Quire.job_status_args): table Get a job's status.
---@field wait fun(args: Quire.job_wait_args): table Wait for a job.

---@class Quire.optimize
---@field start fun(args: Quire.optimize_start_args): Quire.optimize_start_result Tune continuous parameters toward a reference sound or a profile (a job).

---@class Quire.param
---@field get fun(args: Quire.param_get_args): Quire.param_get_result Read parameter values (physical value and display text).
---@field set fun(args: Quire.param_set_args): Quire.param_set_result Set parameters in physical units; returns the values as applied.

---@class Quire.patch
---@field get fun(args: Quire.patch_get_args): Quire.patch_get_result Get an instance's patch document (changed values or all).
---@field merge fun(args: Quire.patch_merge_args): Quire.patch_merge_result Merge a partial patch (RFC 7396) into an instance.
---@field set fun(args: Quire.patch_set_args): Quire.patch_set_result Replace an instance's whole patch; returns a description of the result.

---@class Quire.plugin
---@field describe fun(args: Quire.plugin_describe_args): Quire.plugin_describe_result Describe an instance: parameters, patch format, and current problems.
---@field list fun(args?: Quire.plugin_list_args): Quire.plugin_list_result List the CLAP plugins the sandbox can load.
---@field load fun(args: Quire.plugin_load_args): Quire.plugin_load_result Load a plugin into the chain and return its instance id.
---@field remove fun(args: Quire.plugin_remove_args): Quire.plugin_remove_result Remove a plugin instance from the chain.

---@class Quire.preset
---@field list fun(args?: Quire.preset_list_args): Quire.preset_list_result List user presets, or a plugin's read-only factory patches.
---@field load fun(args: Quire.preset_load_args): Quire.preset_load_result Load a named preset into an instance.
---@field save fun(args: Quire.preset_save_args): Quire.preset_save_result Save an instance's current settings as a named preset.

---@class Quire.profile
---@field evaluate fun(args: Quire.profile_evaluate_args): Quire.profile_evaluate_result Score a render against a target descriptor profile.

---@class Quire.registry
---@field describe fun(args: Quire.registry_describe_args): table Describe a command.
---@field list fun(args?: Quire.registry_list_args): table List available commands.

---@class Quire.script
---@field run fun(args: Quire.script_run_args): Quire.script_run_result Run a Lua script on the command bus.

---@class Quire.session
---@field get fun(): Quire.session_get_result Show the session: settings, chain, instances and renders.
---@field reset fun(): table Unload every plugin, delete renders and restore default settings.
---@field set fun(args?: Quire.session_set_args): Quire.session_set_result Change session settings (rate, block size, tempo, meter, tail limit).

---@class Quire
---@field dsp Quire.dsp Commands named dsp.*
---@field job Quire.job Commands named job.*
---@field optimize Quire.optimize Commands named optimize.*
---@field param Quire.param Commands named param.*
---@field patch Quire.patch Commands named patch.*
---@field plugin Quire.plugin Commands named plugin.*
---@field preset Quire.preset Commands named preset.*
---@field profile Quire.profile Commands named profile.*
---@field registry Quire.registry Commands named registry.*
---@field script Quire.script Commands named script.*
---@field session Quire.session Commands named session.*
---@field analyze fun(args: Quire.analyze_args): Quire.analyze_result Analyze a render or audio file: loudness, spectrum, pitch, envelope, stereo.
---@field compare fun(args: Quire.compare_args): Quire.compare_result Compare two renders or files: notable descriptor changes and timbre distance.
---@field masking fun(args: Quire.masking_args): Quire.masking_result Estimate where one sound masks another (critical bands).
---@field render fun(args?: Quire.render_args): Quire.render_result Render notes, a MIDI file or an audio file through the chain to a WAV.
---@field run fun(name: string, args?: table): any Run any registered command by name, e.g. the ones whose names are not Lua identifiers.
---@field batch fun(label: string, body: fun()) Run the calls inside body as one transaction: one undo step, rolled back when one of them fails.
---@field on fun(topic: string, handler: fun(payload: table, topic: string), filter?: table): integer Subscribe to bus events; returns a handle for q.off.
---@field off fun(handle: integer) Drop a subscription.
---@field log fun(...) Print a line to the script's log (the same as print).
---@field null any The JSON null: Lua tables cannot hold nil.
---@field name string The name this script runs under (its origin is `lua:<name>`).
