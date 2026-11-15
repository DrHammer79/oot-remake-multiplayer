// SPDX-License-Identifier: GPL-3.0-or-later
// The game builds co-op knows, and where things are in each one's main.
// docs/game-builds.md says what every entry is and how to add a build.
//
// Every main offset the readers use is in this table: the hook's build check
// (coop_hook.cpp) reads it, and the readers' own constants (coop_puppet.h
// addr, coop_world.h, coop_svc.h, coop/host/src/addresses.hpp, ...) are
// taken from the primary entry at compile time, so a value lives in one
// place. MainOffsets are the ones the signature scan searches
// (coop_game_scan.h); ReaderOffsets are the rest, filled by hand for a new
// build. tests/lichii/game_build_test.cpp pins every 1.1.1 value. Moving the
// readers from kPrimary to the entry the hook found is the next step
// (docs/game-builds.md, one-time steps).
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace CoopGameBuild {

// Offsets into main. Pointers are global slots that hold a pointer; calls are
// functions the cave or the hook calls; patches are instructions the loader
// rewrites (CoopSvc::PatchMain).
struct MainOffsets {
    // Global pointer slots.
    std::uint64_t actor_manager_ptr;   // CoopPuppet::addr::kActorManagerPtr
    std::uint64_t game_flags_ptr;      // coop_flags.cpp, game_flags.cpp kManagerPtr
    std::uint64_t game_ui_module_ptr;  // addresses.hpp kGameUiModulePtr
    std::uint64_t scene_module_ptr;    // addresses.hpp kSceneModulePtr, coop_pose.cpp
    std::uint64_t banc_module_ptr;     // addresses.hpp kBancModulePtr
    std::uint64_t event_manager_ptr;   // addresses.hpp kEventManagerPtr
    std::uint64_t world_mgr_ptr;       // coop_time.cpp kWorldMgrPtr
    std::uint64_t foliage_module_ptr;  // CoopWorld::kFoliageModule
    // Data.
    std::uint64_t actor_vtable;        // CoopPuppet::addr::kActorVtable
    std::uint64_t got_create_default;  // CoopPuppet::addr::kGotCreateDefault
    // Functions.
    std::uint64_t create_actor;        // kCreate
    std::uint64_t malloc;              // kMalloc
    std::uint64_t request_delete;      // kRequestDelete
    std::uint64_t take_created;        // kTakeCreated
    std::uint64_t drop_create;         // kDropCreate
    std::uint64_t play_as;             // kPlayAs
    std::uint64_t set_as_frame;        // kSetAsFrame
    std::uint64_t sync_as_slot;        // kSyncAsSlot
    std::uint64_t equip;               // kEquip
    std::uint64_t equip_change_state;  // kEquipChangeState
    std::uint64_t unequip_armor;       // kUnequipArmor
    std::uint64_t set_matrix;          // kSetMatrix
    std::uint64_t set_time;            // coop_time.cpp kSetTime
    std::uint64_t cut_foliage;         // CoopWorld::kCutFoliage
    // Code patches. The caves (coop_svc.s, coop_hold.s) are linked at these
    // addresses and branch to frame_step, so a new build needs them
    // re-linked, not only new numbers.
    std::uint64_t frame_step;          // the call the svc hooks replace
    std::uint64_t svc_hook_rare;       // CoopSvc::kHooks[0]
    std::uint64_t svc_hook_frame;      // CoopSvc::kHooks[1]
    std::uint64_t svc_cave;            // CoopSvc::kCaveOffset
    std::uint64_t cave_end;            // CoopSvc::kCaveEnd (rodata starts)
    std::uint64_t hold_hook;           // CoopHold::kHookOffset
    std::uint64_t hold_cave;           // CoopHold::kCaveOffset
    std::uint64_t hold_table;          // CoopHold::kTableOffset
};

// The other offsets into main the readers use. The scan does not search them
// (no patterns yet): a new build fills them by hand. Kept apart from
// MainOffsets because the scan and make_patterns.py take every MainOffsets
// field as a pattern.
struct ReaderOffsets {
    // Global slots and data.
    std::uint64_t controller_manager_ptr;     // coop_local_input.h, coop_local_probe.h
    std::uint64_t renderer_ptr;               // coop_local_view.h kRenderer, coop_local_probe.h kLayers
    std::uint64_t challenge_manager_ptr;      // coop_quest.h kChallengeMgr
    std::uint64_t event_request_got;          // coop_live_events.h kManagerPtr: [[main + this]]
    std::uint64_t layer_owner_got;            // coop_local_layer.h kOwnerSlot
    std::uint64_t layer_heap_got;             // coop_local_layer.h kHeapSlot
    std::uint64_t sound_root_got;             // coop_puppet_sound.h kSoundRootGot
    std::uint64_t ocarina_module_vtable_got;  // coop_puppet_sound.h kOcarinaModuleVtableGot
    std::uint64_t look_at_camera_vtable;      // coop_local_probe.h kLookAtCamera
    std::uint64_t counter_node_vtable;        // coop_counters.h kNodeVtable
    std::uint64_t set_bool_node_vtable;       // coop_lift_ride.h kVtable
    std::uint64_t block_node_vtable;          // coop_world.h BlockNode::kVtable
    std::uint64_t block_node_vtable2;         // coop_world.h BlockNode::kVtable2
    std::uint64_t event_string_vtable;        // coop_live_events.h kStringVtable
    std::uint64_t title_screen_vtable;        // hookshot/src/coop_title.cpp kTitleVtable
    // Strings in rodata.
    std::uint64_t text_main;                  // coop_local_layer.h kSceneMain, "Main"
    std::uint64_t text_game_scene;            // coop_local_layer.h kGameScene
    std::uint64_t text_master_field;          // coop_local_layer.h kMasterField
    std::uint64_t text_3d_0;                  // coop_local_layer.h kName
    // Functions.
    std::uint64_t force_skip;                 // coop_boss_game.cpp kForceSkip
    std::uint64_t set_brain_verb;             // coop_boss_native.h kSetBrainVerb
    std::uint64_t request_action_verb;        // coop_boss_native.h kRequestActionVerb
    std::uint64_t set_bool_node_execute;      // coop_lift_ride.h kExecute
    std::uint64_t xlink_search_asset;         // coop_link_sound.h, coop_puppet_sound.h, coop_spawn.cpp
    std::uint64_t xlink_emit;                 // coop_link_sound.h, coop_puppet_sound.h, coop_spawn.cpp
    std::uint64_t xlink_kill;                 // coop_puppet_sound.h kKill
    std::uint64_t event_arg_build;            // coop_live_events.h kArgBuild
    std::uint64_t event_request;              // coop_live_events.h kRequest
    std::uint64_t string_free;                // coop_live_events.h kStringFree
    std::uint64_t event_set_link;             // coop_live_events.h kSetLink
    std::uint64_t performer_request;          // coop_live_events.h kPerformerRequest
    std::uint64_t layer_maker;                // coop_local_layer.h kMaker
    std::uint64_t layer_set_shown;            // coop_local_layer.h kSetShown
    std::uint64_t layer_bind;                 // coop_local_layer.h kBind
    std::uint64_t equip_state;                // coop_puppet.h kEquipState
    std::uint64_t challenge_set_step;         // coop_quest.h kSetStep
    std::uint64_t transceiver_send;           // coop_scrubs.h kSend
    std::uint64_t block_node_start;           // coop_world.h BlockNode::kStartFn
    std::uint64_t msg_ref_init;               // hookshot/src/coop_title.cpp kMsgRefInit
    std::uint64_t msg_ref_set;                // hookshot/src/coop_title.cpp kMsgRefSet
    std::uint64_t pane_text;                  // hookshot/src/coop_title.cpp kPaneText
    std::uint64_t find_pane;                  // hookshot/src/coop_map_page.h kFindPane
    // Return addresses the hooks compare.
    std::uint64_t buttons_off_return;         // coop_player_input.h kButtonsOffReturn
    std::uint64_t shoot_create_return;        // hookshot/src/coop_hookshot.cpp kShootCreateReturn
    // The AI counter patch (coop_counters.h): the hooked start and its cave.
    std::uint64_t counter_start;              // kStartOffset
    std::uint64_t counter_cave;               // kCaveOffset
    std::uint64_t counter_ring;               // kRingPointer
};

// Field offsets inside the game's objects that more than one reader uses.
// They may well be the same in another build, which is not proven: a new
// build checks them by hand (docs/game-builds.md, step 3). Each field's
// readers keep a constant of their own name set from here.
struct StructOffsets {
    // The actor (docs/puppet.md, docs/objects.md).
    std::uint64_t actor_state;            // u32, 4 = alive
    std::uint64_t actor_name;             // the actor name (char*)
    std::uint64_t actor_components;       // the component table (kActorComponents, kActorParts)
    std::uint64_t actor_position;         // 3 x f32
    std::uint64_t actor_rotation;         // 9 x f32, rows of 3
    std::uint64_t actor_placement;        // the placement it came from
    std::uint64_t actor_node;             // the actor's own node in the manager's ring
    std::uint64_t placement_hash;         // placement -> its u64 hash
    // The actor manager's ring of actors.
    std::uint64_t manager_lists;          // manager -> lists
    std::uint64_t manager_list_head;      // lists + this: the ring head
    std::uint64_t manager_list_count;     // the ring count
    std::uint64_t manager_node_owner;     // node -> its actor
    // The component table and the AS (animation) component.
    std::uint64_t components_as;          // components -> AS
    std::uint64_t components_life;        // components -> Life
    std::uint64_t as_slot_count;          // AS: slot count
    std::uint64_t as_slot_array;          // AS: slot array
    std::uint64_t as_slot_stride;         // one array entry (kAsSlotStride, kAsSlotEntry)
    std::uint64_t as_slot_rate;           // slot: f32 rate
    std::uint64_t as_slot_frame;          // slot: f32 frame
    // The Life component and its last hit.
    std::uint64_t life_value;             // kLifeValue, kLifeValueEntry
    std::uint64_t life_last_hit_id;
    std::uint64_t life_hit_header;
    std::uint64_t life_hit_amount;
    std::uint64_t life_hit_record;
    // The EquipmentUser and its links (a link -> holder -> actor).
    std::uint64_t equip_slots;
    std::uint64_t equip_slot_stride;
    std::uint64_t equip_slot_holder;
    std::uint64_t equip_slot_id;
    std::uint64_t equip_slot_bits;        // kEquipSlotBits, kEquipSlotState
    std::uint64_t equip_armor_links;
    std::uint64_t link_holder;            // link -> holder
    std::uint64_t link_id;                // link: u32 id
    std::uint64_t holder_actor;           // holder -> actor
    std::uint64_t holder_id;              // holder: u32, equals the link id
    // The event manager's running session.
    std::uint64_t event_session;          // set while an event runs
    std::uint64_t session_count;          // u32
    std::uint64_t session_events;         // -> event pointers
    std::uint64_t session_current;        // u32
    // The game flags manager's key records (docs/flags.md).
    std::uint64_t flags_key_count;
    std::uint64_t flags_key_records;
    std::uint64_t flags_key_record_size;
    std::uint64_t flags_key_value;
    // Others.
    std::uint64_t xlink_user_resource;    // xlink user -> resource
    std::uint64_t xlink_resource_accessor;// resource -> accessor
    std::uint64_t scene_table;            // Scene module -> table
    std::uint64_t scene_table_array;      // table -> array
    std::uint64_t scene_player_manager;   // array slot of the player manager
    std::uint64_t logic_holder_actor;     // an AI/logic node's holder -> actor (kHolderActor, kOwnerActor)
    std::uint64_t renderer_layer_count;   // u32
    std::uint64_t renderer_layer_array;   // layer pointers, by id
};

struct Build {
    std::string_view id;       // first 8 bytes of the build id, lower-case hex
    std::string_view main_id;  // main's full build id, upper-case hex (loader)
    std::string_view version;  // the game's display version
    MainOffsets main;
    ReaderOffsets reader;
    StructOffsets fields;
};

inline constexpr std::array<Build, 1> kBuilds{{
    {"e0f3ccfaf50065ae", "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348", "1.1.1",
     MainOffsets{
         .actor_manager_ptr = 0x4e91888,
         .game_flags_ptr = 0x4e842a8,
         .game_ui_module_ptr = 0x4e7cfa8,
         .scene_module_ptr = 0x4e97648,
         .banc_module_ptr = 0x4e919d0,
         .event_manager_ptr = 0x4e94840,
         .world_mgr_ptr = 0x4e7eab0,
         .foliage_module_ptr = 0x4e22540,
         .actor_vtable = 0x4b25e18,
         .got_create_default = 0x4c7dad0,
         .create_actor = 0x372a70,
         .malloc = 0x14c6c90,
         .request_delete = 0xa6490,
         .take_created = 0x36fbd0,
         .drop_create = 0x36fde0,
         .play_as = 0x2022a0,
         .set_as_frame = 0x37b5d70,
         .sync_as_slot = 0x3abec10,
         .equip = 0x2df7b20,
         .equip_change_state = 0xe34c40,
         .unequip_armor = 0x10aceb0,
         .set_matrix = 0x4c6580,
         .set_time = 0x34522d0,
         .cut_foliage = 0xb80800,
         .frame_step = 0x3b4e300,
         .svc_hook_rare = 0x6e6dd8,
         .svc_hook_frame = 0x6e70f8,
         .svc_cave = 0x3c46280,
         .cave_end = 0x3c47000,
         .hold_hook = 0x369df8,
         .hold_cave = 0x3c46300,
         .hold_table = 0x3c46400,
     },
     ReaderOffsets{
         .controller_manager_ptr = 0x4e92f10,
         .renderer_ptr = 0x4e81cf0,
         .challenge_manager_ptr = 0x4e29950,
         .event_request_got = 0x4c7e090,
         .layer_owner_got = 0x4c7e880,
         .layer_heap_got = 0x4c7e740,
         .sound_root_got = 0x4c7e678,
         .ocarina_module_vtable_got = 0x4c8bfd0,
         .look_at_camera_vtable = 0x4bf8a88,
         .counter_node_vtable = 0x499f200,
         .set_bool_node_vtable = 0x49a9550,
         .block_node_vtable = 0x49ae6f8,
         .block_node_vtable2 = 0x49ae8b8,
         .event_string_vtable = 0x4974b20,
         .title_screen_vtable = 0x4bf06e8,
         .text_main = 0x3dc7946,
         .text_game_scene = 0x3d00948,
         .text_master_field = 0x3caa19d,
         .text_3d_0 = 0x3f085b5,
         .force_skip = 0x3b3af30,
         .set_brain_verb = 0x6733b0,
         .request_action_verb = 0xc08f0,
         .set_bool_node_execute = 0x2727540,
         .xlink_search_asset = 0x43db00,
         .xlink_emit = 0x440630,
         .xlink_kill = 0x7cc120,
         .event_arg_build = 0xd79f10,
         .event_request = 0xd7a820,
         .string_free = 0x20b6f0,
         .event_set_link = 0x2bc440,
         .performer_request = 0x4aa890,
         .layer_maker = 0x1174620,
         .layer_set_shown = 0x11745c0,
         .layer_bind = 0x336f5a0,
         .equip_state = 0xce7ab0,
         .challenge_set_step = 0x2cf54a0,
         .transceiver_send = 0x2838c50,
         .block_node_start = 0x2751790,
         .msg_ref_init = 0xa66f70,
         .msg_ref_set = 0xa6afe0,
         .pane_text = 0xa67d30,
         .find_pane = 0x76e2b0,
         .buttons_off_return = 0x279cdf8,
         .shoot_create_return = 0xc17300,
         .counter_start = 0x5e6760,
         .counter_cave = 0x3c46700,
         .counter_ring = 0x3c46780,
     },
     StructOffsets{
         .actor_state = 0x2c,
         .actor_name = 0x260,
         .actor_components = 0x270,
         .actor_position = 0x31c,
         .actor_rotation = 0x328,
         .actor_placement = 0x3d8,
         .actor_node = 0x538,
         .placement_hash = 0x88,
         .manager_lists = 0x118,
         .manager_list_head = 0x670,
         .manager_list_count = 0x10,
         .manager_node_owner = 0x10,
         .components_as = 0x40,
         .components_life = 0x2b8,
         .as_slot_count = 0x18,
         .as_slot_array = 0x20,
         .as_slot_stride = 0x48,
         .as_slot_rate = 0xd4,
         .as_slot_frame = 0xe8,
         .life_value = 0x1c10,
         .life_last_hit_id = 0x1b18,
         .life_hit_header = 0x1f8,
         .life_hit_amount = 0x224,
         .life_hit_record = 0x240,
         .equip_slots = 0x20,
         .equip_slot_stride = 0x18,
         .equip_slot_holder = 0x8,
         .equip_slot_id = 0x10,
         .equip_slot_bits = 0x13bc,
         .equip_armor_links = 0xe0,
         .link_holder = 0x8,
         .link_id = 0x10,
         .holder_actor = 0x40,
         .holder_id = 0x48,
         .event_session = 0x68,
         .session_count = 0xeb0,
         .session_events = 0xeb8,
         .session_current = 0xf18,
         .flags_key_count = 0xc58,
         .flags_key_records = 0xc60,
         .flags_key_record_size = 0x10,
         .flags_key_value = 0xc,
         .xlink_user_resource = 0x58,
         .xlink_resource_accessor = 0x18,
         .scene_table = 0x1f0,
         .scene_table_array = 0x58,
         .scene_player_manager = 0x78,
         .logic_holder_actor = 0x1f0,
         .renderer_layer_count = 0x20,
         .renderer_layer_array = 0x28,
     }},
}};

// The readers take their offsets from kPrimary at compile time. Remove this
// when they take them from the entry the hook found.
static_assert(kBuilds.size() == 1,
              "readers still use the primary entry: docs/game-builds.md, one-time steps");

// The build co-op was made on; HELLO names it.
inline constexpr const Build& kPrimary = kBuilds[0];

// What the readers' constants are set from: the primary build's entry.
inline constexpr const MainOffsets& kMain = kPrimary.main;
inline constexpr const ReaderOffsets& kReader = kPrimary.reader;
inline constexpr const StructOffsets& kFields = kPrimary.fields;

// Where the recomps (Hookshot, Longshot) load main in guest memory
// (hos_coop_api.h main_base): module code names a game function by its guest
// address, kRecompMain + its main offset.
inline constexpr std::uint64_t kRecompMain = 0x80020000;

// `build` as hex, either case: the 8-byte id or main's whole one. Null when
// co-op does not know that build.
inline const Build* Find(std::string_view build) {
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };
    for (const auto& b : kBuilds) {
        if (same(build, b.id) || same(build, b.main_id)) {
            return &b;
        }
    }
    return nullptr;
}

// What co-op works with, for messages: "1.1.1 (build e0f3ccfaf50065ae)".
inline std::string Supported() {
    std::string out;
    for (const auto& b : kBuilds) {
        if (!out.empty()) {
            out += " or ";
        }
        out += std::string(b.version) + " (build " + std::string(b.id) + ")";
    }
    return out;
}

// The panel line when the running game is another build. Short: it is drawn
// over the game.
inline std::string PanelRefusal(std::string_view running) {
    return "Co-op off: game build " + std::string(running.substr(0, 16)) +
           " not supported, needs version " + std::string(kPrimary.version);
}

// The log line, once per game start: what runs, what co-op needs, what to do.
inline std::string LogRefusal(std::string_view running) {
    return "game build " + std::string(running.substr(0, 16)) +
           " is not supported; co-op needs version " + Supported() +
           ". Install that game update (docs/game-builds.md); nothing is read or sent until then";
}

} // namespace CoopGameBuild
