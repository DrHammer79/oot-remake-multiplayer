// Tests for the table of game builds (lichii/hook/coop_game_build.h): the
// lookup, the refusal text, and that the table agrees with the constants the
// readers use today, so a reader can move onto the table without a change.
#include <doctest/doctest.h>

#include <ostream>
#include <string>

#include "../../coop/host/src/addresses.hpp"
#include "coop_fairy.h"
#include "coop_game_build.h"
#include "coop_hold.h"
#include "coop_puppet.h"
#include "coop_svc.h"
#include "coop_world.h"

using CoopGameBuild::Find;

namespace {

// Where a `bl` at `at` goes.
std::uint64_t BlTarget(std::uint64_t at, std::uint32_t word) {
    std::int64_t imm = word & 0x3ffffff;
    if (imm & 0x2000000) {
        imm -= 0x4000000;
    }
    return at + static_cast<std::uint64_t>(imm * 4);
}

} // namespace

TEST_CASE("game build: the known build is found by either id, in either case") {
    const auto* b = Find("e0f3ccfaf50065ae");
    REQUIRE(b != nullptr);
    CHECK(b->version == "1.1.1");
    CHECK(Find("E0F3CCFAF50065AE") == b);
    CHECK(Find("E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348") == b);
    CHECK(Find("e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348") == b);
    CHECK(&CoopGameBuild::kPrimary == b);
}

TEST_CASE("game build: other builds and broken ids are not found") {
    CHECK(Find("") == nullptr);
    CHECK(Find("e0f3ccfaf500") == nullptr);
    CHECK(Find("0000000000000000") == nullptr);
    CHECK(Find("e0f3ccfaf50065af") == nullptr);
    CHECK(Find("e0f3ccfaf50065ae00") == nullptr);  // neither length
    CHECK(Find("E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3349") == nullptr);
}

TEST_CASE("game build: the refusal names the running build and the one needed") {
    const std::string panel = CoopGameBuild::PanelRefusal("0123456789abcdef");
    CHECK(panel.find("0123456789abcdef") != std::string::npos);
    CHECK(panel.find("1.1.1") != std::string::npos);
    CHECK(panel.size() <= 80);  // one line of the panel
    const std::string log = CoopGameBuild::LogRefusal("0123456789abcdef");
    CHECK(log.find("0123456789abcdef") != std::string::npos);
    CHECK(log.find("1.1.1 (build e0f3ccfaf50065ae)") != std::string::npos);
    CHECK(CoopGameBuild::Supported() == "1.1.1 (build e0f3ccfaf50065ae)");
}

TEST_CASE("game build: the table matches the full build id the loader patch wants") {
    // coop_svc.cpp's kMainBuild is file-local; it is this string.
    CHECK(CoopGameBuild::kPrimary.main_id == "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348");
    CHECK(CoopGameBuild::kPrimary.main_id.substr(0, 16) == "E0F3CCFAF50065AE");
}

TEST_CASE("game build: the table agrees with the readers' constants") {
    const auto& m = CoopGameBuild::kPrimary.main;
    namespace p = CoopPuppet::addr;
    namespace h = coop::host::addr;
    CHECK(m.actor_manager_ptr == p::kActorManagerPtr);
    CHECK(m.actor_manager_ptr == CoopFairy::addr::kManagerPtr);
    CHECK(m.game_ui_module_ptr == h::kGameUiModulePtr);
    CHECK(m.scene_module_ptr == h::kSceneModulePtr);
    CHECK(m.banc_module_ptr == h::kBancModulePtr);
    CHECK(m.event_manager_ptr == h::kEventManagerPtr);
    CHECK(m.foliage_module_ptr == CoopWorld::kFoliageModule);
    CHECK(m.actor_vtable == p::kActorVtable);
    CHECK(m.got_create_default == p::kGotCreateDefault);
    CHECK(m.create_actor == p::kCreate);
    CHECK(m.malloc == p::kMalloc);
    CHECK(m.request_delete == p::kRequestDelete);
    CHECK(m.take_created == p::kTakeCreated);
    CHECK(m.drop_create == p::kDropCreate);
    CHECK(m.play_as == p::kPlayAs);
    CHECK(m.set_as_frame == p::kSetAsFrame);
    CHECK(m.sync_as_slot == p::kSyncAsSlot);
    CHECK(m.equip == p::kEquip);
    CHECK(m.equip_change_state == p::kEquipChangeState);
    CHECK(m.unequip_armor == p::kUnequipArmor);
    CHECK(m.set_matrix == p::kSetMatrix);
    CHECK(m.cut_foliage == CoopWorld::kCutFoliage);
    // The loader patches.
    CHECK(m.svc_hook_rare == CoopSvc::kHooks[0].offset);
    CHECK(m.svc_hook_frame == CoopSvc::kHooks[1].offset);
    CHECK(m.svc_cave == CoopSvc::kCaveOffset);
    CHECK(m.cave_end == CoopSvc::kCaveEnd);
    CHECK(m.hold_hook == CoopHold::kHookOffset);
    CHECK(m.hold_cave == CoopHold::kCaveOffset);
    CHECK(m.hold_table == CoopHold::kTableOffset);
    // Both hooked calls go to frame_step in the unpatched game.
    for (const auto& hook : CoopSvc::kHooks) {
        CHECK(BlTarget(hook.offset, hook.original) == m.frame_step);
    }
    // The caves sit in main's zero padding, before rodata.
    CHECK(m.svc_cave < m.hold_cave);
    CHECK(m.hold_cave < m.hold_table);
    CHECK(m.hold_table < m.cave_end);
    // The file-local constants (coop_flags.cpp, coop_time.cpp, ...) are set
    // from the table too; the next test pins every value.
}

TEST_CASE("game build: every offset the readers use keeps its 1.1.1 value") {
    // The readers' constants were these literals before they took them from
    // the table (v1 step 2): a pure move, the same numbers.
    using CoopGameBuild::kMain;
    using CoopGameBuild::kReader;
    using CoopGameBuild::kRecompMain;
    CHECK(kMain.actor_manager_ptr == 0x4e91888);
    CHECK(kMain.actor_vtable == 0x4b25e18);
    CHECK(kMain.banc_module_ptr == 0x4e919d0);
    CHECK(kMain.cave_end == 0x3c47000);
    CHECK(kMain.create_actor == 0x372a70);
    CHECK(kMain.cut_foliage == 0xb80800);
    CHECK(kMain.drop_create == 0x36fde0);
    CHECK(kMain.equip == 0x2df7b20);
    CHECK(kMain.equip_change_state == 0xe34c40);
    CHECK(kMain.event_manager_ptr == 0x4e94840);
    CHECK(kMain.foliage_module_ptr == 0x4e22540);
    CHECK(kMain.game_flags_ptr == 0x4e842a8);
    CHECK(kMain.game_ui_module_ptr == 0x4e7cfa8);
    CHECK(kMain.got_create_default == 0x4c7dad0);
    CHECK(kMain.hold_cave == 0x3c46300);
    CHECK(kMain.hold_hook == 0x369df8);
    CHECK(kMain.hold_table == 0x3c46400);
    CHECK(kMain.malloc == 0x14c6c90);
    CHECK(kMain.play_as == 0x2022a0);
    CHECK(kMain.request_delete == 0xa6490);
    CHECK(kMain.scene_module_ptr == 0x4e97648);
    CHECK(kMain.set_as_frame == 0x37b5d70);
    CHECK(kMain.set_matrix == 0x4c6580);
    CHECK(kMain.set_time == 0x34522d0);
    CHECK(kMain.svc_cave == 0x3c46280);
    CHECK(kMain.svc_hook_frame == 0x6e70f8);
    CHECK(kMain.svc_hook_rare == 0x6e6dd8);
    CHECK(kMain.sync_as_slot == 0x3abec10);
    CHECK(kMain.take_created == 0x36fbd0);
    CHECK(kMain.unequip_armor == 0x10aceb0);
    CHECK(kMain.world_mgr_ptr == 0x4e7eab0);
    CHECK(kReader.block_node_start == 0x2751790);
    CHECK(kReader.block_node_vtable == 0x49ae6f8);
    CHECK(kReader.block_node_vtable2 == 0x49ae8b8);
    CHECK(kReader.buttons_off_return == 0x279cdf8);
    CHECK(kReader.challenge_manager_ptr == 0x4e29950);
    CHECK(kReader.challenge_set_step == 0x2cf54a0);
    CHECK(kReader.controller_manager_ptr == 0x4e92f10);
    CHECK(kReader.counter_cave == 0x3c46700);
    CHECK(kReader.counter_node_vtable == 0x499f200);
    CHECK(kReader.counter_ring == 0x3c46780);
    CHECK(kReader.counter_start == 0x5e6760);
    CHECK(kReader.equip_state == 0xce7ab0);
    CHECK(kReader.event_arg_build == 0xd79f10);
    CHECK(kReader.event_request == 0xd7a820);
    CHECK(kReader.event_request_got == 0x4c7e090);
    CHECK(kReader.event_set_link == 0x2bc440);
    CHECK(kReader.event_string_vtable == 0x4974b20);
    CHECK(kReader.force_skip == 0x3b3af30);
    CHECK(kReader.layer_bind == 0x336f5a0);
    CHECK(kReader.layer_heap_got == 0x4c7e740);
    CHECK(kReader.layer_maker == 0x1174620);
    CHECK(kReader.layer_owner_got == 0x4c7e880);
    CHECK(kReader.layer_set_shown == 0x11745c0);
    CHECK(kReader.look_at_camera_vtable == 0x4bf8a88);
    CHECK(kReader.ocarina_module_vtable_got == 0x4c8bfd0);
    CHECK(kReader.performer_request == 0x4aa890);
    CHECK(kReader.renderer_ptr == 0x4e81cf0);
    CHECK(kReader.request_action_verb == 0xc08f0);
    CHECK(kReader.set_bool_node_execute == 0x2727540);
    CHECK(kReader.set_bool_node_vtable == 0x49a9550);
    CHECK(kReader.set_brain_verb == 0x6733b0);
    CHECK(kReader.shoot_create_return == 0xc17300);
    CHECK(kReader.sound_root_got == 0x4c7e678);
    CHECK(kReader.string_free == 0x20b6f0);
    CHECK(kReader.text_3d_0 == 0x3f085b5);
    CHECK(kReader.text_game_scene == 0x3d00948);
    CHECK(kReader.text_main == 0x3dc7946);
    CHECK(kReader.text_master_field == 0x3caa19d);
    CHECK(kReader.transceiver_send == 0x2838c50);
    CHECK(kReader.xlink_emit == 0x440630);
    CHECK(kReader.xlink_kill == 0x7cc120);
    CHECK(kReader.xlink_search_asset == 0x43db00);
    CHECK(kRecompMain + kReader.find_pane == 0x8078e2b0);
    CHECK(kRecompMain + kReader.msg_ref_init == 0x80a86f70);
    CHECK(kRecompMain + kReader.msg_ref_set == 0x80a8afe0);
    CHECK(kRecompMain + kReader.pane_text == 0x80a87d30);
    CHECK(kRecompMain + kReader.title_screen_vtable == 0x84c106e8);
    CHECK(kMain.frame_step == 0x3b4e300);
}

TEST_CASE("game build: a new field in the tables is pinned above") {
    // A field added without a pin fails here: add its CHECK above and here.
    CHECK(sizeof(CoopGameBuild::MainOffsets) / sizeof(std::uint64_t) == 32);
    CHECK(sizeof(CoopGameBuild::ReaderOffsets) / sizeof(std::uint64_t) == 47);
}

TEST_CASE("game build: the recomps' guest addresses are main's") {
    // Hookshot and Longshot name a game function by kRecompMain + its offset
    // (hookshot/src/coop_title.cpp, coop_map_page.h): sead::LookAtCamera's
    // vtable is main+0x4bf8a88 and 0x84c18a88 there (coop_local_probe.h).
    CHECK(CoopGameBuild::kRecompMain + CoopGameBuild::kReader.look_at_camera_vtable == 0x84c18a88);
}
