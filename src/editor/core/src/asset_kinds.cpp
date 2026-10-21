#include "mce/editor/asset_kinds.h"

#include <cstring>

namespace mce::editor {
namespace {

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
{"name":"height_scale","type":"number","default":0,"help":"0..0.5: parallax occlusion mapping depth in texture repeats (0.02..0.1 typical), needs textures.height; surfaces appear to have depth when seen at an angle (bricks, cobbles), faded out with distance by the quality tier, off on Low"},
{"name":"parallax_steps","type":"[min, max]","default":[8,32],"help":"4..64: march steps looking straight on and at a grazing angle"},
{"name":"textures","type":"object","help":"{\"base_color\", \"normal\", \"metallic_roughness\", \"emissive\", \"height\"}: \"guid:<Texture>\" each, all optional; height is a grey image (white is high) imported with srgb false, the same size as the normal map"}])json",
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
    {"RoadNetwork", ".mcroads", "mce-roads", 1,
     "A road network (M-OPENWORLD B1): junction 'nodes' and the 'roads' between them. The cook builds every lane, the "
     "connectors across each junction (straight on from the matching lane, turns towards the kerb from the outermost "
     "lane, across traffic from the innermost), who yields (control) and the traffic-light phases.",
     R"json([
{"name":"name","type":"string"},
{"name":"drive","type":"string","default":"right","help":"\"right\" or \"left\": the side traffic keeps to"},
{"name":"lane_width","type":"number","default":3.5,"help":"Metres, the default for every road"},
{"name":"speed","type":"number","default":13.9,"help":"m/s limit, the default for every road"},
{"name":"nodes","type":"array","help":"{\"name\", \"at\": [x, y, z], \"control\": \"none\" | \"priority\" | \"stop\" | \"signal\" (default: priority where 3+ roads meet, else none), \"radius\": metres the lanes stop short (default from the roads)}"},
{"name":"roads","type":"array","help":"{\"name\", \"from\": node, \"to\": node, \"lanes\": n each way | [forward, backward], \"points\": [[x, y, z], ...] shape between the nodes, \"speed\", \"lane_width\", \"sidewalk\": metres per side (0), \"priority\": 0-15 (higher goes first at priority junctions)}"},
{"name":"signals","type":"array","help":"{\"node\", \"phases\": [{\"green\": [\"road\" | \"road>road\"], \"seconds\"}], \"amber\": 3, \"all_red\": 1, \"offset\": 0}; every turn at the node must be green in some phase"},
{"name":"no_turns","type":"array","help":"{\"node\", \"from\": road, \"to\": road}: turns the cook leaves out"}])json",
     R"json({"name":"roads","nodes":[{"name":"a","at":[0,0,0]},{"name":"b","at":[100,0,0]}],"roads":[{"name":"main","from":"a","to":"b","lanes":1}]})json"},
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
    {"ConvarDefaults", ".mcdefaults", "mce-defaults", 1,
     "The project's own convar defaults (M-WORKFLOW 7), cooked into the pack for every platform. A value is true/false, "
     "a number or text; names are 1-63 letters, digits, '_', '-', '.'. A saved option and a command-line value still win.",
     R"json([
{"name":"convars","type":"object","help":"convar name -> true/false, a number or text, e.g. {\"ui.reference_height\": 720}"}])json",
     R"json({"convars":{}})json"},
    {"ItemCatalog", ".mcitems", "mce-items", 1,
     "The project's items, loot tables and recipes (M-INVENTORY 1, M-CRAFT 1): item definitions (id, name, stack size, weight in kg, tags, named "
     "numeric stats, icon and prefab), weighted loot tables that roll items or other tables, and crafting recipes (1-8 distinct inputs to 1-8 outputs, "
     "an optional station tag, seconds). Ids are 1-63 letters, digits, '_', '-', '.'; tables may nest 8 deep and may not loop.",
     R"json([
{"name":"items","type":"array","help":"[{\"id\":\"medkit\",\"name\":\"Medkit\",\"stack\":5,\"weight\":0.4,\"tags\":[\"consumable\"],\"stats\":{\"heal\":25},\"icon\":\"guid:<texture>\",\"prefab\":\"guid:<prefab>\"}]"},
{"name":"tables","type":"array","help":"[{\"id\":\"chest\",\"rolls\":[1,3],\"entries\":[{\"item\":\"medkit\",\"weight\":5,\"qty\":[1,2]},{\"table\":\"rare\",\"weight\":1},{\"item\":\"key\",\"guaranteed\":true}]}]"},
{"name":"recipes","type":"array","help":"[{\"id\":\"bandage\",\"name\":\"Bandage\",\"station\":\"workbench\",\"time\":1.5,\"in\":[{\"item\":\"cloth\",\"qty\":2}],\"out\":[{\"item\":\"medkit\",\"qty\":1}]}]"}])json",
     R"json({"items":[],"tables":[],"recipes":[]})json"},
};

}  // namespace

Span<const AssetKind> asset_kinds() { return Span<const AssetKind>(kKinds, sizeof(kKinds) / sizeof(kKinds[0])); }

const AssetKind* asset_kind_for_extension(StringView path) {
    const usize dot = path.rfind('.');
    if (dot == StringView::npos) return nullptr;
    const StringView ext = path.substr(dot);
    for (const AssetKind& k : kKinds)
        if (ext == k.ext) return &k;
    return nullptr;
}

const AssetKind* asset_kind_named(StringView name) {
    for (const AssetKind& k : kKinds)
        if (name == k.type || name == k.ext || (name.size() + 1 == std::strlen(k.ext) && name == StringView(k.ext + 1)))
            return &k;
    return nullptr;
}

}  // namespace mce::editor
