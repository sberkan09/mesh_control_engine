// The shop demo on the phones (M-MONETIZE demo, device half; SBA-26): the stock player with purchases
// and ads turned on, playing this sample's pack (assets/store.mcstore and an empty scene). There is no
// shop UI: the requests come from the player's -monetize_test script, passed at launch
// (user guide 22.6), so the phone's real store and ad network answer the same steps the fakes do:
//
//   xcrun simctl launch <sim> org.meshcontrol.sample_shop_demo -monetize_test "30:query 60:buy:gems_100"
//   adb shell am start -n org.meshcontrol.shop_demo/.MceActivity --es mce_args '-monetize_test 30:query'
#include "mce/player/player.h"

int main(int argc, char** argv) {
    mce::player::PlayerDesc desc;
    desc.app_name = "shop_demo";
    desc.title = "MCE - Shop Demo";
    desc.pack = "assets.mce";
    desc.pack_fallback = MCE_SAMPLE_DEV_PACK;
    desc.monetize = true;
    return mce::player::run(argc, argv, desc);
}
