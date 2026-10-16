// Asset authoring tools (ROADMAP M-DATA-REACH step 1): every JSON source asset the cook reads
// (.mcpart, .mcsound, .mcseq, .mctilemap, .mcsheet, .mcanim, .mcmat, .mcinput, .mcstore, .mcservices, .mcui, .mcconfig, .mcterrain) can be
// created, read, rewritten and described over MCP, and a tilemap painted, without the client
// writing files itself. A document is cooked in memory with the project's importer before
// anything is written (cook_document: the same reference and value checks as cook_run), so a
// failed call leaves the file as it was. These native assets carry their GUID inline: a new file
// gets a fresh one, a rewrite keeps it.

#include <cstring>
#include <utility>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/editor/tilemap_editor.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxDiagnostics = 20;
constexpr u32 kMaxPaintOps = 65536;

bool fail(ToolCall& c, StringView msg) {
    c.error += msg;
    return false;
}

cook::JsonValue arg(const ToolCall& c, const char* name) { return c.args.is_object() ? c.args.get(name) : cook::JsonValue(); }

bool arg_string(ToolCall& c, const char* name, StringView& out, bool required) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid() && !required) return true;
    if (!v.is_string()) {
        c.error += "argument '";
        c.error += name;
        c.error += "' must be a string";
        return false;
    }
    out = v.as_string();
    return true;
}

// ---------------------------------------------------------------- the asset kinds

// One JSON source type. `fields` is a JSON array of {name, type, default?, help}; `tmpl` is the
// object asset_create starts from (format, version and guid are added). Keep both in step with
// the importer (src/engine/asset/cook/src/*_importer.cpp): the tests create every kind from its
// template and cook it.
struct AssetKind {
    const char* type;  // the asset_list type name
    const char* ext;
    const char* format;
    u32 version;
    const char* summary;
    const char* fields;
    const char* tmpl;
};

constexpr AssetKind kKinds[] = {
    {"ParticlePreset", ".mcpart", "mce-particles", 1,
     "A 2D particle emitter preset, played by a ParticleEmitter component (preset: \"guid:...\"). Ranges are [min, max].",
     R"json([
{"name":"name","type":"string","help":"Display name"},
{"name":"texture","type":"guid ref | null","default":null,"help":"A Texture asset (\"guid:<uuid>\"); null draws soft white squares"},
{"name":"rate","type":"number","default":0,"help":"Particles per second (>= 0)"},
{"name":"burst","type":"integer","default":0,"help":"Particles emitted at once when the emitter starts (0-65536)"},
{"name":"max_particles","type":"integer","default":256,"help":"Live particle cap (1-65536)"},
{"name":"duration","type":"number","default":0,"help":"Seconds the emitter runs; 0 = forever"},
{"name":"lifetime","type":"[min, max]","default":[1,1],"help":"Seconds; max > 0"},
{"name":"speed","type":"[min, max]","default":[1,1],"help":"Metres per second"},
{"name":"direction","type":"number","default":90,"help":"Degrees; 90 = up"},
{"name":"spread","type":"number","default":0,"help":"Degrees around direction"},
{"name":"gravity","type":"[x, y]","default":[0,0],"help":"Metres per second squared"},
{"name":"drag","type":"number","default":0,"help":"Velocity damping per second"},
{"name":"size","type":"[start, end]","default":[0.25,0.25],"help":"Metres, over the lifetime"},
{"name":"spin","type":"[min, max]","default":[0,0],"help":"Degrees per second"},
{"name":"area","type":"[w, h]","default":[0,0],"help":"Spawn box around the emitter, metres"},
{"name":"color_start","type":"[r, g, b, a]","default":[1,1,1,1]},
{"name":"color_end","type":"[r, g, b, a]","default":[1,1,1,1]},
{"name":"blend","type":"\"alpha\" | \"additive\"","default":"alpha"}])json",
     R"json({"name":"particles","texture":null,"rate":20,"burst":0,"max_particles":256,"duration":0,"lifetime":[0.5,1],"speed":[1,2],"direction":90,"spread":30,"gravity":[0,0],"drag":0,"size":[0.25,0.05],"spin":[0,0],"area":[0,0],"color_start":[1,1,1,1],"color_end":[1,1,1,0],"blend":"alpha"})json"},
    {"SoundSet", ".mcsound", "mce-sound", 1,
     "A sound set (M-AUDIO2): clips picked on each play, with a pitch and gain range, so footsteps and impacts vary. "
     "It is an Sfx asset: name it wherever a sound goes (AudioSource.clip, SfxTrigger.clip, PlaySfx, sequence and "
     "timeline sound keys). Picks follow the tick, so a replay picks the same.",
     R"json([
{"name":"clips","type":"array of guid refs","help":"The Sfx assets to pick from (\"guid:<uuid>\"), up to 4096; not other sound sets"},
{"name":"pick","type":"\"random\" | \"sequence\"","default":"random"},
{"name":"no_repeat","type":"boolean","default":true,"help":"random: never the same clip twice in a row"},
{"name":"pitch","type":"[min, max]","default":[1,1],"help":"Playback rate, 0.25-4 (1.06 is about a semitone up)"},
{"name":"gain_db","type":"[min, max]","default":[0,0],"help":"Added to the play's gain, -60 to 12 dB"}])json",
     R"json({"clips":[],"pick":"random","no_repeat":true,"pitch":[0.95,1.05],"gain_db":[-2,0]})json"},
    {"Sequence", ".mcseq", "mce-sequence", 1,
     "An action list a SequencePlayer component plays. Every step is an object with exactly one action key; a looping "
     "sequence needs a step that waits (wait > 0, wait_event or move_to).",
     R"json([
{"name":"name","type":"string"},
{"name":"loop","type":"boolean","default":false},
{"name":"steps","type":"array of step objects","help":"{\"wait\": s} | {\"wait_event\": id} | {\"move_to\": [x, y], \"speed\": m/s, \"relative\": bool} | {\"anim\": name} | {\"sound\": \"guid:<Sfx>\", \"gain_db\": db} | {\"stop_sound\": true} | {\"emit\": id} | {\"call\": hook} | {\"spawn\": \"guid:<Prefab>\", \"at\": [x, y], \"relative\": bool} | {\"destroy\": true}"}])json",
     R"json({"name":"sequence","loop":false,"steps":[{"wait":1}]})json"},
    {"Timeline", ".mctimeline", "mce-timeline", 1,
     "Keyed tracks a TimelinePlayer component plays against the fixed tick (cutscenes, scripted beats). A track's "
     "\"target\" is \"self\" (the player's entity, the default) or a binding index 0-7 (TimelinePlayer.bindings). "
     "Keys are in time order; a key's \"ease\" (linear, step, smooth, ease_in, ease_out) shapes the curve to the next key.",
     R"json([
{"name":"name","type":"string"},
{"name":"duration","type":"number","help":"Seconds; default: the last key's end"},
{"name":"tracks","type":"array of track objects","help":"{\"type\": \"transform\", \"target\", \"keys\": [{\"t\", \"position\": [x, y, z], \"rotation\": [x, y, z, w], \"scale\": [x, y, z], \"ease\"}]} | {\"type\": \"property\", \"target\", \"path\": \"Component.field\", \"keys\": [{\"t\", \"value\": number or [..], \"ease\"}]} | {\"type\": \"activation\", \"target\", \"path\": \"Component.bool_field\", \"ranges\": [{\"from\", \"to\"}]} | {\"type\": \"animation\", \"target\", \"keys\": [{\"t\", \"clip\", \"loop\", \"fade\"}]} | {\"type\": \"audio\", \"keys\": [{\"t\", \"sound\": \"guid:<Sfx>\", \"gain_db\"}]} | {\"type\": \"camera\", \"cuts\": [{\"t\", \"camera\": \"self\" | 0-7, \"blend\": seconds eased from the previous cut}]} | {\"type\": \"event\", \"keys\": [{\"t\", \"emit\": id}]} | {\"type\": \"subtitle\", \"lines\": [{\"t\", \"duration\", \"text\", \"speaker\"}]}"}])json",
     R"json({"name":"timeline","duration":1,"tracks":[{"type":"event","keys":[{"t":0,"emit":"start"}]}]})json"},
    {"BehaviorTree", ".mcbt", "mce-bt", 1,
     "A behavior tree an AiBrain component runs against the fixed tick, reading and writing its entity's Blackboard. "
     "Every node is an object with exactly one kind key (plus that kind's extras and an optional \"name\"). A running "
     "node resumes on the next tick; a finished root starts again on the tick after.",
     R"json([
{"name":"name","type":"string"},
{"name":"blackboard","type":"object","help":"key: \"bool\" | \"int\" | \"float\" | \"vec3\" | \"entity\", or key: {\"type\", \"default\"}; at most 16 bool/int/float, 8 vec3 and 8 entity keys"},
{"name":"root","type":"node object","help":"{\"sequence\" | \"selector\": [nodes]} | {\"parallel\": [nodes], \"policy\": \"all\" | \"one\"} | {\"invert\" | \"succeed\" | \"fail\": node} | {\"repeat\" | \"retry\": node, \"count\": n (0 = forever)} | {\"cooldown\" | \"timeout\": node, \"seconds\": s} | {\"guard\": node, \"key\", \"op\", \"value\", \"abort\": \"none\" | \"self\" | \"lower\" | \"both\"} | {\"check\": key, \"op\": \"is_set\" | \"not_set\" | \"==\" | \"!=\" | \"<\" | \"<=\" | \">\" | \">=\", \"value\"} | {\"wait\": s} | {\"set\": key, \"value\" | \"add\" | \"clear\": true | \"from\": key} | {\"emit\": signal} | {\"task\": name, \"key\", \"value\", \"vector\", \"arg\"} | {\"utility\": [score nodes], \"hysteresis\"} | {\"score\": node, \"base\", \"by\": [{\"key\", \"from\", \"to\", \"curve\": \"linear\" | \"power\" | \"logistic\" | \"step\", \"exponent\", \"invert\", \"weight\"}]} | {\"token\": node, \"pool\": name, \"key\": entity key}; stock tasks: {\"task\": \"move_to\" | \"look_at\", \"key\": vec3 or entity key, \"radius\"} | {\"task\": \"animate\", \"clip\", \"finish\": bool} | {\"task\": \"play_sound\", \"sound\": \"guid:...\", \"gain_db\"} | {\"task\": \"spawn\", \"prefab\": \"guid:...\", \"key\", \"offset\"} | {\"task\": \"raise\" | \"wait_event\", \"event\"} | {\"task\": \"log\", \"text\"} | {\"task\": \"make_noise\", \"loudness\"} | {\"task\": \"find_point\", \"store\": vec3 key, \"around\", \"from\", \"radius\", \"rings\", \"count\", \"prefer\": \"near\" | \"far\" | \"close\", \"need\": \"visible\" | \"hidden\"}; at most 128 nodes"}])json",
     R"json({"name":"brain","blackboard":{"alert":{"type":"bool","default":false}},"root":{"selector":[{"sequence":[{"check":"alert","value":true},{"emit":"alarm"},{"set":"alert","value":false}]},{"wait":1}]}})json"},
    {"Tilemap", ".mctilemap", "mce-tilemap", 1,
     "One tile layer over a tileset texture cut into columns x rows cells, drawn by a TilemapLayer component (map: "
     "\"guid:...\"). tiles: width x height indices, row 0 = bottom; 0 = empty, n = cell n-1 (cells count left to right, "
     "top row first). Paint it with tilemap_paint.",
     R"json([
{"name":"name","type":"string"},
{"name":"tileset","type":"guid ref","help":"A Texture asset (required)"},
{"name":"columns","type":"integer","help":"Tileset cells across (1-256)"},
{"name":"rows","type":"integer","help":"Tileset cells down (1-256)"},
{"name":"width","type":"integer","help":"Map tiles across (1-4096)"},
{"name":"height","type":"integer","help":"Map tiles up (1-4096)"},
{"name":"tile_size","type":"number","default":1,"help":"Metres per tile"},
{"name":"tiles","type":"array of integers","help":"width x height; omitted on asset_create: all empty"}])json",
     R"json({"name":"tilemap","tileset":null,"columns":1,"rows":1,"width":16,"height":9,"tile_size":1})json"},
    {"SpriteSheet", ".mcsheet", "mce-spritesheet", 1,
     "Regions of one texture and named flipbook clips over them. An AnimatedSprite component (sheet: \"guid:...\", "
     "clip: name) plays a clip on the Sprite next to it; Sprite.region shows one region by hand. Regions are numbered: "
     "the grid's cells first (left to right, top row first), then the named pixel rects.",
     R"json([
{"name":"name","type":"string"},
{"name":"texture","type":"guid ref | null","default":null,"help":"A Texture asset (\"guid:<uuid>\")"},
{"name":"grid","type":"object","help":"{\"columns\": 1-1024, \"rows\": 1-1024, \"count\": cells used (default columns x rows)}"},
{"name":"image_size","type":"[w, h]","help":"Texture size in pixels; required with regions"},
{"name":"regions","type":"array","help":"[{\"name\": \"hurt\", \"rect\": [x, y, w, h]}] in pixels, y down from the top"},
{"name":"clips","type":"array","help":"[{\"name\": 1-31 chars, \"frames\": [index | region name, ...] | {\"from\": a, \"to\": b}, \"fps\": 12, \"mode\": \"Loop\" | \"PingPong\" | \"Once\", \"events\": [{\"frame\": index within the clip, \"name\": 1-23 chars}]}]"}])json",
     R"json({"name":"sheet","texture":null,"grid":{"columns":1,"rows":1},"clips":[{"name":"idle","frames":[0],"fps":12,"mode":"Loop"}]})json"},
    {"Skeleton2D", ".mcskel2d", "mce-skeleton2d", 1,
     "A 2D cut-out skeleton: a bone tree and the slots drawn on its bones. A Skeleton2D component "
     "(skeleton: \"guid:...\") poses and draws it; Skeleton2D.pose[i] offsets bone i from its rest pose. Metres and "
     "degrees, Y up. Bones list parents before children; slots are in draw order (first drawn first, behind). At most "
     "64 bones and 256 slots. A slot shows one attachment at a time: its own quad, or one of its 'attachments' (quads, "
     "or meshes of textured triangles whose vertices sit in the slot bone's space or are weighted to up to 4 bones). "
     "'animations' are named clips of per-bone keys (offsets from the rest pose: rotate and translate add, scale "
     "multiplies), slot keys (color, attachment), mesh deform keys and timed events; a Skeleton2DAnimator component "
     "(clip: name, or graph: \"guid:<AnimGraph>\" whose states name the animations) plays them and keeps slot state "
     "in a Skeleton2DSlots component. 'ik' constraints (two-bone or CCD) turn a chain of bones so its tip reaches a "
     "target bone or an IKTarget2D entity, every tick after the pose; animations key their mix, and a Skeleton2DIK "
     "component holds the mix in use and a gameplay override.",
     R"json([
{"name":"name","type":"string"},
{"name":"bones","type":"array of bone objects","help":"{\"name\": 1-31 chars, unique, \"parent\": a bone listed earlier (absent: a root), \"x\": m, \"y\": m, \"rotation\": degrees, \"scale\": [sx, sy] or s, \"length\": m}; the first bone is a root"},
{"name":"slots","type":"array of slot objects","help":"{\"name\": 1-31 chars, unique, \"bone\": a bone name, \"texture\": \"guid:<Texture>\" | null (a solid quad), \"rect\": [u0, v0, u1, v1] (v0 = top), \"size\": [w, h] m, \"pivot\": [x, y] 0-1, \"offset\": [x, y] m in the bone's space, \"rotation\": degrees, \"tint\": [r, g, b, a], \"attachment\": the attachment shown first (default: the first listed; null hides), \"attachments\": up to 64 [{\"name\", \"type\": \"quad\" (texture, rect, size, pivot, offset, rotation as on a slot) | \"mesh\", \"texture\", \"vertices\": 3-1024 [{\"uv\": [u, v], \"x\", \"y\"} (m in the slot bone's space) or {\"uv\", \"weights\": 1-4 [{\"bone\", \"x\", \"y\", \"weight\"}] (normalized to sum 1)}], \"triangles\": [i0, i1, i2, ...] (1-2048 triangles)}]}; a slot with 'attachments' takes no quad fields itself"},
{"name":"animations","type":"array of animation objects","help":"{\"name\": 1-31 chars, unique, \"duration\": s (default: the last key or event), \"loop\": true, \"bones\": {bone name: {\"rotate\": [{\"t\", \"value\": degrees, \"curve\"}], \"translate\": [{\"t\", \"x\", \"y\", \"curve\"}] (m, default 0), \"scale\": [{\"t\", \"x\", \"y\", \"curve\"}] (factors, default 1)}}, \"slots\": {slot name: {\"color\": [{\"t\", \"color\": [r, g, b, a], \"curve\"}] (absolute linear RGBA; alpha 0 hides), \"attachment\": [{\"t\", \"name\": attachment | null}] (stepped)}}, \"deform\": {slot name: {mesh attachment name: [{\"t\", \"offsets\": [dx0, dy0, dx1, dy1, ...] (2 per vertex, default 0), \"curve\"}]}}, \"ik\": {IK constraint name: [{\"t\", \"mix\": 0-1 (default 1), \"curve\"}]}, \"events\": [{\"t\", \"name\": 1-23 chars}]}; keys in increasing t; \"curve\" shapes the interval to the next key: \"linear\" (default), \"stepped\" or a bezier [cx1, cy1, cx2, cy2] with cx in 0..1"},
{"name":"ik","type":"array of IK constraint objects","help":"Up to 16, applied in 'order' every tick after the pose: {\"name\": 1-31 chars, unique, \"bones\": the chain, parent first, each the child of the one before (1-32 bones), \"target\": a bone outside the chain and not under its first bone, or null (only an IKTarget2D entity drives it), \"type\": \"two_bone\" (default for 2 bones; exact, law of cosines) | \"ccd\" (default otherwise), \"bend\": \"positive\" (counter-clockwise elbow, default) | \"negative\" (two-bone), \"iterations\": 1-64 (CCD, default 10), \"mix\": 0-1 (default 1), \"order\": whole number (default 0)}"}])json",
     R"json({"name":"skeleton","bones":[{"name":"root"},{"name":"body","parent":"root","y":0.5,"length":0.5}],"slots":[{"name":"body","bone":"body","texture":null,"size":[0.4,0.6],"pivot":[0.5,0],"tint":[1,1,1,1]},{"name":"cape","bone":"body","tint":[0.8,0.2,0.2,1],"attachments":[{"name":"cape","type":"mesh","texture":null,"vertices":[{"uv":[0,0],"x":-0.2,"y":0.6},{"uv":[1,0],"x":0.2,"y":0.6},{"uv":[1,1],"weights":[{"bone":"body","x":0.25,"y":0,"weight":0.5},{"bone":"root","x":0.25,"y":0.5,"weight":0.5}]},{"uv":[0,1],"weights":[{"bone":"body","x":-0.25,"y":0,"weight":0.5},{"bone":"root","x":-0.25,"y":0.5,"weight":0.5}]}],"triangles":[0,1,2,0,2,3]}]}],"animations":[{"name":"idle","bones":{"body":{"rotate":[{"t":0,"value":0},{"t":0.5,"value":5},{"t":1,"value":0}]}},"deform":{"cape":{"cape":[{"t":0},{"t":0.5,"offsets":[0,0,0,0,0.05,0,0.05,0]},{"t":1}]}}}]})json"},
    {"AnimGraph", ".mcanim", "mce-animgraph", 2,
     "A 3D animation state graph: states play a mesh clip, a 1D blend of clips or a 2D blend space, transitions fire "
     "on parameter conditions or when a one-shot state finishes; layers play clips over a bone mask on top (override "
     "or additive), root_motion moves the entity by a bone's ground motion, and IK chains put a hand on an "
     "IKTarget3D, turn a head towards one, or plant feet on the ground. An Animator component (graph: \"guid:...\") runs "
     "it on its MeshRef; Animator.params holds the parameter values in the order 'params' lists them. A graph without "
     "the version 2 fields (blend2d, root_motion, layers, ik) is saved as version 1.",
     R"json([
{"name":"name","type":"string"},
{"name":"params","type":"array of strings","help":"Up to 4 parameter names (1-23 chars)"},
{"name":"start","type":"string","help":"The first state (default: the first listed)"},
{"name":"states","type":"array","help":"[{\"name\": 1-31 chars, \"clip\": mesh clip name | \"blend\": {\"param\": name, \"points\": [{\"clip\": name, \"at\": value}, ...]} | \"blend2d\": {\"x\": param, \"y\": param, \"points\": 3-8 [{\"clip\": name, \"at\": [x, y]}]}, \"loop\": true, \"speed\": 1, \"events\": [{\"time\": seconds, \"name\": 1-23 chars}]}]"},
{"name":"transitions","type":"array","help":"[{\"from\": state | \"*\", \"to\": state, \"when\": [{\"param\": name, \"op\": \">\" | \">=\" | \"<\" | \"<=\" | \"==\" | \"!=\", \"value\": n}], \"on_finish\": false, \"fade\": 0.2}]"},
{"name":"root_motion","type":"string","help":"(version 2) A bone (usually the hips) whose ground motion and turn move the entity; Animator.root_motion picks move, report or off"},
{"name":"layers","type":"array","help":"(version 2) Up to 4 [{\"name\", \"clip\", \"mask\": [bone, ...] (each with its subtree; empty = the whole body), \"weight\": 0-1, \"param\": a parameter scaling the weight, \"additive\": false, \"loop\": true, \"speed\": 1}]"},
{"name":"ik","type":"array","help":"(version 2) Up to 4 [{\"name\", \"type\": \"two_bone\" | \"look_at\" | \"foot\", \"bone\": the end bone (a hand, the head, a foot), \"pelvis\": the bone a foot chain lowers, \"weight\": 0-1, \"param\", \"pole\": [x, y, z] (two_bone, foot), \"axis\": [x, y, z] and \"limit\": degrees (look_at), \"reach\": metres a foot ray looks down}]; goals come from IKTarget3D entities naming the chain"}])json",
     R"json({"name":"graph","states":[{"name":"idle","clip":"Idle"}]})json"},
    {"Material", ".mcmat", "mce-material", 1, "A metallic-roughness material; texture references are Texture assets.",
     R"json([
{"name":"name","type":"string"},
{"name":"base_color","type":"[r, g, b, a]","default":[1,1,1,1]},
{"name":"metallic","type":"number","default":0},
{"name":"roughness","type":"number","default":1},
{"name":"emissive","type":"[r, g, b]","default":[0,0,0]},
{"name":"uv_scale","type":"[u, v]","default":[1,1],"help":"how often the textures repeat across the mesh's UVs ([8, 8] tiles a floor 8 times each way)"},
{"name":"alpha_mode","type":"\"opaque\" | \"mask\" | \"blend\"","default":"opaque"},
{"name":"alpha_cutoff","type":"number","default":0.5},
{"name":"double_sided","type":"boolean","default":false},
{"name":"unlit","type":"boolean","default":false},
{"name":"clearcoat","type":"number","default":0,"help":"0..1: a clear varnish layer over the base (lacquer, car paint)"},
{"name":"clearcoat_roughness","type":"number","default":0,"help":"0..1: the varnish's own roughness"},
{"name":"sheen_color","type":"[r, g, b]","default":[0,0,0],"help":"cloth and velvet: the grazing-angle sheen's color (0 = none)"},
{"name":"sheen_roughness","type":"number","default":0,"help":"0..1"},
{"name":"anisotropy","type":"number","default":0,"help":"-1..1: highlights stretched along the UV tangent (brushed metal)"},
{"name":"anisotropy_rotation","type":"number","default":0,"help":"radians: turns the stretch direction"},
{"name":"transmission","type":"number","default":0,"help":"0..1: see-through like glass; the opaque scene behind shows through, tinted by base_color"},
{"name":"ior","type":"number","default":0,"help":"1..3 index of refraction (0 = 1.5): a dielectric's reflectance and how much transmission bends"},
{"name":"textures","type":"object","help":"{\"base_color\", \"normal\", \"metallic_roughness\", \"emissive\"}: \"guid:<Texture>\" each, all optional"}])json",
     R"json({"name":"material","base_color":[1,1,1,1],"metallic":0,"roughness":1,"emissive":[0,0,0],"alpha_mode":"opaque","alpha_cutoff":0.5,"double_sided":false,"unlit":false,"textures":{}})json"},
    {"InputMap", ".mcinput", "mce-input", 1,
     "The project's input action maps; the first map is the one the stack starts with. run_headless input names these actions.",
     R"json([
{"name":"maps","type":"array","help":"[{\"name\": \"Gameplay\", \"blocks_below\": false, \"player\": 1 (2..4: a local player's map, always active), \"actions\": [{\"name\": \"Move\", \"type\": \"button\" | \"axis1d\" | \"axis2d\", \"bindings\": [...]}]}]"},
{"name":"maps[].actions[].bindings[]","type":"object","help":"Exactly one device key: {\"key\": \"Space\"} | {\"gamepad_button\": \"South\"} | {\"gamepad_axis\": \"LeftX\"} | {\"mouse_button\": \"Left\"} | {\"mouse_delta\": \"x\"} | {\"mouse_wheel\": \"y\"} | {\"touch_zone\": [x0, y0, x1, y1]} | {\"touch_tap\": [x0, y0, x1, y1]} | {\"touch_stick\": \"x\", \"zone\": [...]} | {\"touch_swipe\": \"x\"}; options: axis (x|y), scale, deadzone [lo, hi], curve, invert. Zones are 0..1 over the window, y down"}])json",
     R"json({"maps":[{"name":"Gameplay","actions":[{"name":"Jump","type":"button","bindings":[{"key":"Space"},{"gamepad_button":"South"}]}]}]})json"},
    {"StoreCatalog", ".mcstore", "mce-store", 1, "The project's in-app products and ad placements (M-MONETIZE).",
     R"json([
{"name":"products","type":"array","help":"[{\"id\", \"type\": \"consumable\" | \"non_consumable\", \"ios\", \"android\", \"removes_ads\": bool}]"},
{"name":"ads","type":"array","help":"[{\"id\", \"format\": \"rewarded\" | \"interstitial\" | \"banner\", \"ios\", \"android\"}]"},
{"name":"admob","type":"object","help":"{\"ios\": \"ca-app-pub-...~...\", \"android\": \"ca-app-pub-...~...\"} (app ids, not unit ids)"}])json",
     R"json({"products":[],"ads":[]})json"},
    {"GameServicesCatalog", ".mcservices", "mce-services", 1,
     "The project's achievements, leaderboards and cloud saves (M-LIVEOPS).",
     R"json([
{"name":"achievements","type":"array","help":"[{\"id\", \"steps\": 1-1000000, \"ios\", \"android\", \"steam\" (Steamworks API name; default: id)}]"},
{"name":"leaderboards","type":"array","help":"[{\"id\", \"order\": \"higher_is_better\" | \"lower_is_better\", \"ios\", \"android\", \"steam\" (default: id)}]"},
{"name":"cloud_saves","type":"array of strings","help":"save::write names synced to the cloud"}])json",
     R"json({"achievements":[],"leaderboards":[],"cloud_saves":[]})json"},
    {"UiDocument", ".mcui", "mce-ui", 1,
     "A UI document: a widget tree with anchors, per-state styles (UiStyle), stack and grid layouts, the skin's font "
     "and sprite sheet by reference (M-UI-DATA). A widget starts from the look the C++ ui::Builder gives its kind; "
     "\"style\" and \"styles\" change only the UiStyle fields they list. Names are unique among siblings; \"nav\" "
     "edges and lookups take paths of names from the root (\"menu/play\").",
     R"json([
{"name":"font","type":"guid ref | null","default":null,"help":"A Font asset; null: the built-in 13 px font"},
{"name":"sprites","type":"guid ref | null","default":null,"help":"A SpriteSheet whose region white_region is white (flat quads); null: built-in"},
{"name":"white_region","type":"integer","default":0},
{"name":"theme","type":"guid ref | null","default":null,"help":"A UiTheme (.mcuitheme): its palette, fonts and looks replace the kind defaults; a UiScreen's theme wins"},
{"name":"modal","type":"boolean","default":true,"help":"false: a HUD, never takes focus"},
{"name":"widgets","type":"array","help":"[{\"name\": 1-31 chars, no '/', \"kind\": Panel | Image | Text | Button | Toggle | Slider | Stack | Grid | Console | Scroll | Tabs, \"children\": [...]}]"},
{"name":"widgets[] anchors","type":"one of","help":"\"fill\": true | \"at\": [anchor x, anchor y, width, height] (0 = preferred) | \"anchors\": UiAnchors {min, max, offset_min, offset_max, pivot, to: Parent | SafeArea | Screen}"},
{"name":"widgets[] look","type":"object","help":"\"style\": UiStyle fields over every state {tint, text_color, accent, sprite_region, text_size, draw_quad, slice}; \"styles\": {\"Hover\": {...}} per state (Normal, Hover, Pressed, Focused, Disabled); \"font_role\": Auto | Body | Title | Button (the theme's font); \"text_align\": Left | Center | Right (a Text widget's lines across its rect); \"own_style\": [\"tint\" | \"text_color\" | \"accent\" | \"sprite\" | \"text_size\" | \"letter_spacing\" | \"text_outline\" | \"text_shadow\"] (fields a theme never restyles: the kind's default look stays, with or without a \"style\" for them)"},
{"name":"widgets[] values","type":"object","help":"\"text\", \"value\", \"range\": [lo, hi, step], \"visible\", \"interactable\", \"blocks_pointer\", \"default_focus\": true, \"convar\": name (two-way), \"nav\": {\"up\" | \"down\" | \"left\" | \"right\": path}, \"stack\": UiStackLayout {axis, spacing, padding, align}, \"grid\": UiGridLayout {columns, cell, spacing}"},
{"name":"widgets[] special","type":"object","help":"A Panel with \"modal\": true is a hidden, dimmed veil for Document::open_modal; a Tabs widget lists \"tabs\": [{\"name\", \"label\", \"children\"}] and its \"value\" is the page shown; \"kind_defaults\": false starts from the C++ defaults"}])json",
     R"json({"widgets":[{"name":"menu","kind":"Stack","at":[0.5,0.5,320,0],"children":[{"name":"play","kind":"Button","text":"PLAY","default_focus":true}]}]})json"},
    {"UiTheme", ".mcuitheme", "mce-uitheme", 1,
     "A UI theme (M-UI-DATA): a palette, a font per role and per-kind looks that restyle every .mcui document "
     "naming it (\"theme\": \"guid:...\") or shown by a UiScreen whose theme is set. A widget style field the "
     "document leaves at its kind's default look takes the theme's.",
     R"json([
{"name":"palette","type":"object","help":"UiPalette colors (sRGB): panel, button, button_hover, button_pressed, button_focused, button_disabled, label, text, text_focused, text_disabled, accent, console, console_text"},
{"name":"fonts","type":"object","help":"{\"body\" | \"title\" | \"button\": \"guid:<Font>\"}; a widget's font_role picks one (Auto: button kinds use button, others body)"},
{"name":"sprites","type":"guid ref | null","default":null,"help":"A SpriteSheet skin (white_region: its white region)"},
{"name":"white_region","type":"integer","default":0},
{"name":"styles","type":"object","help":"{\"<UiWidgetKind>\": {\"style\": UiStyle fields over every state, \"styles\": {\"Hover\": {...}}}} over the palette's look"}])json",
     R"json({"palette":{"panel":[0.1,0.12,0.2,0.92],"button":[0.2,0.35,0.6,1],"accent":[1,0.6,0.2,1]}})json"},
    {"Terrain", ".mcterrain", "mce-terrain", 1,
     "A heightmap terrain (M-TERRAIN): resolution x resolution height samples over size metres, centred on the entity "
     "whose Terrain3D component (terrain: \"guid:...\") draws it, with LOD, shadows and an optional .mcmat. A new "
     "terrain is flat at 'fill' (default: the low end of height_range). Up to 8 'layers' (each a .mcmat) paint it "
     "by a weight map ('splat') or by height and slope rules ('auto').",
     R"json([
{"name":"name","type":"string"},
{"name":"resolution","type":"integer","default":129,"help":"Samples per side: 2^n + 1 (33, 65, 129, 257, 513, 1025, 2049, 4097)"},
{"name":"chunk","type":"integer","default":32,"help":"Quads per side of a finest LOD node: 8, 16, 32, 64, 128 or 256"},
{"name":"size","type":"[x, z]","default":[128,128],"help":"Metres along x and z"},
{"name":"height_range","type":"[low, high]","default":[0,32],"help":"Metres a sample of 0 and of 65535 stand for"},
{"name":"heights","type":"string","help":"base64 of resolution^2 little-endian u16 samples, z rows of x columns; omitted = flat"},
{"name":"fill","type":"number","help":"Metres of a flat terrain (without 'heights'); default: the low end of height_range"},
{"name":"layers","type":"array","help":"Up to 8 splat layers: {material: \"guid:...\" (.mcmat), tile: metres per repeat (4), triplanar: false, height_strength: 1, auto: {height: [lo, hi], slope: [deg, deg]}}"},
{"name":"splat_resolution","type":"integer","help":"Weight samples per side: 2^n + 1, 33 up to resolution; default min(resolution, 1025)"},
{"name":"splat","type":"string","help":"base64 of splat_resolution^2 x layer count u8 weights, sample by sample; omitted = the layers' auto rules (else all layer 0)"},
{"name":"holes","type":"string","help":"base64 of a bit per height sample (sample z*resolution+x at bit i%8 of byte i/8): set bits cut the drawn surface and the collision open"}])json",
     R"json({"name":"terrain","resolution":129,"chunk":32,"size":[128,128],"height_range":[0,32]})json"},
    {"RemoteConfig", ".mcconfig", "mce-config", 1,
     "The project's remote config defaults (M-LIVEOPS). A value's type is its JSON's unless {\"type\": "
     "\"int|float|bool|string\", \"value\": ...} names it; keys are 1-63 letters, digits, '_', '-', '.'.",
     R"json([
{"name":"values","type":"object","help":"key -> true/false, a number, text, or {\"type\", \"value\"}"}])json",
     R"json({"values":{}})json"},
};

const AssetKind* kind_for_extension(StringView path) {
    const usize dot = path.rfind('.');
    if (dot == StringView::npos) return nullptr;
    const StringView ext = path.substr(dot);
    for (const AssetKind& k : kKinds)
        if (ext == k.ext) return &k;
    return nullptr;
}

const AssetKind* kind_named(StringView name) {
    for (const AssetKind& k : kKinds)
        if (name == k.type || name == k.ext || (name.size() + 1 == std::strlen(k.ext) && name == StringView(k.ext + 1)))
            return &k;
    return nullptr;
}

void append_kind_list(cook::String& s) {
    for (const AssetKind& k : kKinds) {
        if (&k != kKinds) s += ", ";
        s += k.ext;
    }
}

// ---------------------------------------------------------------- paths and lookup

// A file path under assets/ as the tools accept it: forward slashes, no "." / ".." segments.
bool valid_rel(StringView rel) {
    if (rel.empty() || rel.size() >= 400 || rel[0] == '/' || rel.find('\\') != StringView::npos || rel.find(':') != StringView::npos)
        return false;
    for (usize start = 0; start <= rel.size();) {
        usize end = rel.find('/', start);
        if (end == StringView::npos) end = rel.size();
        const StringView seg = rel.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        start = end + 1;
    }
    return true;
}

bool need_project(ToolCall& c) {
    if (c.server.session().project().is_open()) return true;
    return fail(c, "no project is open: start mce_mcp with --project <dir>");
}

// The registered asset named by 'path' or 'guid'.
bool arg_asset(ToolCall& c, const editor::ProjectAsset*& out) {
    StringView path, guid_text;
    if (!arg_string(c, "path", path, false) || !arg_string(c, "guid", guid_text, false)) return false;
    if (path.empty() == guid_text.empty()) return fail(c, "give one of 'path' (under assets/) or 'guid' (\"guid:<uuid>\")");
    const editor::EditorProject& project = c.server.session().project();
    out = nullptr;
    if (!path.empty()) {
        for (const editor::ProjectAsset& a : project.assets())
            if (a.path.view() == path) out = &a;
        if (!out) return fail(c, "no registered asset at that path (asset_list lists them; asset_create makes a new one)");
        return true;
    }
    if (guid_text.size() > 5 && guid_text.substr(0, 5) == "guid:") guid_text = guid_text.substr(5);
    const Result<asset::Guid> g = asset::parse_guid(guid_text);
    if (!g) return fail(c, "argument 'guid' must be \"guid:<uuid>\"");
    out = project.find(g.value());
    if (!out) return fail(c, "no registered asset with that guid (asset_list lists them)");
    return true;
}

cook::String guid_ref(asset::Guid g) {
    cook::String s("guid:");
    s += asset::to_string(g).view();
    return s;
}

// ---------------------------------------------------------------- validate and write

// Cooks `text` in memory as the asset at `rel` would cook, then writes it atomically and rescans
// the catalog. Nothing is written when the cook refuses it (c.error lists the diagnostics).
bool validate_and_write(ToolCall& c, StringView rel, asset::Guid guid, StringView text) {
    editor::EditorProject& project = c.server.session().project();
    const usize dot = rel.rfind('.');
    const cook::IImporter* importer = project.importers().for_extension(rel.substr(dot));
    if (!importer) return fail(c, "no importer reads that file type");
    cook::AssetRegistry registry;
    cook::DiagList scan;
    registry.scan(project.root(), project.importers(), false, scan);
    cook::DiagList diags;
    Array<byte> cooked;
    if (!cook::cook_document(registry, *importer, guid, rel, text, cooked, diags)) {
        c.error += "the cook refuses this document (nothing was written):";
        usize n = 0;
        for (const cook::Diagnostic& d : diags.all()) {
            if (d.severity != cook::Severity::Error || n++ == kMaxDiagnostics) continue;
            c.error += '\n';
            cook::format_diagnostic(d, c.error);
        }
        return false;
    }
    const cook::String abs = project.asset_path(rel);
    const usize slash = abs.view().rfind('/');
    if (slash != StringView::npos) {
        if (Result<void> r = cook::fs::create_directories(abs.view().substr(0, slash)); !r) return fail(c, "could not create the folder");
    }
    if (!cook::fs::write_file_atomic(abs.view(), Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size())))
        return fail(c, "could not write the file");
    project.rescan();
    return true;
}

void write_result(ToolCall& c, StringView rel, asset::Guid guid, const AssetKind& k, usize bytes) {
    c.out.begin_object();
    c.out.key("path").str(rel).key("guid").str(guid_ref(guid).view()).key("type").str(k.type).key("bytes").num_u(bytes);
    c.out.end_object();
}

// A document from its members: format, version and guid first, then `members` in order.
struct Member {
    StringView key;
    cook::JsonValue value;
};

void build_document(const AssetKind& k, asset::Guid guid, Span<const Member> members, cook::String& out) {
    cook::String compact;
    JsonWriter w(compact);
    w.begin_object();
    w.key("format").str(k.format).key("version").num_u(k.version).key("guid").str(asset::to_string(guid).view());
    for (const Member& m : members) w.key(m.key).copy(m.value);
    w.end_object();
    // Re-parsed to be written indented, keys in this order (the form the importers' examples use).
    cook::JsonDoc doc;
    cook::JsonError err;
    doc.parse(compact.view(), err);
    cook::write_json(doc.root(), out, false);
    out += '\n';
}

bool reserved_key(StringView key) { return key == "format" || key == "version" || key == "guid"; }

bool check_content(ToolCall& c, cook::JsonValue content) {
    if (!content.is_object()) {
        c.error += "argument 'content' must be an object of the asset's fields (asset_schema describes them)";
        return false;
    }
    for (u32 i = 0; i < content.size(); ++i) {
        if (!reserved_key(content.key_at(i))) continue;
        c.error += "content.";
        c.error += content.key_at(i);
        c.error += ": format, version and guid are set by the tool and kept from the file: leave them out of 'content'";
        return false;
    }
    return true;
}

// Members in order: `base` (minus format/version/guid) with `over` replacing same-named ones,
// then the members of `over` that `base` lacks.
void merge_members(cook::JsonValue base, cook::JsonValue over, Array<Member>& out) {
    for (u32 i = 0; base.valid() && i < base.size(); ++i) {
        const StringView key = base.key_at(i);
        if (reserved_key(key)) continue;
        const cook::JsonValue o = over.valid() ? over.get(key) : cook::JsonValue();
        out.push_back(Member{key, o.valid() ? o : base.at(i)});
    }
    for (u32 i = 0; over.valid() && i < over.size(); ++i)
        if (!base.valid() || !base.get(over.key_at(i)).valid()) out.push_back(Member{over.key_at(i), over.at(i)});
}

// A tilemap without "tiles" gets width x height empty ones (when both are sane; the cook reports
// anything else).
void add_empty_tiles(Array<Member>& members, cook::JsonDoc& holder) {
    u64 w = 0, h = 0;
    for (const Member& m : members) {
        if (m.key == "tiles") return;
        if (m.key == "width" && m.value.is_integer() && m.value.as_number() >= 1) w = static_cast<u64>(m.value.as_number());
        if (m.key == "height" && m.value.is_integer() && m.value.as_number() >= 1) h = static_cast<u64>(m.value.as_number());
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return;
    cook::String text("[");
    for (u64 i = 0; i < w * h; ++i) text += i == 0 ? "0" : ",0";
    text += ']';
    cook::JsonError err;
    holder.parse(text.view(), err);
    members.push_back(Member{"tiles", holder.root()});
}

// ---------------------------------------------------------------- tools

bool asset_schema(ToolCall& c) {
    StringView name;
    if (!arg_string(c, "type", name, false)) return false;
    if (name.empty()) {
        c.out.begin_object().key("types").begin_array();
        for (const AssetKind& k : kKinds)
            c.out.begin_object().key("type").str(k.type).key("extension").str(k.ext).key("summary").str(k.summary).end_object();
        c.out.end_array().end_object();
        return true;
    }
    const AssetKind* k = kind_named(name);
    if (!k) {
        c.error += "unknown asset type '";
        c.error += name;
        c.error += "' (asset_schema without 'type' lists them)";
        return false;
    }
    c.out.begin_object();
    c.out.key("type").str(k->type).key("extension").str(k->ext).key("format").str(k->format).key("version").num_u(k->version);
    c.out.key("summary").str(k->summary);
    c.out.key("fields").raw(k->fields);
    c.out.key("template").raw(k->tmpl);
    c.out.end_object();
    return true;
}

bool asset_create(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (!valid_rel(rel)) return fail(c, "argument 'path' must be a file path under assets/ like \"effects/dust.mcpart\" (forward slashes, no '..')");
    const AssetKind* k = kind_for_extension(rel);
    if (!k) {
        c.error += "asset_create makes ";
        append_kind_list(c.error);
        c.error += " files (scenes and prefabs: scene_new / scene_save)";
        return false;
    }
    const cook::JsonValue content = arg(c, "content");
    if (content.valid() && !check_content(c, content)) return false;
    editor::EditorProject& project = c.server.session().project();
    if (cook::fs::exists(project.asset_path(rel).view())) return fail(c, "a file already exists there: asset_write changes it");

    cook::JsonDoc tmpl, tiles;
    cook::JsonError err;
    tmpl.parse(k->tmpl, err);
    Array<Member> members;
    merge_members(tmpl.root(), content, members);
    if (std::strcmp(k->ext, ".mctilemap") == 0) add_empty_tiles(members, tiles);
    const asset::Guid guid = cook::generate_guid();
    cook::String text;
    build_document(*k, guid, members.span(), text);
    if (!validate_and_write(c, rel, guid, text.view())) return false;
    write_result(c, rel, guid, *k, text.size());
    return true;
}

bool asset_write(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    StringView mode = "merge";
    if (!need_project(c) || !arg_asset(c, a) || !arg_string(c, "mode", mode, false)) return false;
    if (mode != "merge" && mode != "replace") return fail(c, "argument 'mode' must be \"merge\" or \"replace\"");
    const AssetKind* k = kind_for_extension(a->path.view());
    if (!k) {
        c.error += "asset_write changes ";
        append_kind_list(c.error);
        c.error += " files (scenes and prefabs: the scene tools; textures: asset_set_import_settings)";
        return false;
    }
    const cook::JsonValue content = arg(c, "content");
    if (!check_content(c, content)) return false;
    const cook::String rel = a->path;
    const asset::Guid guid = a->guid;
    cook::String old_text;
    cook::JsonDoc old_doc;
    cook::JsonError err;
    if (mode == "merge" &&
        (!cook::fs::read_text(c.server.session().project().asset_path(rel.view()).view(), old_text) ||
         !old_doc.parse(old_text.view(), err) || !old_doc.root().is_object()))
        return fail(c, "the file on disk is not a JSON object: use mode \"replace\"");
    Array<Member> members;
    merge_members(mode == "merge" ? old_doc.root() : cook::JsonValue(), content, members);
    cook::String text;
    build_document(*k, guid, members.span(), text);
    if (!validate_and_write(c, rel.view(), guid, text.view())) return false;
    write_result(c, rel.view(), guid, *k, text.size());
    return true;
}

bool asset_read(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    if (!need_project(c) || !arg_asset(c, a)) return false;
    const cook::String abs = c.server.session().project().asset_path(a->path.view());
    cook::String text;
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!cook::fs::read_text(abs.view(), text)) return fail(c, "the file could not be read");
    if (!doc.parse(text.view(), err) || !doc.root().is_object())
        return fail(c, "not a JSON source asset (textures, sounds, fonts and meshes: asset_set_import_settings changes their import settings)");
    c.out.begin_object();
    c.out.key("path").str(a->path.view()).key("guid").str(guid_ref(a->guid).view()).key("type").str(editor::asset_type_name(a->type));
    c.out.key("content").copy(doc.root());
    c.out.end_object();
    return true;
}

// tilemap_paint's operations: [x, y, tile] cells, [x0, y0, x1, y1, tile] rects, [x, y, tile] flood fills.
bool read_op(ToolCall& c, const char* name, cook::JsonValue v, u32 n, i64* out) {
    if (v.is_array() && v.size() == n) {
        bool ok = true;
        for (u32 i = 0; i < n && ok; ++i) {
            ok = v.at(i).is_integer() && v.at(i).as_number() >= -1e9 && v.at(i).as_number() <= 1e9;
            if (ok) out[i] = static_cast<i64>(v.at(i).as_number());
        }
        if (ok && out[n - 1] >= 0) return true;
    }
    c.error += "argument '";
    c.error += name;
    c.error += n == 3 ? "' must be an array of [x, y, tile]" : "' must be an array of [x0, y0, x1, y1, tile]";
    c.error += " (whole numbers, tile >= 0)";
    return false;
}

// Replaces the 4-connected region of (x, y)'s tile with `tile`; tiles changed.
u32 flood_fill(editor::TilemapDocument& d, i32 x, i32 y, u32 tile) {
    if (!d.contains(x, y)) return 0;
    const u32 from = d.at(x, y);
    if (from == tile) return 0;
    u32 changed = 0;
    Array<u32> stack;
    stack.push_back(u32(y) * d.width + u32(x));
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        if (d.tiles[i] != from) continue;
        d.tiles[i] = tile;
        ++changed;
        const u32 tx = i % d.width, ty = i / d.width;
        if (tx > 0) stack.push_back(i - 1);
        if (tx + 1 < d.width) stack.push_back(i + 1);
        if (ty > 0) stack.push_back(i - d.width);
        if (ty + 1 < d.height) stack.push_back(i + d.width);
    }
    return changed;
}

char preview_char(u32 tile) {
    if (tile == 0) return '.';
    if (tile <= 9) return static_cast<char>('0' + tile);
    if (tile <= 35) return static_cast<char>('a' + tile - 10);
    return '#';
}

bool tilemap_paint(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    if (!need_project(c) || !arg_asset(c, a)) return false;
    if (a->type != asset::ChunkType::Tilemap) return fail(c, "that asset is not a tilemap (asset_create makes one: a .mctilemap path)");
    const cook::String rel = a->path;
    const cook::String abs = c.server.session().project().asset_path(rel.view());
    cook::String text;
    cook::DiagList diags;
    editor::TilemapDocument d;
    if (!cook::fs::read_text(abs.view(), text) || !d.parse(text.view(), rel.view(), diags))
        return fail(c, "the tilemap file cannot be read (asset_read shows it; cook_run names the error)");

    const cook::JsonValue clear = arg(c, "clear"), cells = arg(c, "cells"), rects = arg(c, "rects"), fills = arg(c, "fills");
    bool preview = false;
    if (const cook::JsonValue p = arg(c, "preview"); p.valid()) {
        if (!p.is_bool()) return fail(c, "argument 'preview' must be true or false");
        preview = p.as_bool();
    }
    for (const auto& [v, name] : {std::pair{cells, "cells"}, std::pair{rects, "rects"}, std::pair{fills, "fills"}}) {
        if (v.valid() && (!v.is_array() || v.size() > kMaxPaintOps)) {
            c.error += "argument '";
            c.error += name;
            c.error += "' must be an array (at most 65536 entries)";
            return false;
        }
    }
    // Every operation is checked before any is applied: a bad one changes nothing.
    const u32 cells_n = cells.valid() ? cells.size() : 0, rects_n = rects.valid() ? rects.size() : 0, fills_n = fills.valid() ? fills.size() : 0;
    const auto in_range = [&](i64 tile) {
        if (tile <= static_cast<i64>(d.cells())) return true;
        cook::appendf(c.error, "tile %lld is outside the tileset (0 = empty, 1-%u)", static_cast<long long>(tile), d.cells());
        return false;
    };
    i64 op[5];
    if (clear.valid() && (!clear.is_integer() || clear.as_number() < 0)) return fail(c, "argument 'clear' must be a tile (a whole number >= 0)");
    if (clear.valid() && !in_range(static_cast<i64>(clear.as_number()))) return false;
    for (u32 i = 0; i < cells_n; ++i)
        if (!read_op(c, "cells", cells.at(i), 3, op) || !in_range(op[2])) return false;
    for (u32 i = 0; i < rects_n; ++i)
        if (!read_op(c, "rects", rects.at(i), 5, op) || !in_range(op[4])) return false;
    for (u32 i = 0; i < fills_n; ++i)
        if (!read_op(c, "fills", fills.at(i), 3, op) || !in_range(op[2])) return false;

    // clear, then cells, then rects, then fills.
    u64 changed = 0, outside = 0;
    if (clear.valid()) changed += d.fill_rect(0, 0, i32(d.width) - 1, i32(d.height) - 1, static_cast<u32>(clear.as_number()));
    for (u32 i = 0; i < cells_n; ++i) {
        read_op(c, "cells", cells.at(i), 3, op);
        if (!d.contains(i32(op[0]), i32(op[1]))) ++outside;
        else if (d.set(i32(op[0]), i32(op[1]), u32(op[2]))) ++changed;
    }
    for (u32 i = 0; i < rects_n; ++i) {
        read_op(c, "rects", rects.at(i), 5, op);
        changed += d.fill_rect(i32(op[0]), i32(op[1]), i32(op[2]), i32(op[3]), u32(op[4]));
    }
    for (u32 i = 0; i < fills_n; ++i) {
        read_op(c, "fills", fills.at(i), 3, op);
        if (!d.contains(i32(op[0]), i32(op[1]))) ++outside;
        else changed += flood_fill(d, i32(op[0]), i32(op[1]), u32(op[2]));
    }
    if (changed > 0) {
        cook::String out;
        d.serialize(out);
        if (!validate_and_write(c, rel.view(), d.guid, out.view())) return false;
    }
    c.out.begin_object();
    c.out.key("path").str(rel.view()).key("guid").str(guid_ref(d.guid).view());
    c.out.key("width").num_u(d.width).key("height").num_u(d.height).key("tiles").num_u(d.cells());
    c.out.key("changed").num_u(changed).key("outside").num_u(outside);
    if (preview) {
        // Top row first, as it is seen: '.' empty, 1-9 then a-z for tiles 1-35, '#' above.
        c.out.key("preview").begin_array();
        cook::String row;
        for (u32 y = d.height; y-- > 0;) {
            row.clear();
            for (u32 x = 0; x < d.width; ++x) row += preview_char(d.at(i32(x), i32(y)));
            c.out.str(row.view());
        }
        c.out.end_array();
    }
    c.out.end_object();
    return true;
}

const ToolDef kAssetTools[] = {
    {"asset_schema",
     "Describe a JSON source asset type: its fields (type, default, help) and the template asset_create starts from. "
     "Without 'type': the types asset_create/asset_write handle (.mcpart, .mcsound, .mcseq, .mctilemap, .mcsheet, .mcskel2d, .mcanim, .mcmat, "
     ".mcinput, .mcstore, .mcservices, .mcui, .mcconfig).",
     R"json({"type":"object","properties":{"type":{"type":"string","description":"ParticlePreset, SoundSet, Sequence, Timeline, BehaviorTree, Tilemap, SpriteSheet, Skeleton2D, AnimGraph, Material, InputMap, StoreCatalog, GameServicesCatalog, UiDocument, UiTheme, RemoteConfig, Terrain, or the extension"}}})json",
     asset_schema},
    {"asset_create",
     "Create a JSON source asset at 'path' (its extension picks the type) with a fresh GUID: the type's template with "
     "'content' fields over it. The document is cooked in memory first; a document the cook refuses is not written and "
     "the error names the field. A .mctilemap needs content.tileset (\"guid:<Texture>\"), columns and rows (the tileset's "
     "cells), width and height; its tiles start empty. Returns the path and \"guid:<uuid>\" to reference it.",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"New file under assets/, e.g. effects/dust.mcpart"},"content":{"type":"object","description":"Fields to set (asset_schema lists them)"}},"required":["path"]})json",
     asset_create},
    {"asset_write",
     "Change a JSON source asset (by path or guid), keeping its GUID. mode \"merge\" (default): 'content' fields replace "
     "the file's same-named fields, others stay; \"replace\": the document becomes exactly 'content'. Cooked in memory "
     "first: a refused document leaves the file unchanged.",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"Path under assets/"},"guid":{"type":"string","description":"\"guid:<uuid>\" instead of path"},"content":{"type":"object"},"mode":{"type":"string","enum":["merge","replace"],"default":"merge"}},"required":["content"]})json",
     asset_write},
    {"asset_read", "Read a JSON source asset (by path or guid): its path, guid, type and parsed content (scenes and prefabs too).",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"Path under assets/"},"guid":{"type":"string","description":"\"guid:<uuid>\" instead of path"}}})json",
     asset_read},
    {"tilemap_paint",
     "Paint a .mctilemap (by path or guid) in one call: 'clear' sets every tile first, then 'cells' [[x, y, tile]], "
     "'rects' [[x0, y0, x1, y1, tile]] (inclusive, clipped) and 'fills' [[x, y, tile]] (flood fill of the region "
     "holding (x, y)) apply in that order. x right, y up, (0, 0) bottom-left; tile 0 erases, n draws tileset cell n-1. "
     "Every operation is checked before any applies. preview: true returns the map as text rows, top row first.",
     R"json({"type":"object","properties":{"path":{"type":"string","description":"Path under assets/"},"guid":{"type":"string","description":"\"guid:<uuid>\" instead of path"},"clear":{"type":"integer","minimum":0},"cells":{"type":"array","items":{"type":"array","items":{"type":"integer"},"minItems":3,"maxItems":3}},"rects":{"type":"array","items":{"type":"array","items":{"type":"integer"},"minItems":5,"maxItems":5}},"fills":{"type":"array","items":{"type":"array","items":{"type":"integer"},"minItems":3,"maxItems":3}},"preview":{"type":"boolean","default":false}}})json",
     tilemap_paint},
};

}  // namespace

Span<const ToolDef> asset_tool_table() { return Span<const ToolDef>(kAssetTools, sizeof(kAssetTools) / sizeof(kAssetTools[0])); }

void write_asset_schemas(JsonWriter& w) {
    w.begin_object().key("types").begin_array();
    for (const AssetKind& k : kKinds) {
        w.begin_object();
        w.key("type").str(k.type).key("extension").str(k.ext).key("format").str(k.format).key("version").num_u(k.version);
        w.key("summary").str(k.summary).key("fields").raw(k.fields).key("template").raw(k.tmpl);
        w.end_object();
    }
    w.end_array().end_object();
}

bool asset_tool_arg(ToolCall& c, const editor::ProjectAsset*& out) { return arg_asset(c, out); }
bool asset_rel_valid(StringView rel) { return valid_rel(rel); }
cook::String asset_guid_ref(asset::Guid g) { return guid_ref(g); }
bool asset_validate_and_write(ToolCall& c, StringView rel, asset::Guid guid, StringView text) {
    return validate_and_write(c, rel, guid, text);
}

}  // namespace mce::mcp
